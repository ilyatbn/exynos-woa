"""gen-video-ddi.py OUT_DIR COMPILE_COMMANDS_JSON - generate the D3D11 video DDI glue for Mesa d3d10umd (G17,
notes/g17-dxva-mfc.md V6a): for D3DWDDM2_0DDI_VIDEODEVICEFUNCS (RetrieveSubObject ID 3) and D3D11_1DDI_VIDEODEVICEFUNCS
(ID 2) every member is assigned either a hand-written implementation Vid_<member> (the H.264 decoder subset, in
Video.cpp) or a generated stub. Stubs: VOID functions whose first parameter is the device report E_NOTIMPL through
SetError (the D3D11 video DDI reports errors that way - MS Learn pfnSetErrorCb), SIZE_T ones return a pointer size,
HRESULT ones E_NOTIMPL, BOOL ones FALSE. Writes video_stubs.inc, video_fill_wddm2_0.inc, video_fill_11_1.inc.
Run again when the tables or the WDK change; output committed with the Mesa patch, this script with exynos-woa.
"""
import json
import os
import re
import subprocess
import sys

out = sys.argv[1]
cc = json.load(open(sys.argv[2]))

# the decoder subset implemented by hand in Video.cpp (member name without "pfn")
HAND = {
    'GetVideoDecoderProfileCount', 'GetVideoDecoderProfile', 'CheckVideoDecoderFormat', 'GetVideoDecoderConfigCount',
    'GetVideoDecoderConfig', 'GetVideoDecoderBufferTypeCount', 'GetVideoDecoderBufferInfo',
    'CalcPrivateVideoDecoderSize', 'CreateVideoDecoder', 'DestroyVideoDecoder', 'VideoDecoderBeginFrame',
    'VideoDecoderEndFrame', 'VideoDecoderSubmitBuffers', 'CalcPrivateVideoDecoderOutputViewSize',
    'CreateVideoDecoderOutputView', 'DestroyVideoDecoderOutputView', 'GetContentProtectionCaps',
    'VideoDecoderExtension', 'VideoDecoderGetHandle', 'VideoDecoderSubmitBuffers1', 'QueryVideoCapabilities',
}


def preprocessed():
    e = next(x for x in cc if x['file'].replace(chr(92), '/').endswith('d3d10umd/D3D11.cpp'))
    probe = os.path.join(out, '_pp_probe.cpp')
    open(probe, 'w').write('#include "DriverIncludes.h"' + chr(10))
    cmd = e['command']
    cmd = re.sub(r'\s/Fo\S+|\s-o\s*\S+|\s/Fd\S+|\s/c\b|\s-c\b|\s/showIncludes\b|\s/Fp\S+', ' ', cmd)
    src = e['file']
    cmd = cmd.replace(src, probe) if src in cmd else cmd + ' ' + probe
    r = subprocess.run(cmd + ' /E', shell=True, cwd=e['directory'], capture_output=True, text=True, errors='replace')
    os.remove(probe)
    if r.returncode != 0 or 'D3DWDDM2_0DDI_VIDEODEVICEFUNCS' not in r.stdout:
        raise SystemExit('preprocess failed: ' + r.stderr[-2000:])
    return re.sub(r'^#.*$', '', r.stdout, flags=re.M)


hdr = preprocessed()


def table(name):
    m = re.search(r'typedef struct ' + name + r'\s*\{(.*?)\}\s*' + name + r'\s*;', hdr, re.S)
    body = re.sub(r'//[^\n]*', '', m.group(1))
    return re.findall(r'(PFND3D\w+)\s+(pfn\w+)\s*;', body)


def typedef(pfn):
    m = re.search(r'typedef\s+([\w\s]+?)\s*\(\s*(?:APIENTRY|__stdcall)?\s*\*\s*' + pfn + r'\s*\)\s*\((.*?)\)\s*;',
                  hdr, re.S)
    if not m:
        raise SystemExit('no typedef for ' + pfn)
    return ' '.join(m.group(1).split()), ' '.join(re.sub(r'//[^\n]*', '', m.group(2)).split())


stubs, done = [], set()
for tname, fname in (('D3DWDDM2_0DDI_VIDEODEVICEFUNCS', 'video_fill_wddm2_0.inc'),
                     ('D3D11_1DDI_VIDEODEVICEFUNCS', 'video_fill_11_1.inc')):
    lines = []
    for pfn_t, name in table(tname):
        m = name[3:]
        if m in HAND:
            lines.append('   f->%s = Vid_%s;' % (name, m))
            continue
        fn = 'VidStub_' + m + '_' + pfn_t[4:].split('DDI')[0]   # per-table name: types can differ per table
        if fn not in done:
            ret, params = typedef(pfn_t)
            ret = {'void': 'VOID'}.get(ret, ret)
            first = params.split(',')[0].strip()
            if ret == 'VOID':
                body = ('   LOG_UNSUPPORTED_ENTRYPOINT();\n   SetError(%s, E_NOTIMPL);' % first.split()[-1]
                        if first.startswith('D3D10DDI_HDEVICE') else '   LOG_UNSUPPORTED_ENTRYPOINT();')
            elif ret == 'SIZE_T':
                body = '   LOG_UNSUPPORTED_ENTRYPOINT();\n   return sizeof(void *);'
            elif ret == 'HRESULT':
                body = '   LOG_UNSUPPORTED_ENTRYPOINT();\n   return E_NOTIMPL;'
            elif ret == 'BOOL':
                body = '   LOG_UNSUPPORTED_ENTRYPOINT();\n   return FALSE;'
            else:
                raise SystemExit('return type %s of %s' % (ret, pfn_t))
            # parameters may be unnamed in the WDK typedefs: name them p0..pN
            ps = []
            for i, p in enumerate(x.strip() for x in params.split(',')):
                if p in ('', 'void', 'VOID'):
                    continue
                toks = p.replace('*', ' * ').split()
                named = (len(toks) > 1 and toks[-1] != '*' and toks[-1] not in ('const', 'CONST') and
                         not (len(toks) == 2 and toks[0] in ('const', 'CONST')))
                ps.append(p if named else '%s p%d' % (p, i))
            if body.find('SetError(') >= 0:
                dev = ps[0].split()[-1]
                body = '   LOG_UNSUPPORTED_ENTRYPOINT();\n   SetError(%s, E_NOTIMPL);' % dev
            body = '   malikm_tlog("video stub %s");\n' % fn + body   # which stub the runtime calls (V6a debugging)
            stubs.append('static %s APIENTRY\n%s(%s)\n{\n%s\n}\n' % (ret, fn, ', '.join(ps) or 'void', body))
            done.add(fn)
        lines.append('   f->%s = %s;' % (name, fn))
    open(os.path.join(out, fname), 'w', newline='\n').write(
        '/* Generated by exynos-woa tools/mesa-build/gen-video-ddi.py - do not edit. */\n' + '\n'.join(lines) + '\n')
open(os.path.join(out, 'video_stubs.inc'), 'w', newline='\n').write(
    '/* Generated by exynos-woa tools/mesa-build/gen-video-ddi.py from the WDK d3d10umddi.h - do not edit. */\n\n'
    + '\n'.join(stubs))
print('%d stubs, hand-written: %s' % (len(stubs), ' '.join(sorted(HAND))))

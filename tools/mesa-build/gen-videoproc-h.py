"""gen-videoproc-h.py D3D10UMD_DIR - write VideoProc.h (prototypes of the Vid_* video processor members defined in
VideoProc.cpp, G17 V6c, notes/g17-dxva-mfc.md) and print the member names for the HAND set of gen-video-ddi.py.
Run after changing a VideoProc.cpp signature; the output is committed with the Mesa patch.
"""
import re
import sys

d = sys.argv[1]
src = open(d + '/VideoProc.cpp', newline='').read().replace('\r\n', '\n')
protos, names = [], []
for m in re.finditer(r'^(SIZE_T|HRESULT|VOID) APIENTRY\n(Vid_\w+)\((.*?)\)\n\{', src, re.M | re.S):
    params = ' '.join(m.group(3).split())
    protos.append('%s APIENTRY %s(%s);' % (m.group(1), m.group(2), params))
    names.append(m.group(2)[4:])
hdr = ('/* exynos-woa G17 V6c: the video processor members implemented in VideoProc.cpp (generated from it by\n'
       ' * exynos-woa tools/mesa-build/gen-videoproc-h.py - do not edit). */\n'
       '#pragma once\n\n' + '\n'.join(protos) + '\n')
open(d + '/VideoProc.h', 'w', newline='\n').write(hdr)
print(len(protos))
print(' '.join("'%s'," % n for n in names))

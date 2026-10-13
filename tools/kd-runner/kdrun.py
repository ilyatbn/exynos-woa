"""Unattended kernel-debugger session for the S10e (KDNET over USB EEM, notes/m17-usb-kd.md).

Starts kd.exe as the KDNET host, logs everything, and:
  - on any "kd>" prompt (a bugcheck break, an assertion, or our own break-in) runs the analysis commands once;
  - if no prompt arrived --break-after seconds after the target connected ("Kernel Debugger connection
    established"), sends CTRL_BREAK to kd (break into a hung target, or a timed look at a booted one).
usage: python -I kdrun.py LOGFILE [--break-after 240] [--total 600] [--cmds "cmd1;cmd2"]
"""
import os
import signal
import subprocess
import sys
import threading
import time

KD = r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe'
DEFAULT_CMDS = ['.symfix+ C:\\symbols', '.reload', '!analyze -v', 'r', 'kb 40', '!running -ti', '!cpuinfo', 'lm o', 'q']


def main():
    log = sys.argv[1]
    break_after = 240
    total = 600
    cmds = DEFAULT_CMDS
    a = sys.argv
    if '--break-after' in a:
        break_after = int(a[a.index('--break-after') + 1])
    if '--total' in a:
        total = int(a[a.index('--total') + 1])
    if '--cmds' in a:
        cmds = a[a.index('--cmds') + 1].split(';')
    env = dict(os.environ)
    env['_NT_SYMBOL_PATH'] = r'srv*C:\symbols*https://msdl.microsoft.com/download/symbols'
    p = subprocess.Popen([KD, '-k', 'net:port=50005,key=1.2.3.4', '-logo', log], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                         creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    state = {'prompt': 0, 'out': '', 'sent': False, 'connected': None}

    def reader():
        buf = ''
        while True:
            ch = p.stdout.read(1)
            if not ch:
                break
            buf += ch.decode('latin1')
            if state['connected'] is None and buf.endswith('connection established'):
                state['connected'] = time.time()
            if buf.endswith('kd> '):
                state['prompt'] += 1
                buf = ''
            if len(buf) > 4096:
                buf = buf[-200:]

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    t0 = time.time()
    broke = False
    while p.poll() is None and time.time() - t0 < total:
        if state['prompt'] and not state['sent']:
            state['sent'] = True
            for c in cmds:
                p.stdin.write((c + '\n').encode())
                p.stdin.flush()
                time.sleep(4 if c.startswith('!analyze') or c == '.reload' else 1.5)
        if not broke and not state['prompt'] and state['connected'] and time.time() - state['connected'] > break_after:
            broke = True
            p.send_signal(signal.CTRL_BREAK_EVENT)
        time.sleep(1)
    if p.poll() is None:
        try:
            p.stdin.write(b'q\n')
            p.stdin.flush()
            time.sleep(3)
        except OSError:
            pass
        p.kill()
    print('kd done; prompts', state['prompt'], 'break-in sent', broke)


if __name__ == '__main__':
    main()

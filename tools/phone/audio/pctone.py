"""Play a test tone on the PC speakers for the phone's microphone test (notes/d8-mic.md).

usage: python -I pctone.py SECONDS [--freq 1000] [--amp 0.15] [--delay 0]
Quiet by default (amplitude 0.15 of full scale, 20 ms fades) - the user's room, not a lab.
"""
import argparse
import time

import numpy as np
import sounddevice as sd

FS = 48000
ap = argparse.ArgumentParser()
ap.add_argument('seconds', type=float)
ap.add_argument('--freq', type=float, default=1000.0)
ap.add_argument('--amp', type=float, default=0.15)
ap.add_argument('--delay', type=float, default=0.0)
a = ap.parse_args()
n = int(FS * a.seconds)
t = np.arange(n) / FS
x = (a.amp * np.sin(2 * np.pi * a.freq * t)).astype(np.float32)
f = int(FS * 0.02)
x[:f] *= np.linspace(0, 1, f, dtype=np.float32)
x[-f:] *= np.linspace(1, 0, f, dtype=np.float32)
time.sleep(a.delay)
print(f'{time.strftime("%H:%M:%S")} tone {a.freq:.0f} Hz amp {a.amp} for {a.seconds} s', flush=True)
sd.play(x, FS, blocking=True)
print(f'{time.strftime("%H:%M:%S")} tone done', flush=True)

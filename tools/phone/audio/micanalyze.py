"""Analyse a phone microphone recording (Exynos9820Audio MicSelfTest raw file: int32 LE, --ch interleaved slots, 48 kHz).

usage: python -I micanalyze.py FILE.raw [--freq 1000] [--skip 0.2]
Prints per channel: RMS (dBFS), the 5 strongest spectral peaks, and the tone band (+-10 Hz) against the median of the
surrounding 200 Hz (SNR in dB). notes/d8-mic.md.
"""
import argparse

import numpy as np

FS = 48000
ap = argparse.ArgumentParser()
ap.add_argument('file')
ap.add_argument('--freq', type=float, default=1000.0)
ap.add_argument('--skip', type=float, default=0.2, help='seconds dropped at the start (mic bias / HPF settling)')
ap.add_argument('--float', action='store_true', help='float32 samples (wascap.exe shared-mode recordings)')
ap.add_argument('--ch', type=int, default=4, help='interleaved int32 channels in the file (driver 1.0.2.6+: 4 frame slots)')
a = ap.parse_args()
x = (np.fromfile(a.file, dtype='<f4').reshape(-1, a.ch).astype(np.float64) if a.float else
     np.fromfile(a.file, dtype='<i4').reshape(-1, a.ch).astype(np.float64) / 2**31)
x = x[int(a.skip * FS):]
print(f'{a.file}: {len(x)} frames = {len(x) / FS:.2f} s after skipping {a.skip} s')
for ch in range(a.ch):
    s = x[:, ch] - x[:, ch].mean()
    rms = np.sqrt(np.mean(s * s))
    w = np.hanning(len(s))
    spec = np.abs(np.fft.rfft(s * w)) / (w.sum() / 2)
    freqs = np.fft.rfftfreq(len(s), 1 / FS)
    db = 20 * np.log10(spec + 1e-12)
    band = (freqs > 50)
    top = np.argsort(db * band)[::-1]
    peaks, used = [], []
    for i in top:
        if freqs[i] < 50 or any(abs(freqs[i] - u) < 30 for u in used):
            continue
        peaks.append(f'{freqs[i]:.0f} Hz {db[i]:.1f} dB')
        used.append(freqs[i])
        if len(peaks) == 5:
            break
    t = np.abs(freqs - a.freq) <= 10
    around = (np.abs(freqs - a.freq) <= 200) & ~(np.abs(freqs - a.freq) <= 30)
    snr = db[t].max() - np.median(db[around])
    print(f'ch{ch}: rms {20 * np.log10(rms + 1e-12):.1f} dBFS, peak {20 * np.log10(np.abs(s).max() + 1e-12):.1f} dBFS; '
          f'{a.freq:.0f} Hz band {db[t].max():.1f} dB, SNR vs surroundings {snr:.1f} dB')
    print(f'      strongest: ' + ', '.join(peaks))

"""Listen through the PC microphone for test tones from the phone (D8 audio, unattended runs).

The Windows-level mic endpoint on this PC is muted (WASAPI/MME return exact zeros), but the raw WDM-KS
pin still delivers the hardware signal, so this opens the WDM-KS "Microphone Array" device by name.
Nothing is saved to disk: only per-100 ms levels and the detected tone bursts are printed.

usage: python -I listen.py SECONDS [--selftest]
  --selftest  plays a 1 kHz tone on the PC speakers after 1 s (proves the mic path works)
Output: one line per detected burst: start s, duration s, dominant Hz, level dBFS, SNR dB.
"""
import queue
import sys

import numpy as np
import sounddevice as sd

FS = 48000
FRAME = FS // 10


def find_mic():
    for i, d in enumerate(sd.query_devices()):
        api = sd.query_hostapis(d['hostapi'])['name']
        if d['max_input_channels'] > 0 and 'WDM-KS' in api and 'Microphone Array' in d['name']:
            return i, min(2, d['max_input_channels'])
    raise SystemExit('no WDM-KS Microphone Array input found')


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
    selftest = '--selftest' in sys.argv
    dev, ch = find_mic()
    q = queue.Queue()
    with sd.InputStream(samplerate=FS, channels=ch, dtype='float32', device=dev,
                        callback=lambda d, f, t, s: q.put(d.copy())):
        if selftest:
            sd.sleep(1000)
            t = np.arange(FS) / FS
            sd.play((0.3 * np.sin(2 * np.pi * 1000 * t)).astype('float32'), FS)
            sd.sleep(int(secs * 1000) - 1000)
        else:
            sd.sleep(int(secs * 1000))
    x = np.concatenate(list(q.queue)).mean(axis=1)
    frames = len(x) // FRAME
    w = np.hanning(FRAME)
    f = np.fft.rfftfreq(FRAME, 1 / FS)
    band = (f > 150) & (f < 12000)
    peak_db, peak_hz = [], []
    for i in range(frames):
        X = np.abs(np.fft.rfft(x[i * FRAME:(i + 1) * FRAME] * w)) / (w.sum() / 2)
        k = np.argmax(np.where(band, X, 0))
        peak_db.append(20 * np.log10(X[k] + 1e-12))
        peak_hz.append(f[k])
    peak_db = np.array(peak_db)
    floor = np.median(peak_db)
    on = peak_db > floor + 15
    on[:5] = False   # the first 0.5 s holds the capture start-up transient
    print(f'mic dev {dev}, {frames} frames, noise floor (median peak) {floor:.0f} dBFS')
    bursts = 0
    i = 0
    while i < frames:
        if on[i]:
            j = i
            while j < frames and on[j]:
                j += 1
            if j - i >= 2:   # >= 200 ms: ignore clicks
                hz = np.median(peak_hz[i:j])
                lvl = peak_db[i:j].max()
                print(f'BURST start {i / 10:.1f} s dur {(j - i) / 10:.1f} s ~{hz:.0f} Hz peak {lvl:.0f} dBFS SNR {lvl - floor:.0f} dB')
                bursts += 1
            i = j
        else:
            i += 1
    print(f'BURSTS {bursts}')


if __name__ == '__main__':
    main()

"""Listen through the PC microphone for test tones from the phone (D8 audio, unattended runs).

The Windows-level mic endpoint on this PC is muted (WASAPI/MME return exact zeros), but the raw WDM-KS
pin still delivers the hardware signal, so this opens the WDM-KS "Microphone Array" device by name.
Nothing is saved to disk: only narrowband levels at the requested frequencies and the detected bursts.

usage: python -I listen.py SECONDS [--freqs 1000,1500,...] [--selftest]
  --freqs     target tones (Hz); default 1000,1500,3000,6000
  --selftest  plays a 1 kHz tone on the PC speakers after 1 s (proves the mic path works)
Output: per frequency, bursts where the +-15 Hz band rises >= 10 dB over its own median for >= 300 ms.
"""
import queue
import sys

import numpy as np
import sounddevice as sd

FS = 48000
FRAME = FS // 10   # 100 ms -> 10 Hz bins


def find_mic():
    for i, d in enumerate(sd.query_devices()):
        api = sd.query_hostapis(d['hostapi'])['name']
        if d['max_input_channels'] > 0 and 'WDM-KS' in api and 'Microphone Array' in d['name']:
            return i, min(2, d['max_input_channels'])
    raise SystemExit('no WDM-KS Microphone Array input found')


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
    freqs = [1000, 1500, 3000, 6000]
    if '--freqs' in sys.argv:
        freqs = [float(v) for v in sys.argv[sys.argv.index('--freqs') + 1].split(',')]
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
    spec = np.array([np.abs(np.fft.rfft(x[i * FRAME:(i + 1) * FRAME] * w)) / (w.sum() / 2) for i in range(frames)])
    print(f'mic dev {dev}, {frames} frames')
    total = 0
    for hz in freqs:
        sel = (f >= hz - 15) & (f <= hz + 15)
        lvl = 20 * np.log10(spec[:, sel].max(axis=1) + 1e-12)
        floor = np.median(lvl)
        on = lvl > floor + 10
        on[:5] = False   # capture start-up transient
        i = 0
        found = 0
        while i < frames:
            if on[i]:
                j = i
                while j < frames and on[j]:
                    j += 1
                if j - i >= 3:
                    print(f'TONE {hz:.0f} Hz start {i / 10:.1f} s dur {(j - i) / 10:.1f} s peak {lvl[i:j].max():.0f} dBFS'
                          f' SNR {lvl[i:j].max() - floor:.0f} dB')
                    found += 1
                i = j
            else:
                i += 1
        print(f'{hz:.0f} Hz: floor {floor:.0f} dBFS, max {lvl[5:].max():.0f} dBFS, bursts {found}')
        total += found
    print(f'BURSTS {total}')


if __name__ == '__main__':
    main()

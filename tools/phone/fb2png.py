"""fb2png.py DUMP [PITCH] - a framebuffer dump (s10ediag dumpmmio 0xCA000000 ...) as PNG next to it.

notes/gpu-hw-path.md H4b: the S10e's scan-out is 1080 x 2280 B8G8R8X8, pitch 4352 when the KMD re-strided it
(4320 = the GOP layout). The PNG is the visible 1080 columns, plus a half-size copy (*-half.png) for quick looks.
"""
import sys

from PIL import Image

path = sys.argv[1]
pitch = int(sys.argv[2]) if len(sys.argv) > 2 else 4352
W, H = 1080, 2280
data = open(path, 'rb').read()
if len(data) < pitch * (H - 1) + W * 4:
    sys.exit('%s: %d bytes, too short for pitch %d' % (path, len(data), pitch))
rows = b''.join(data[y * pitch:y * pitch + W * 4] for y in range(H))
img = Image.frombuffer('RGBX', (W, H), rows, 'raw', 'BGRX', 0, 1).convert('RGB')
out = path.rsplit('.', 1)[0] + '.png'
img.save(out)
img.resize((W // 2, H // 2)).save(out[:-4] + '-half.png')
print(out)

"""recpack.py DIR NAME FPS - pack screenrec.ps1 frames (DIR/frame_NNNN.jpg, DIR/frames.txt) into DIR/NAME.gif (real-time
pace) and DIR/NAME-sheet.jpg (up to 24 evenly spaced frames with their time). notes/g27-x64-apps.md "Remote viewing"."""
import glob, os, re, sys
from PIL import Image, ImageDraw

d, name, fps = sys.argv[1], sys.argv[2], float(sys.argv[3])
times = {}
for line in open(os.path.join(d, 'frames.txt'), encoding='utf-8', errors='replace'):
    m = re.match(r'(\d{4})\s+(\d+) ms$', line.strip())
    if m:
        times[int(m.group(1))] = int(m.group(2))
files = sorted(glob.glob(os.path.join(d, 'frame_*.jpg')))
if not files:
    sys.exit('no frames')
frames = [Image.open(f).convert('RGB') for f in files]
idx = [int(re.search(r'frame_(\d+)', f).group(1)) for f in files]
durs = []
for k, i in enumerate(idx):
    nxt = times.get(idx[k + 1]) if k + 1 < len(idx) else None
    durs.append(max(20, (nxt - times[i]) if (nxt is not None and i in times) else int(1000 / fps)))
gif = os.path.join(d, name + '.gif')
pal = [f.quantize(colors=128, method=Image.Quantize.MEDIANCUT) for f in frames]
pal[0].save(gif, save_all=True, append_images=pal[1:], duration=durs, loop=0, optimize=True)
n = min(24, len(frames))
pick = [round(k * (len(frames) - 1) / max(1, n - 1)) for k in range(n)]
w, h = frames[0].size
tw, th = w // 2, h // 2
cols = 4
rows = (n + cols - 1) // cols
sheet = Image.new('RGB', (cols * tw, rows * (th + 16)), 'black')
dr = ImageDraw.Draw(sheet)
for k, p in enumerate(pick):
    x, y = (k % cols) * tw, (k // cols) * (th + 16)
    sheet.paste(frames[p].resize((tw, th)), (x, y + 16))
    dr.text((x + 4, y + 2), 't=%.1f s' % (times.get(idx[p], 0) / 1000), fill='white')
sheet.save(os.path.join(d, name + '-sheet.jpg'), quality=85)
print('%d frames -> %s (%d KB), %s-sheet.jpg' % (len(frames), gif, os.path.getsize(gif) // 1024, name))

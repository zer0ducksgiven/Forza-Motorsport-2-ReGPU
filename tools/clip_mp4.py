"""Side-by-side MP4 of two captures. Driving: aligned by the race timer; parked: frame by frame.
Usage: clip_mp4.py <a dir> <b dir> <out.mp4> <fps> <align 0|1> <left label> <right label>"""
import sys, glob
import numpy as np, imageio.v2 as iio
from PIL import Image, ImageDraw
a, b, out, fps, align, la, lb = sys.argv[1:8]
A = sorted(glob.glob(a + '/*.png')); B = sorted(glob.glob(b + '/*.png'))
def ld(f):
    im = Image.open(f).convert('RGB')  # FSR runs are 1600x900: bring to 1280x720 for alignment and layout
    return np.asarray(im if im.size == (1280, 720) else im.resize((1280, 720), Image.LANCZOS))
k = 0
if align == '1':
    crop = lambda f: ld(f)[45:80, 1030:1200].astype(np.float32)
    ta = [crop(f) for f in A]; tb = [crop(f) for f in B]
    best = None
    for kk in range(-40, 41):
        pr = [(i, i + kk) for i in range(len(A)) if 0 <= i + kk < len(B) and ta[i].mean() > 80 and tb[i + kk].mean() > 80]
        if len(pr) < 30: continue
        e = np.mean([np.abs(ta[i] - tb[j]).mean() for i, j in pr])
        if best is None or e < best[0]: best = (e, kk)
    k = best[1]
pairs = [(i, i + k) for i in range(len(A)) if 0 <= i + k < len(B)]
W, H = 960, 540
w = iio.get_writer(out, fps=float(fps), codec='libx264', quality=7, macro_block_size=8, ffmpeg_params=['-pix_fmt', 'yuv420p'])
for i, j in pairs:
    c = Image.new('RGB', (W * 2 + 8, H + 28), (12, 14, 13))
    c.paste(Image.fromarray(ld(A[i])).resize((W, H), Image.LANCZOS), (0, 28)); c.paste(Image.fromarray(ld(B[j])).resize((W, H), Image.LANCZOS), (W + 8, 28))
    d = ImageDraw.Draw(c); d.text((8, 7), la, fill=(230, 235, 232)); d.text((W + 16, 7), lb, fill=(230, 235, 232))
    w.append_data(np.asarray(c))
w.close()
print(out, 'offset', k, len(pairs), 'frames')

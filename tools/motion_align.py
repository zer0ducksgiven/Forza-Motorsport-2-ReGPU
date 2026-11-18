"""Aligns two motion captures by the race timer (HUD crop), reports how alike the aligned frames are, and can write a
side-by-side GIF of the aligned frames. Usage: motion_align.py a b [out.gif] [left label] [right label]"""
import sys, glob
import numpy as np
from PIL import Image, ImageDraw
def load(d):
    return [np.asarray(Image.open(f).convert('RGB')) for f in sorted(glob.glob(d + '/*.png'))]
A, B = load(sys.argv[1]), load(sys.argv[2])
crop = lambda f: f[45:80, 1030:1200].astype(np.float32)
ta, tb = [crop(f) for f in A], [crop(f) for f in B]
best = None
for k in range(-60, 61):
    pairs = [(i, i + k) for i in range(len(A)) if 0 <= i + k < len(B) and ta[i].mean() > 80 and tb[i + k].mean() > 80]
    if len(pairs) < 30: continue
    e = np.mean([np.abs(ta[i] - tb[j]).mean() for i, j in pairs])
    if best is None or e < best[0]: best = (e, k, pairs)
e, k, pairs = best
full = np.mean([np.abs(A[i].astype(np.float32) - B[j].astype(np.float32)).mean() for i, j in pairs])
over = np.mean([(np.abs(A[i].astype(np.int16) - B[j].astype(np.int16)).max(2) > 8).mean() for i, j in pairs]) * 100
print(f"offset {k}, timer diff {e:.2f}, {len(pairs)} aligned frames, mean diff {full:.2f}/255, pixels over 8: {over:.2f}%")
if len(sys.argv) > 3:
    w = 480; h = 270; fr = []
    la = sys.argv[4] if len(sys.argv) > 4 else 'A'; lb = sys.argv[5] if len(sys.argv) > 5 else 'B'
    for i, j in pairs:
        c = Image.new('RGB', (w * 2 + 4, h))
        c.paste(Image.fromarray(A[i]).resize((w, h), Image.LANCZOS), (0, 0)); c.paste(Image.fromarray(B[j]).resize((w, h), Image.LANCZOS), (w + 4, 0))
        fr.append(c.quantize(255))
    fr[0].save(sys.argv[3], save_all=True, append_images=fr[1:], duration=83, loop=0, optimize=True)

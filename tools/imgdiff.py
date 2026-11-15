"""Compare two screenshots: share of differing pixels, mean and max difference; writes a side-by-side + diff image.
Usage: py -3 tools/imgdiff.py a.png b.png out.png"""
import sys
from PIL import Image, ImageChops

a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB").resize(a.size)
d = ImageChops.difference(a, b)
px = list(d.getdata())
n = len(px)
diff = sum(1 for p in px if max(p) > 8)
mean = sum(sum(p) for p in px) / (3 * n)
print(f"pixels over 8/255: {100 * diff / n:.2f}%  mean diff {mean:.2f}/255  max {max(max(p) for p in px)}")
w, h = a.size
out = Image.new("RGB", (w * 3, h))
out.paste(a, (0, 0)); out.paste(b, (w, 0)); out.paste(d.point(lambda v: min(255, v * 8)), (w * 2, 0))
out.thumbnail((2400, 2400))
out.save(sys.argv[3])

"""Side-by-side animated GIF of two frame sequences (for eyeballing flicker and motion): flicker_gif.py a b out.gif"""
import sys, glob
from PIL import Image
a = sorted(glob.glob(sys.argv[1] + '/*.png')); b = sorted(glob.glob(sys.argv[2] + '/*.png'))
frames = []
for fa, fb in zip(a, b):
    ia = Image.open(fa).convert('RGB').resize((640, 360)); ib = Image.open(fb).convert('RGB').resize((640, 360))
    c = Image.new('RGB', (1280, 360)); c.paste(ia, (0, 0)); c.paste(ib, (640, 0)); frames.append(c.quantize(256))
frames[0].save(sys.argv[3], save_all=True, append_images=frames[1:], duration=33, loop=0)

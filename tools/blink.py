"""Blink detector for parked captures (16 Nov 2026). tools/flicker.py counts one-frame blips; this counts pixels that
vary over the 60 frames (std > 4) where the stock capture is steady (std < 1), in the whole frame and in the box around
the player's car, and the dominant period of the car pixels. Found the Potato car-highlight blink (6 frames on, 6 off).
Usage (repo root): blink.py <capture dir> [...]   (reference: out/fx/static/native)"""
import sys, glob, numpy as np
from PIL import Image
W = np.array([.299, .587, .114], np.float32)
CAR = (slice(360, 670), slice(430, 850))  # rows, columns around the player's car at 1280x720


def lum(d):
    return np.stack([np.asarray(Image.open(f).convert('RGB')) for f in sorted(glob.glob(d + '/*.png'))]).astype(np.float32) @ W


sn = lum('out/fx/static/native').std(0)
for d in sys.argv[1:]:
    if len(glob.glob(d + '/*.png')) < 60: continue
    p = lum(d)
    if p.shape[1:] != sn.shape: print('%-30s different size' % d); continue
    m = (p.std(0) > 4) & (sn < 1); car = m[CAR]; t = p[:, CAR[0], CAR[1]][:, car]
    per = 0
    if car.sum() >= 20:
        f = np.abs(np.fft.rfft(t - t.mean(0), axis=0)).mean(1); k = int(np.argmax(f[1:]) + 1); per = 60 / k
    print('%-30s unstable px %5d   on the car %5d   car period %4.1f frames' % (d, m.sum(), car.sum(), per), flush=True)

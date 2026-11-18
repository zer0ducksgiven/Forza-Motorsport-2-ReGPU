"""Flicker study: finds pixels that flicker (A-B-A: change, then return) over a run of consecutive frames.
Usage: flicker.py <dir with 000.png..> [reference dir] [out.png]
Prints events per frame (whole image and per screen region) and, with a reference run, the pixels that flicker
in this run but not in the reference (excess), and writes a heat map: the first frame dimmed, flickering pixels
red (excess) or yellow (also in the reference)."""
import sys, os, glob
import numpy as np
from PIL import Image

T = 24          # a luminance step this big (of 255) counts as a change
RET = 8         # back within this much of the frame before: a return
MIN_EVENTS = 3  # pixels with at least this many A-B-A events count as flickering


def fit(im):
    # FSR runs output 1600x900: compare at 1280x720 like the native runs
    return im if im.size == (1280, 720) else im.resize((1280, 720), Image.BILINEAR)


def load(d):
    files = sorted(glob.glob(os.path.join(d, '*.png')))
    fr = [np.asarray(fit(Image.open(f).convert('RGB')), dtype=np.float32) for f in files]
    return np.stack(fr)


def events(fr):
    y = fr[..., 0] * 0.299 + fr[..., 1] * 0.587 + fr[..., 2] * 0.114
    a, b, c = y[:-2], y[1:-1], y[2:]
    ev = (np.abs(b - a) > T) & (np.abs(c - a) < RET) & (np.abs(c - b) > T)
    return ev.sum(0), len(fr) - 2


def regions(mask):
    h, w = mask.shape
    out = []
    for name, (y0, y1, x0, x1) in {'top': (0, h // 3, 0, w), 'middle': (h // 3, 2 * h // 3, 0, w), 'bottom': (2 * h // 3, h, 0, w)}.items():
        out.append(f"{name} {100.0 * mask[y0:y1, x0:x1].mean():.3f}%")
    return ' '.join(out)


def main():
    d = sys.argv[1]
    ref = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != '-' else None
    outp = sys.argv[3] if len(sys.argv) > 3 else None
    fr = load(d)
    cnt, n = events(fr)
    flick = cnt >= MIN_EVENTS
    line = f"{os.path.basename(d.rstrip('/\\'))}: {cnt.sum() / n:.0f} events/frame, flickering px {100.0 * flick.mean():.3f}%"
    if ref:
        rc, rn = events(load(ref))
        rflick = rc >= 1
        excess = flick & ~rflick
        line += f", excess vs ref {100.0 * excess.mean():.3f}% ({int(excess.sum())} px) [{regions(excess)}]"
    else:
        excess = flick
    print(line)
    if outp:
        img = fr[0] * 0.35
        if ref:
            img[flick & rflick] = [255, 220, 0]
        img[excess] = [255, 0, 0]
        Image.fromarray(img.clip(0, 255).astype(np.uint8)).save(outp)


main()

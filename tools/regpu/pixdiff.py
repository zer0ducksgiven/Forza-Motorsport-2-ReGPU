#!/usr/bin/env python3
"""pixdiff.py A.png B.png [diff.png]: how two screenshots differ (pixels changed, by how much, PSNR). Needs numpy +
Pillow."""
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB')).astype(np.int16)
if a.shape != b.shape:
    sys.exit(f"sizes differ: {a.shape} vs {b.shape}")
d = np.abs(a - b).max(axis=2)
n = d.size
changed = int((d > 0).sum()); over8 = int((d > 8).sum()); over32 = int((d > 32).sum())
mse = float(((a - b) ** 2).mean())
psnr = float('inf') if mse == 0 else 10 * np.log10(255 ** 2 / mse)
print(f"changed {changed} px ({100*changed/n:.3f}%), >8: {over8} ({100*over8/n:.3f}%), >32: {over32} ({100*over32/n:.3f}%), "
      f"max {int(d.max())}, PSNR {psnr:.2f} dB")
if len(sys.argv) > 3:
    Image.fromarray(np.clip(d * 8, 0, 255).astype(np.uint8)).save(sys.argv[3])

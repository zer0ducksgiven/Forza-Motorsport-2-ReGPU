"""Flags captures whose frames are not the race (loading screen): the sky region's mean brightness, first and last frame."""
import sys, glob, os
import numpy as np
from PIL import Image
for d in sorted(glob.glob(sys.argv[1] + '/*/')):
    fs = sorted(glob.glob(d + '*.png'))
    m = [np.asarray(Image.open(f).convert('L'))[20:200, 300:700].mean() for f in (fs[0], fs[-1])]
    print(f"{os.path.basename(d[:-1]):24s} sky {m[0]:6.1f} {m[1]:6.1f} {'LOADING?' if min(m) < 60 else ''}")

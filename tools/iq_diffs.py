"""Pixel diff of each iq-<name>-2 screenshot against iq-native-2 (same moment, parked). Writes out/iq_diffs.json."""
import json, glob, re
from PIL import Image, ImageChops
ref = Image.open('out/bench_iq-native-2.png').convert('RGB')
res = {}
for f in sorted(glob.glob('out/bench_iq-*-2.png')):
    m = re.search(r'bench_iq-(.+)-2\.png$', f)
    if not m: continue
    n = m.group(1)
    if n == 'native': continue
    im = Image.open(f).convert('RGB').resize(ref.size)
    h = ImageChops.difference(ref, im).convert('L').histogram(); t = sum(h)
    res[n] = dict(mean=sum(i * c for i, c in enumerate(h)) / t, over8=sum(h[9:]) / t * 100)
json.dump(res, open('out/iq_diffs.json', 'w'), indent=1)
for n, v in sorted(res.items(), key=lambda kv: kv[1]['mean']): print('%-11s mean %5.2f  px>8 %5.1f%%' % (n, v['mean'], v['over8']))

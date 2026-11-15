"""Sums the last full FM2_GPU_PASSES report per profile: total GPU ms, scene vs post phase, temporal upscaler dispatch,
and the top passes. Usage: fsr_profile_sum.py name..."""
import re, sys, collections
for n in sys.argv[1:]:
    lines = [l for l in open(f'out/bench_fp-{n}.err', errors='replace') if 'gpu passes:' in l]
    # group by timestamp (one report per 2 s); take the last few reports and average
    by = collections.OrderedDict()
    for l in lines:
        t = l[1:l.index(']')].strip()
        by.setdefault(t[:-2], []).append(l)
    reps = list(by.values())[-6:-1]
    tot = collections.Counter(); scene = post = up = 0
    for rep in reps:
        for l in rep:
            m = re.search(r'gpu passes:\s+([\d.]+) ms\s+(\d+) draws\s+(scene|post)\s+(.*)$', l)
            if not m: continue
            ms = float(m.group(1)) / len(reps); lab = (m.group(3) + ' ' + m.group(4)).strip()
            tot[lab] += ms
            if m.group(3) == 'scene': scene += ms
            else: post += ms
            if 'temporal upscaler' in lab and 'after' not in lab: up += ms
    print(f'== {n}: total {scene + post:.1f} ms (scene {scene:.1f}, post {post:.1f}), temporal upscaler dispatch {up:.2f} ms')
    for lab, ms in tot.most_common(6): print(f'   {ms:6.2f}  {lab[:80]}')

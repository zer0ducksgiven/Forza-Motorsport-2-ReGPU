"""Medians per config for labels <pre>-<name>-<round>: median fps, 1% low, GPU ms, renderer (CPU) ms; % vs native."""
import re, statistics as st, sys, collections, json
pre = sys.argv[1]
d = collections.defaultdict(list)
for l in open('out/bench.txt'):
    m = re.match(r'%s-(\S+)-(\d)\s+race.*?1%% low\s+([\d.]+)\s+GPU\s+([\d.]+).*?renderer\s+([\d.]+).*median\s+([\d.]+)' % pre, l)
    if m and float(m.group(6)) < 150: d[m.group(1)].append((int(m.group(2)), float(m.group(6)), float(m.group(3)), float(m.group(4)), float(m.group(5))))
nat = d['native'] + d['native2']
med = lambda v, i: st.median([x[i] for x in v])
base = [med(nat, i) for i in range(1, 5)]
out = {}
for k, v in d.items():
    m = [med(v, i) for i in range(1, 5)]
    out[k] = dict(fps=m[0], low=m[1], gpu=m[2], cpu=m[3], gain=100 * (m[0] / base[0] - 1), dgpu=m[2] - base[2], dcpu=m[3] - base[3],
                  runs=[x[1] for x in sorted(v)])
json.dump(out, open('out/iq_stats.json', 'w'), indent=1)
for k, v in sorted(out.items(), key=lambda kv: -kv[1]['gain']):
    print('%-11s %5.1f fps %+6.1f%%  low %5.1f  gpu %6.2f (%+6.2f)  cpu %5.2f (%+5.2f)  %s' % (k, v['fps'], v['gain'], v['low'], v['gpu'], v['dgpu'], v['cpu'], v['dcpu'], ' '.join('%.1f' % x for x in v['runs'])))

"""Sizes the set of recompiled functions a run used (out/coverage.tsv from an FM2_COVERAGE build).

  py -3 tools/decomp/coverage.py title=out/cov_title.tsv race=out/cov_race.tsv ...

Each NAME=FILE is a run that went further than the one before it: a function's phase is the first run it shows up
in. Prints the count and code size per phase and per kind of function, and writes out/decomp/coverage.tsv: every function that ran,
with its phase, size, name and its level bottom-up (0 = calls nothing else that ran; n = calls only levels
below n; "cycle" = part of a recursion), the order replacements can be made in.
"""
import collections
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    runs = [a.partition('=')[::2] for a in argv[1:]]
    symbols = json.load(open(os.path.join(ROOT, 'out', 'decomp', 'symbols.json')))
    xrefs = json.load(open(os.path.join(ROOT, 'out', 'decomp', 'xrefs.json')))

    first = {}  # address -> (run index, ms)
    for i, (name, path) in enumerate(runs):
        for line in open(path):
            a, ms, _ = line.split('\t')
            first.setdefault(a, (i, int(ms)))

    def phase(hit):
        return runs[hit[0]][0]

    total_code = sum(s.get('size', 0) for s in symbols.values())
    print('%d functions ran, %d KB of code (of %d functions, %d KB)' % (
        len(first), sum(symbols.get(a, {}).get('size', 0) for a in first) // 1024, len(symbols), total_code // 1024))

    by_phase = collections.OrderedDict((n, [0, 0]) for n, _ in runs)
    for a, hit in first.items():
        p = by_phase[phase(hit)]
        p[0] += 1
        p[1] += symbols.get(a, {}).get('size', 0)
    print('\nnew in each phase:')
    for n, (count, size) in by_phase.items():
        print('  %-8s %6d functions %6d KB' % (n, count, size // 1024))

    kinds = collections.Counter()
    kind_size = collections.Counter()
    for a in first:
        s = symbols.get(a, {})
        k = s.get('lib') or s.get('kind', 'missing')
        kinds[k] += 1
        kind_size[k] += s.get('size', 0)
    print('\nby kind (library name for library code):')
    for k, c in kinds.most_common():
        print('  %-10s %6d functions %6d KB' % (k, c, kind_size[k] // 1024))

    # bottom-up levels over the call graph of the functions that ran
    ran = set(first)
    callees = {a: [c for c in xrefs.get(a, {}).get('calls', []) if c in ran and c != a] for a in ran}
    level = {}
    pending = set(ran)
    n = 0
    while True:
        ready = [a for a in pending if all(c in level for c in callees[a])]
        if not ready:
            break
        for a in ready:
            level[a] = n
            pending.discard(a)
        n += 1
    levels = collections.Counter(level.values())
    print('\nbottom-up levels: %s; %d in or above recursion' % (
        ', '.join('%d: %d' % (l, levels[l]) for l in sorted(levels)[:8]) + (', ... up to %d' % (n - 1) if n > 8 else ''),
        len(pending)))
    sizes = sorted(symbols.get(a, {}).get('size', 0) for a in ran)
    print('sizes: median %d bytes, 90%% under %d, largest %d' % (sizes[len(sizes) // 2], sizes[len(sizes) * 9 // 10], sizes[-1]))

    out = os.path.join(ROOT, 'out', 'decomp', 'coverage.tsv')
    with open(out, 'w') as f:
        f.write('address\tphase\tfirst_ms\tlevel\tsize\tkind\tname\n')
        for a in sorted(ran, key=lambda a: (level.get(a, 1 << 30), a)):
            s = symbols.get(a, {})
            f.write('%s\t%s\t%d\t%s\t%d\t%s\t%s\n' % (a, phase(first[a]), first[a][1], level.get(a, 'cycle'), s.get('size', 0),
                                                    s.get('lib') or s.get('kind', 'missing'), s.get('name', 'sub_' + a)))
    print('\nwrote', out)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

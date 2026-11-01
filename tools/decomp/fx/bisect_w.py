"""Finds the lifted functions that break boot: runs build/cov/fm2.exe with FM2_LIFT_ENABLE=<subset>."""
import os
import re
import subprocess
import sys

ROOT = os.environ.get('FM2_ROOT', r'D:\fm2-port')
SP = os.path.dirname(os.path.abspath(__file__))
runs = 0


TOUR = 'START,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,WAIT:4,QUIT'


def good(subset):
    """two windowed runs with the freeze watcher: good only if both finish"""
    global runs
    path = os.path.join(SP, 'enable.txt')
    with open(path, 'w') as f:
        f.write(chr(10).join(subset) + chr(10))
    for _ in range(int(os.environ.get('BISECT_TRIES', '2'))):
        runs += 1
        r = subprocess.run([sys.executable, os.path.join(SP, 'runwatch.py'), os.path.join(SP, 'bisect_run.log'), TOUR, path],
                           cwd=ROOT, capture_output=True, text=True)
        print('run %d: %d functions -> %s' % (runs, len(subset), r.stdout.strip()), flush=True)
        if not r.stdout.startswith('finished'):
            return False
    return True


def find(subset):
    """the bad functions in subset, which is known bad"""
    if len(subset) == 1:
        return subset
    half = len(subset) // 2
    a, b = subset[:half], subset[half:]
    ga, gb = good(a), good(b)
    bad = []
    if not ga:
        bad += find(a)
    if not gb:
        bad += find(b)
    if ga and gb:  # only together: keep a, find what in b breaks it with a
        bad += [x for x in find_with(a, b)]
    return bad


def find_with(base, subset):
    if len(subset) == 1:
        return subset
    half = len(subset) // 2
    a, b = subset[:half], subset[half:]
    if not good(base + a):
        return find_with(base, a)
    return find_with(base + a, b)


def main():
    if len(sys.argv) > 1:
        keep = open(sys.argv[1]).read().split()
    else:
        lifted = os.path.join(ROOT, 'out', 'lift')
        text = ''.join(open(os.path.join(lifted, f)).read() for f in os.listdir(lifted) if f.endswith('.cpp'))
        keep = re.findall(r'static void lifted_([0-9A-F]{8})', text)
    keep.sort()
    print(len(keep), 'functions')
    if good(keep):
        print('all good')
        return
    bad = find(keep)
    print('BAD:', ' '.join(bad))
    rest = [k for k in keep if k not in bad]
    print('without them:', 'good' if good(rest) else 'still bad')


main()

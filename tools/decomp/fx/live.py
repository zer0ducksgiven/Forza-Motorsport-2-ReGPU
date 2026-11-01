"""The routes live (lifted code running for real), each N times; coverage of each into lift/cov_<route><n>.tsv."""
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
ROOT = os.environ['FM2_ROOT']
L = os.path.join(ROOT, 'lift')
from checks_routes import ROUTES, PROFILE
n = int(sys.argv[1]) if len(sys.argv) > 1 else 2
for r in (sys.argv[2:] or ROUTES):
    for i in range(1, n + 1):
        p = subprocess.run([sys.executable, os.path.join(L, 'runwatch.py'), os.path.join(L, 'live_%s%d.log' % (r, i)), ROUTES[r], '-',
                            'FM2_PROFILE=' + PROFILE.get(r, 'Benchmark'), 'FM2_COVERAGE_FILE=' + os.path.join(L, 'cov_%s%d.tsv' % (r, i))],
                           cwd=ROOT, capture_output=True, text=True)
        print('live', r, i, p.stdout.strip(), flush=True)

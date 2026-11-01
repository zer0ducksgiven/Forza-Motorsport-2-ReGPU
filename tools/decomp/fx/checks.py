"""The checked routes (FM2_DECOMP_CHECK=lift) one after another; each route's lift_check.tsv into lift."""
import os, shutil, subprocess, sys
ROOT = os.environ.get('FM2_ROOT', os.getcwd())  # the game's folder (job.cmd sets it on the test PC)
HERE = os.path.dirname(os.path.abspath(__file__))
L = os.environ.get('FM2_LIFT_OUT', HERE)  # logs and results (on this PC: FM2_LIFT_OUT=out/fx_local)
ROUTES = {'race': 'START,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,WAIT:2,A,IDLE,WAIT:3,RT:25,WAIT:8,QUIT', 'menus': 'START,IDLE,A,IDLE,DOWN,IDLE,A,IDLE,A,IDLE,B,IDLE,DOWN,DOWN,IDLE,A,IDLE,A,IDLE,B,IDLE,B,IDLE,RIGHT,IDLE,A,IDLE,A,IDLE,DOWN,A,IDLE,A,IDLE,WAIT:3,B,IDLE,B,IDLE,WAIT:2,QUIT', 'career': 'START,IDLE,RIGHT,A,IDLE,RIGHT,A,IDLE,RIGHT,IDLE,RIGHT,IDLE,Y,IDLE,Y,IDLE,B,IDLE,RIGHT,A,IDLE,A,IDLE,A,IDLE,RIGHT,IDLE,RIGHT,IDLE,Y,IDLE,B,IDLE,B,IDLE,RIGHT,IDLE,A,IDLE,DOWN,A,IDLE,B,IDLE,B,IDLE,B,IDLE,RIGHT,A,IDLE,A,IDLE,RIGHT,IDLE,RIGHT,IDLE,B,IDLE,RIGHT,IDLE,A,IDLE,B,IDLE,B,IDLE,RB,IDLE,RT:0.3,IDLE,RT:0.3,IDLE,RT:0.3,IDLE,B,IDLE,LEFT,LEFT,LEFT,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,WAIT:3,A,IDLE,WAIT:3,RT:25,WAIT:5,QUIT', 'options': 'START,IDLE,RIGHT,RIGHT,RIGHT,A,IDLE,DOWN,IDLE,DOWN,IDLE,A,IDLE,B,IDLE,DOWN,A,IDLE,B,IDLE,DOWN,A,IDLE,B,IDLE,B,IDLE,LEFT,A,IDLE,WAIT:2,B,IDLE,WAIT:2,QUIT'}
PROFILE = {'career': 'LiftCareer', 'options': 'LiftCareer'}
args = sys.argv[1:]
tag = args.pop(0)[4:] if args and args[0].startswith('tag=') else ''
EXTRA = [x for x in args if '=' in x]
args = [x for x in args if '=' not in x]
for r in (args or ROUTES):
    tsv = os.path.join(ROOT, 'out', 'lift_check.tsv')
    if os.path.exists(tsv):
        os.remove(tsv)
    p = subprocess.run([sys.executable, os.path.join(HERE, 'runwatch.py'), os.path.join(L, 'check_%s%s.log' % (r, tag)), ROUTES[r], '-',
                        'FM2_DECOMP_CHECK=lift', 'FM2_PROFILE=' + PROFILE.get(r, 'Benchmark'), 'FREEZE=120'] + EXTRA,
                       cwd=ROOT, capture_output=True, text=True)
    ok = os.path.exists(tsv)
    if ok:
        shutil.copy(tsv, os.path.join(L, 'lift_check_%s%s.tsv' % (r, tag)))
    print('check', r, p.stdout.strip(), 'tsv' if ok else 'NO TSV', flush=True)

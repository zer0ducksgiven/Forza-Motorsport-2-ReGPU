"""Runs build/cov/fm2.exe windowed with the debug server; kills it when its frame count stops for FREEZE seconds.
usage: runwatch.py <log> <tour> [enable file | -] [KEY=VALUE ...]; prints 'finished', 'frozen at <last tour step>' or 'exit N'"""
import json, os, re, socket, subprocess, sys, time
ROOT = os.environ.get('FM2_ROOT', r'D:\fm2-port')
log, tour = sys.argv[1], sys.argv[2]
env = dict(os.environ, FM2_NO_LAUNCHER='1', FM2_NO_UPDATE_CHECK='1', FM2_SKIP_INTRO='1', FM2_FULLSCREEN='0', FM2_RESOLUTION='1280x720',
           FM2_COVERAGE_FILE='out/cov_x.tsv', FM2_TOUR=tour, FM2_DEBUG='1')
env.setdefault('FM2_PROFILE', 'Benchmark')
if len(sys.argv) > 3 and sys.argv[3] != '-':
    env['FM2_LIFT_ENABLE'] = sys.argv[3]
for kv in sys.argv[4:]:  # KEY=VALUE settings for this run (FM2_DECOMP_CHECK=lift, FM2_PROFILE=..., FREEZE=30)
    k, v = kv.split('=', 1)
    env[k] = v
    os.environ[k] = v


def frame():
    try:
        with socket.create_connection(('127.0.0.1', 7341), timeout=2) as c:
            c.settimeout(2)
            c.sendall(b'status\n')
            data = b''
            while not data.endswith(b'\n'):
                data += c.recv(4096)
            return json.loads(data).get('frame')
    except Exception:
        return None


FREEZE = float(os.environ.get('FREEZE', '8'))
out = open(log, 'w')
p = subprocess.Popen([os.environ.get('FM2_EXE', os.path.join(ROOT, 'build', 'cov', 'fm2.exe'))], cwd=ROOT, env=env, stdout=out, stderr=out)
last, seen, t0 = None, time.time(), time.time()
while p.poll() is None:
    time.sleep(1)
    cur = frame()
    if cur is not None and cur != last:
        last, seen = cur, time.time()
    elif time.time() - seen > (FREEZE if last else 60):
        p.kill()
        p.wait()
        steps = re.findall(r'tour: step [^\n]*', open(log, errors='replace').read())
        print('frozen at', steps[-1] if steps else 'boot', 'frame', last, 'after %.0fs' % (time.time() - t0))
        sys.exit(1)
text = open(log, errors='replace').read()
print('finished' if 'tour: finished' in text else 'exit %d' % p.returncode, '%.0fs' % (time.time() - t0))

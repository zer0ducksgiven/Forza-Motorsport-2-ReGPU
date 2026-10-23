"""Drives the running game through its debug server (src/debug.cpp), so a test doesn't need a restart.

  fm2ctl.py start [NAME=VALUE ...]   start build/tu/fm2.exe with FM2_DEBUG=1 (and those variables), wait for it
  fm2ctl.py <command> [args]         one command; prints the reply
  fm2ctl.py run <file>               commands from a file, one per line (# comments); stops at the first error
  fm2ctl.py run -                    the same from stdin

Commands (sent as typed):
  ping | status                      alive? / frame, fps, profile
  press A [ms] | press A+B 200       tap buttons (default 120 ms): A B X Y START BACK UP DOWN LEFT RIGHT LB RB LS RS
  hold A | release A | release all   hold until released (release all also stops sticks and triggers)
  stick L|R x y [ms]                 x, y in -1..1; without ms it stays until "stick L 0 0" or "release all"
  trigger L|R v [ms]                 v in 0..1
  wait 30 | wait 2.5s                frames, or seconds
  shot [path]                        the next frame as a PNG (default out/live.png)
  capture                            what F12 does (trace the next frame into out/capture)
  log [n]                            the last n lines of the log
  peek ADDR [bytes] | poke ADDR HEX  guest memory (hex addresses, big-endian bytes)
  ps HASH EXPR | ps HASH tint R,G,B  that pixel shader returns EXPR (HLSL over r[], input.o[], ...) or a flat
                                     colour, live; "ps HASH" alone goes back to the original
  vs HASH STATEMENT                  that vertex shader also runs STATEMENT at its end (e.g. o[0].zw = r[4].xy;),
                                     to read values with a ps edit; "vs HASH" alone goes back
  hlsl HASH [path]                   save a shader's translated HLSL (default out/hlsl/HASH.hlsl)
  switches                           the on/off FM2_* switches reached so far, and their state
  switch FM2_NAME 0|1                flip one live (A/B tests without a restart; one not reached yet is
                                     kept for when it is)
  env NAME [value]                   set an environment variable inside the game (switches with a value,
                                     which are mostly read once at their first use)
  quit                               close the game
Port: FM2_DEBUG_PORT (default 7341).
"""
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = int(os.environ.get('FM2_DEBUG_PORT', '7341'))


def connect(timeout=5.0):
    s = socket.create_connection(('127.0.0.1', PORT), timeout=timeout)
    s.settimeout(None)
    return s


def send(s, line):
    s.sendall((line.strip() + '\n').encode())
    data = b''
    while not data.endswith(b'\n'):
        chunk = s.recv(65536)
        if not chunk:
            raise ConnectionError('the game closed the connection')
        data += chunk
    return json.loads(data)


def show(reply):
    if reply.get('ok') and 'lines' in reply:
        print('\n'.join(reply['lines']))
    else:
        print(json.dumps(reply))


def start(assignments):
    env = dict(os.environ, FM2_DEBUG='1')
    env.setdefault('FM2_NO_LAUNCHER', '1')
    env.setdefault('FM2_NO_UPDATE_CHECK', '1')
    for a in assignments:
        k, _, v = a.partition('=')
        env[k] = v
    os.makedirs(os.path.join(ROOT, 'out'), exist_ok=True)
    log = open(os.path.join(ROOT, 'out', 'live.log'), 'w')
    flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0
    # FM2CTL_WRAP: a launcher to start the game through, e.g. renderdoccmd capture -w (its arguments, '|'-separated)
    wrap = [w for w in os.environ.get('FM2CTL_WRAP', '').split('|') if w]
    p = subprocess.Popen(wrap + [os.path.join(ROOT, 'build', 'tu', 'fm2.exe')], cwd=ROOT, env=env, stdout=log, stderr=log,
                         creationflags=flags)
    for _ in range(120):
        time.sleep(0.5)
        if p.poll() is not None:
            print(json.dumps({'ok': False, 'error': 'the game exited (%s); see out/live.log' % p.returncode}))
            return 1
        try:
            with connect(1) as s:
                print(json.dumps(dict(send(s, 'status'), pid=p.pid)))
                return 0
        except OSError:
            pass
    print(json.dumps({'ok': False, 'error': 'no debug server after 60 s; see out/live.log'}))
    return 1


def main(argv):
    if len(argv) < 2 or argv[1] in ('-h', '--help'):
        print(__doc__)
        return 2
    if argv[1] == 'start':
        return start(argv[2:])
    try:
        s = connect()
    except OSError:
        print(json.dumps({'ok': False, 'error': 'the game is not running with FM2_DEBUG=1 (fm2ctl.py start)'}))
        return 1
    with s:
        if argv[1] == 'run':
            lines = sys.stdin.read().splitlines() if argv[2] == '-' else open(argv[2]).read().splitlines()
            for line in lines:
                line = line.split('#', 1)[0].strip()
                if not line:
                    continue
                reply = send(s, line)
                print('>', line)
                show(reply)
                if not reply.get('ok'):
                    return 1
            return 0
        reply = send(s, ' '.join(argv[1:]))
        show(reply)
        return 0 if reply.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))

"""Unattended race tests: the game's AI drives every car (the player's too) while the renderer watches its
own frames, so problems that only show in a busy race can be found without anyone playing.

  py -3 tools/stressrace.py [--tracks 0,2,20] [--seconds 150] [--ai 7] [--car 254]
                            [--resolution 2560x1440] [--monitor2] [--load N] [NAME=VALUE ...]

For each track: boots straight into an all-AI race (quickrace.py --autodrive) with FM2_FLASH_WATCH=1 from the
first frame, lets it run, then reads the log. Reported per track:
  - flash events: frames whose brightness or saturation jumped and came back (saved to out/stress/<time>_track<id>/)
  - frame times (average and slowest 1%), and what the "performance:" lines say limited them
  - warnings: ring full, dropped draws, upscaler errors, frames where the temporal upscaler did not run
--load N starts N busy processes for the run, so frame times swing as they do on a loaded PC.
--monitor2 moves the window to the second monitor (out of the way of whoever is using the PC).
NAME=VALUE are the game's FM2_* variables, e.g. FM2_UPSCALER=fsr3 FM2_FSR_SCALE=85 FM2_FRAMEGEN=1.
The log of each run is kept as out/stress/<time>_track<id>.log.
"""
import argparse, ctypes, glob, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CTL = [sys.executable, os.path.join(HERE, 'fm2ctl.py')]


def ctl(*args, timeout=60):
    try:
        return subprocess.run(CTL + list(args), capture_output=True, text=True, timeout=timeout).stdout.strip()
    except subprocess.TimeoutExpired:
        return ''


def game_pid(reply):
    m = re.search(r'"pid": (\d+)', reply)
    return int(m.group(1)) if m else 0


def alive(pid):
    out = subprocess.run(['tasklist', '/FI', f'PID eq {pid}', '/NH'], capture_output=True, text=True).stdout
    return str(pid) in out


def to_second_monitor():
    from ctypes import wintypes
    u = ctypes.windll.user32
    u.SetProcessDPIAware()
    mons = []
    MON = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HMONITOR, wintypes.HDC, ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)
    u.EnumDisplayMonitors(0, 0, MON(lambda h, dc, rc, l: mons.append((rc.contents.left, rc.contents.top)) or True), 0)
    second = [m for m in mons if m != (0, 0)]
    if not second:
        return False
    WIN = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    for _ in range(40):
        found = []

        def each(h, l):
            name = ctypes.create_unicode_buffer(200)
            u.GetWindowTextW(h, name, 200)
            if name.value.startswith('Forza Motorsport 2') and u.IsWindowVisible(h):
                found.append(h)
            return True
        u.EnumWindows(WIN(each), 0)
        if found:
            u.SetWindowPos(found[0], 1, second[0][0], second[0][1], 0, 0, 0x0011)  # to the back, not activated
            return True
        time.sleep(0.5)
    return False


def summarise(track, log, out):
    lines = open(log, errors='replace').read().split('\n')
    events = [l for l in lines if 'flash: event' in l]
    watching = sum('flash: watching' in l for l in lines)
    frames = [(float(m.group(1)), float(m.group(2))) for l in lines
              for m in [re.search(r'renderer: frames ([\d.]+) ms average, ([\d.]+) ms 1% slowest', l)] if m]
    limits = {}
    for l in lines:
        m = re.search(r'performance: .*?limited by (.*?) \(frame', l)
        if m and ' 0 of ' not in l:
            limits[m.group(1)] = limits.get(m.group(1), 0) + 1
    warn = [l for l in lines if re.search(r'ring full|draw dropped|fsr3?: (create|dispatch) failed|threw|device removed|DXGI_ERROR', l)]
    idle = 0
    for l in lines:
        m = re.search(r'temporal: (\d+) frames:.*scenes done (\d+) \(upscaler ran (\d+)\)', l)
        if m and int(m.group(2)) != int(m.group(3)):
            idle += int(m.group(2)) - int(m.group(3))
    print(f'track {track}: {len(events)} flash event(s), watch heartbeats {watching}', file=out)
    if frames:
        avg = sum(a for a, _ in frames) / len(frames)
        print(f'  frames {avg:.2f} ms average, slowest 1% up to {max(b for _, b in frames):.1f} ms; limited by: '
              + (', '.join(f'{k} x{v}' for k, v in limits.items()) or 'no race lines'), file=out)
    if idle:
        print(f'  WARNING: {idle} scene frame(s) where the temporal upscaler did not run', file=out)
    for l in warn[:8]:
        print('  WARNING: ' + l[:200], file=out)
    for l in events:
        print('  ' + l[:260], file=out)
    return len(events)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tracks', default='0')
    ap.add_argument('--seconds', type=int, default=150)
    ap.add_argument('--ai', type=int, default=7)
    ap.add_argument('--car', type=int, default=254)
    ap.add_argument('--resolution', default='1280x720')
    ap.add_argument('--monitor2', action='store_true')
    ap.add_argument('--load', type=int, default=0)
    ap.add_argument('env', nargs='*')
    a = ap.parse_args()
    os.makedirs(os.path.join(ROOT, 'out', 'stress'), exist_ok=True)
    total = 0
    for track in a.tracks.split(','):
        flash = os.path.join(ROOT, 'out', 'flash')
        for f in glob.glob(os.path.join(flash, 'e*.png')):
            os.remove(f)
        cmd = [sys.executable, os.path.join(HERE, 'quickrace.py'), '--track', track, '--car', str(a.car), '--ai', str(a.ai), '--autodrive',
               'FM2_FLASH_WATCH=1', 'FM2_RESOLUTION=' + a.resolution] + a.env
        reply = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT).stdout
        pid = game_pid(reply)
        if not pid:
            print(f'track {track}: the game did not start: {reply.strip()[-200:]}')
            continue
        if a.monitor2:
            to_second_monitor()
        burners = [subprocess.Popen([sys.executable, '-c', 'while True: pass']) for _ in range(a.load)]
        end = time.time() + a.seconds
        while time.time() < end and alive(pid):
            time.sleep(2)
        for b in burners:
            b.kill()
        died = not alive(pid)
        if not died:
            ctl('quit')
            for _ in range(40):
                if not alive(pid):
                    break
                time.sleep(1)
        tag = time.strftime('%m%d-%H%M%S') + f'_track{track}'
        log = os.path.join(ROOT, 'out', 'stress', tag + '.log')
        shutil.copyfile(os.path.join(ROOT, 'out', 'live.log'), log)
        keep = os.path.join(ROOT, 'out', 'stress', tag)
        shutil.rmtree(keep, ignore_errors=True)
        pngs = glob.glob(os.path.join(flash, 'e*.png'))
        if pngs:
            os.makedirs(keep)
            for f in pngs:
                shutil.move(f, keep)
        if died:
            print(f'track {track}: THE GAME STOPPED before the {a.seconds} s were up (see {log})')
        total += summarise(f'{track} ({tag})', log, sys.stdout)
        sys.stdout.flush()
    print(f'done: {total} flash event(s)')


if __name__ == '__main__':
    main()

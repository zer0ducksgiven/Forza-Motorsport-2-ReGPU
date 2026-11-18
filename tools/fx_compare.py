"""Per-effect flicker compare from tools/fx_capture.sh captures (out/fx/static/<name>, out/fx/drive/<name>).
For every effect: a native | effect side-by-side MP4 parked and driving (driving aligned by the race timer), a parked
flicker heat map, and flicker scores. Writes <dest>/index.html, static/, drive/, heat/, scores.json.
Flicker event (as tools/flicker.py): a pixel's luminance jumps by more than 24 and is back within 8 on the next frame.
Usage: fx_compare.py <dest dir>"""
import sys, os, glob, json, subprocess, html
import numpy as np
from PIL import Image, ImageDraw

DEST = sys.argv[1]
SRC = 'out/fx'
T, RET, MIN_EVENTS = 24, 8, 3
SW = {'aniso2': 'ANISO=2', 'aniso0': 'ANISO=0', 'help50': 'HELPERS=50', 'help25': 'HELPERS=25', 'r1010': 'RESOLVE1010=1',
      'dec10': 'DECIMATE=10', 'cull64': 'CULL_PIXELS=64 CULL_ALL=1', 'cull0': 'CULL_PIXELS=0 CULL_ALL=1',
      'cull256fix': 'CULL_PIXELS=256 CULL_ALL=1 CULL_PASSES=1 CULL_PROBE=1 CULL_HYST=200', 'a2m': 'A2M_TEST=1',
      'freeze': 'FREEZE=120 FREEZE_REFRESH=3', 'lean2': 'LEAN_FETCH=2', 'lean3': 'LEAN_FETCH=3', 'leanv': 'LEAN_VFETCH=1',
      'nodiscard': 'NODISCARD=1', 'shaderopt': 'SHADER_OPT=1', 'plainmul': 'PLAIN_MUL=5', 'fetchspec': 'FETCHSPEC=1',
      'swizzle': 'SWIZZLE_SEL=1', 'leanlod': 'LEAN_LOD=1', 'tailspec': 'TAILSPEC=1', 'bysize': 'RESOLVE_BY_SIZE=1',
      'native2': '(second native run: the noise floor)'}


def load(d):
    return np.stack([np.asarray(Image.open(f).convert('RGB')) for f in sorted(glob.glob(d + '/*.png'))])


def lum(fr):
    return fr[..., 0] * 0.299 + fr[..., 1] * 0.587 + fr[..., 2] * 0.114


def events(fr):
    y = lum(fr.astype(np.float32))
    a, b, c = y[:-2], y[1:-1], y[2:]
    return ((np.abs(b - a) > T) & (np.abs(c - a) < RET) & (np.abs(c - b) > T)).sum(0), len(fr) - 2


def video(path, frames_a, frames_b, la, lb, fps, loops=1, crf=17):
    h, w = frames_a[0].shape[:2]
    W, H = w * 2 + 8, h + 28
    p = subprocess.Popen(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{W}x{H}', '-r', str(fps), '-i', '-',
                          '-c:v', 'libx264', '-crf', str(crf), '-preset', 'fast', '-pix_fmt', 'yuv420p', path], stdin=subprocess.PIPE)
    for _ in range(loops):
        for i, (fa, fb) in enumerate(zip(frames_a, frames_b)):
            c = Image.new('RGB', (W, H), (12, 14, 13))
            c.paste(Image.fromarray(fa), (0, 28)); c.paste(Image.fromarray(fb), (w + 8, 28))
            d = ImageDraw.Draw(c); d.text((8, 8), f'{la}   frame {i}', fill=(230, 235, 232)); d.text((w + 16, 8), lb, fill=(230, 235, 232))
            p.stdin.write(np.asarray(c).tobytes())
    p.stdin.close(); p.wait()


def align(A, B):
    crop = lambda f: f[45:80, 1030:1200].astype(np.float32)
    ta, tb = [crop(f) for f in A], [crop(f) for f in B]
    best = None
    for k in range(-60, 61):
        pr = [(i, i + k) for i in range(len(A)) if 0 <= i + k < len(B) and ta[i].mean() > 80 and tb[i + k].mean() > 80]
        if len(pr) < 30: continue
        e = np.mean([np.abs(ta[i] - tb[j]).mean() for i, j in pr])
        if best is None or e < best[0]: best = (e, k)
    k = best[1] if best else 0
    return k, [(i, i + k) for i in range(len(A)) if 0 <= i + k < len(B)]


for s in ('static', 'drive', 'heat'):
    os.makedirs(os.path.join(DEST, s), exist_ok=True)
names = [n for n in SW if os.path.isdir(f'{SRC}/static/{n}') or os.path.isdir(f'{SRC}/drive/{n}')]
res = {}
ref_s = load(f'{SRC}/static/native'); rc, rn = events(ref_s); ref_any = rc >= 1
res['native'] = dict(s_events=float(rc.sum() / rn), s_flick=float(100 * (rc >= MIN_EVENTS).mean()))
ref_d = load(f'{SRC}/drive/native') if os.path.isdir(f'{SRC}/drive/native') else None
if ref_d is not None:
    dc, dn = events(ref_d); res['native']['d_events'] = float(dc.sum() / dn)
for n in names:
    r = res.setdefault(n, {})
    if os.path.isdir(f'{SRC}/static/{n}'):
        fr = load(f'{SRC}/static/{n}'); cnt, k = events(fr); flick = cnt >= MIN_EVENTS; excess = flick & ~ref_any
        r.update(s_events=float(cnt.sum() / k), s_flick=float(100 * flick.mean()), s_excess_px=int(excess.sum()),
                 s_diff=float(np.abs(fr[0].astype(np.float32) - ref_s[0].astype(np.float32)).mean()))
        img = fr[0] * 0.35; img[flick & ref_any] = [255, 220, 0]; img[excess] = [255, 0, 0]
        Image.fromarray(img.clip(0, 255).astype(np.uint8)).save(f'{DEST}/heat/{n}.png')
        video(f'{DEST}/static/{n}.mp4', ref_s, fr, 'native', n, 30, loops=3)
    if ref_d is not None and os.path.isdir(f'{SRC}/drive/{n}'):
        fr = load(f'{SRC}/drive/{n}'); cnt, k = events(fr); off, pairs = align(ref_d, fr)
        r.update(d_events=float(cnt.sum() / k), d_offset=off,
                 d_diff=float(np.mean([np.abs(ref_d[i].astype(np.float32) - fr[j].astype(np.float32)).mean() for i, j in pairs[::6]])))
        video(f'{DEST}/drive/{n}.mp4', [ref_d[i] for i, _ in pairs], [fr[j] for _, j in pairs], 'native', n, 12, crf=21)
    print(n, r, flush=True)
json.dump(res, open(f'{DEST}/scores.json', 'w'), indent=1)

nat = res['native']
order = sorted(names, key=lambda n: -(res[n].get('s_excess_px', 0) * 1000 + res[n].get('d_events', 0)))
rows = []
for n in order:
    r = res[n]
    dx = r['d_events'] / nat['d_events'] if 'd_events' in r and nat.get('d_events') else None
    rows.append('<tr><td><b>%s</b><br><code>%s</code></td><td class=n>%s</td><td class=n>%s</td><td class=n>%s</td><td class=n>%s</td><td class=n>%s</td><td>%s %s</td><td>%s</td></tr>' % (
        n, html.escape(SW[n]),
        '%.0f' % r['s_events'] if 's_events' in r else '-', '%.3f%%' % r['s_flick'] if 's_flick' in r else '-',
        r.get('s_excess_px', '-'), '%.2f' % r['s_diff'] if 's_diff' in r else '-',
        ('%.0f (x%.2f)' % (r['d_events'], dx)) if dx else '-',
        '<a href="static/%s.mp4">parked video</a>' % n if 's_events' in r else '', '<a href="heat/%s.png">heat map</a>' % n if 's_events' in r else '',
        '<a href="drive/%s.mp4">driving video</a>' % n if 'd_events' in r else ''))
open(f'{DEST}/index.html', 'w', encoding='utf-8').write('''<!doctype html><meta charset=utf-8><title>Per-effect flicker compare</title>
<style>body{font:15px/1.45 system-ui,sans-serif;background:#111413;color:#e4e8e6;margin:24px auto;max-width:1180px;padding:0 16px}
table{border-collapse:collapse;width:100%%}td,th{border-bottom:1px solid #2a302d;padding:7px 10px;text-align:left;vertical-align:top}
td.n,th.n{text-align:right;font-variant-numeric:tabular-nums}code{color:#9fb0a8;font-size:12px}a{color:#8fc7ff}p{color:#b9c2bd}</style>
<h1>Each effect alone vs native: parked and driving</h1>
<p>Laptop (Ryzen 3 5300U, Vega), native 1280x720, 16x anisotropy, upscaler off. Every video is native on the left, the effect on the right.
Parked: 60 consecutive frames, played 3 times at 30 fps. Driving: straight-line auto-drive capped at 12 fps, 180 frames, aligned by the race timer.</p>
<p>A flicker event is a pixel whose brightness jumps by more than 24 of 255 and is back on the next frame. Heat map: red = flickers here but never in native,
yellow = flickers in native too. Native parked: %.0f events per frame, %.3f%% of pixels flickering. Native driving: %s events per frame.
Rows are sorted with the most extra parked flicker first. Driving scores include real motion, so compare them with native (the multiplier) and with native2.</p>
<table><tr><th>Effect (REGPU_HYPER_ switch)</th><th class=n>Parked events/frame</th><th class=n>Parked flickering px</th><th class=n>Extra flickering px vs native</th>
<th class=n>Parked diff vs native</th><th class=n>Driving events/frame</th><th>Parked</th><th>Driving</th></tr>
%s</table>''' % (nat['s_events'], nat['s_flick'], '%.0f' % nat['d_events'] if 'd_events' in nat else '-', '\n'.join(rows)))
print('wrote', f'{DEST}/index.html')

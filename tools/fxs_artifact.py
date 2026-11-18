"""Stack flicker review (16 Nov 2026): scores, heat maps and artifact-sized videos for the captures of
FX_STACKS=1 FX_DIR=out/fxs tools/fx_capture.sh. Native reference: out/fx/{static,drive}/native.
Per stack: parked side-by-side video (native | stack, one 60-frame loop; the stack alone when its size differs), parked
heat map, driving video on native's timeline (alone, own timeline, when the size or frame rate differs), scores.
Raw frame dumps are re-saved as compressed PNG (lossless) as they are read.
Usage (repo root): fxs_artifact.py <dest dir>"""
import sys, os, glob, json, subprocess, shutil
import numpy as np
from PIL import Image, ImageDraw

DEST, SRC, REF = sys.argv[1], 'out/fxs', 'out/fx'
T, RET, MIN_EVENTS = 24, 8, 3
H = 'REGPU_HYPER'
STACKS = [  # name, label, what it is, group
    ('potato', 'Full Potato', 'REGPU_HYPER=potato: every Potato cut together', 'Full stack'),
    ('potato2', 'Full Potato, second run', 'same switches again: the noise floor for Potato', 'Full stack'),
    ('potato-30fps', 'Full Potato, 30 fps driving', 'Potato with driving captured at a 30 fps cap instead of 12', 'Full stack'),
    ('potato-laptop', 'Potato, older laptop set', 'REGPU_HYPER_POTATO_SET=laptop: lean fetch v3, cull 64 px, no shader cuts, no frozen helpers', 'Full stack'),
    ('potato-fsr1', 'Potato + FSR 1 to 900p', 'Potato rendered at 80% and upscaled to 1600x900 with FSR 1', 'Upscaled'),
    ('potato-fsr3', 'Potato + FSR 3 to 900p', 'Potato rendered at 80% and upscaled to 1600x900 with FSR 3', 'Upscaled'),
    ('p-nocull', 'Potato without draw culling', 'CULL_PIXELS=-1', 'Potato minus one'),
    ('p-nofreeze', 'Potato without frozen helpers', 'FREEZE=0', 'Potato minus one'),
    ('p-help100', 'Potato with full-size helper surfaces', 'HELPERS=100', 'Potato minus one'),
    ('p-nodec', 'Potato without mesh decimation', 'DECIMATE=0', 'Potato minus one'),
    ('p-aniso16', 'Potato with 16x anisotropy', 'ANISO=16', 'Potato minus one'),
    ('p-noshader', 'Potato without the shader cuts', 'SHADER_OPT=-1 PLAIN_MUL=0 FETCHSPEC=0 SWIZZLE_SEL=0 LEAN_LOD=0 TAILSPEC=0', 'Potato minus one'),
    ('p-discard', 'Potato with discards kept', 'NODISCARD=0', 'Potato minus one'),
    ('p-noresolve', 'Potato without the resolve cuts', 'RESOLVE1010=0 RESOLVE_BY_SIZE=0', 'Potato minus one'),
    ('p-novcache', 'Potato without triangle reordering', 'VCACHE=0', 'Potato minus one'),
    ('g-surf', 'Surface cuts only', 'HELPERS=25 FREEZE=120 FREEZE_REFRESH=3 RESOLVE1010=1 RESOLVE_BY_SIZE=1', 'Groups'),
    ('g-geom', 'Geometry cuts only', 'CULL_PIXELS=128 CULL_ALL=1 CULL_PASSES=1 CULL_PROBE=1 CULL_HYST=200 DECIMATE=10 VCACHE=2', 'Groups'),
    ('g-shader', 'Shader cuts only', 'SHADER_OPT=1 PLAIN_MUL=5 FETCHSPEC=1 SWIZZLE_SEL=1 LEAN_LOD=1 TAILSPEC=1 NODISCARD=1', 'Groups'),
    ('g-surf-geom', 'Surface + geometry cuts', 'the two groups together', 'Groups'),
    ('g-surf-shader', 'Surface + shader cuts', 'the two groups together', 'Groups'),
    ('g-geom-shader', 'Geometry + shader cuts', 'the two groups together', 'Groups'),
]


def files(d):
    return sorted(glob.glob(d + '/*.png'))


def load(d):
    out = []
    for f in files(d):
        im = Image.open(f).convert('RGB'); a = np.asarray(im); out.append(a)
        if os.path.getsize(f) > a.size * 0.6:  # an uncompressed dump: re-save it compressed
            im.save(f + '.tmp.png', compress_level=6); os.replace(f + '.tmp.png', f)
    return np.stack(out)


def events(fr):
    y = fr.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32)
    a, b, c = y[:-2], y[1:-1], y[2:]
    return ((np.abs(b - a) > T) & (np.abs(c - a) < RET) & (np.abs(c - b) > T)).sum(0), len(fr) - 2


def encode(path, frames, fps, extra):
    h, w = frames[0].shape[:2]
    p = subprocess.Popen(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{w}x{h}', '-r', str(fps), '-i', '-',
                          '-c:v', 'libx264', *extra, '-pix_fmt', 'yuv420p', '-movflags', '+faststart', path], stdin=subprocess.PIPE)
    for f in frames:
        p.stdin.write(np.ascontiguousarray(f).tobytes())
    p.stdin.close(); p.wait()


def side(fa, fb, la, lb):
    h, w = fa.shape[:2]
    c = Image.new('RGB', (w * 2 + 8, h + 28), (12, 14, 13))
    c.paste(Image.fromarray(fa), (0, 28)); c.paste(Image.fromarray(fb), (w + 8, 28))
    d = ImageDraw.Draw(c); d.text((8, 8), la, fill=(230, 235, 232)); d.text((w + 16, 8), lb, fill=(230, 235, 232))
    return np.asarray(c)


def align(A, B):
    crop = lambda f: f[45:80, 1030:1200].astype(np.float32)
    ta, tb = [crop(f) for f in A], [crop(f) for f in B]
    best = None
    for k in range(-60, 61):
        pr = [(i, i + k) for i in range(len(A)) if 0 <= i + k < len(B) and ta[i].mean() > 80 and tb[i + k].mean() > 80]
        if len(pr) < 30: continue
        e = np.mean([np.abs(ta[i] - tb[j]).mean() for i, j in pr])
        if best is None or e < best[0]: best = (e, k)
    return best[1] if best else 0


DRIVE = ['-b:v', '4000k', '-maxrate', '5000k', '-bufsize', '8000k', '-preset', 'slow', '-g', '12']
for s in ('static', 'drive', 'heat'):
    os.makedirs(f'{DEST}/{s}', exist_ok=True)
res_path = f'{DEST}/scores.json'
res = json.load(open(res_path)) if os.path.exists(res_path) else {}
ref_s = load(f'{REF}/static/native'); rc, rn = events(ref_s); ref_any = rc >= 1
ref_d = load(f'{REF}/drive/native'); dc, dn = events(ref_d)
res['native'] = dict(s_events=float(rc.sum() / rn), d_events=float(dc.sum() / dn))
if not os.path.exists(f'{DEST}/drive/native.mp4'):
    encode(f'{DEST}/drive/native.mp4', ref_d, 12, DRIVE)
for n, label, what, group in STACKS:
    r = res.setdefault(n, {}); r.update(label=label, what=what, group=group)
    sd, dd = f'{SRC}/static/{n}', f'{SRC}/drive/{n}'
    if len(files(sd)) >= 60 and not os.path.exists(f'{DEST}/static/{n}.mp4'):
        fr = load(sd); cnt, k = events(fr); flick = cnt >= MIN_EVENTS; same = fr.shape == ref_s.shape
        r.update(s_events=float(cnt.sum() / k), s_flick_px=int(flick.sum()), s_w=int(fr.shape[2]), s_h=int(fr.shape[1]), s_solo=not same)
        img = fr[0] * 0.35
        if same:
            excess = flick & ~ref_any
            r.update(s_excess_px=int(excess.sum()), s_diff=float(np.abs(fr[0].astype(np.float32) - ref_s[0]).mean()))
            img[flick & ref_any] = [255, 220, 0]; img[excess] = [255, 0, 0]
            encode(f'{DEST}/static/{n}.mp4', [side(a, b, 'native', label) for a, b in zip(ref_s, fr)], 30, ['-crf', '17', '-preset', 'fast'])
        else:
            img[flick] = [255, 0, 0]
            encode(f'{DEST}/static/{n}.mp4', fr, 30, ['-crf', '17', '-preset', 'fast'])
        Image.fromarray(img.clip(0, 255).astype(np.uint8)).save(f'{DEST}/heat/{n}.png')
        del fr
    if len(files(dd)) >= 180 and not os.path.exists(f'{DEST}/drive/{n}.mp4'):
        fr = load(dd); cnt, k = events(fr)
        fps = 30 if '30fps' in n else 12
        solo = fr.shape != ref_d.shape or fps != 12
        r.update(d_events=float(cnt.sum() / k), d_fps=fps, d_solo=bool(solo), d_w=int(fr.shape[2]), d_h=int(fr.shape[1]))
        if solo:
            encode(f'{DEST}/drive/{n}.mp4', fr, fps, DRIVE)
        else:
            off = align(ref_d, fr); r['d_offset'] = off
            encode(f'{DEST}/drive/{n}.mp4', [fr[min(max(i + off, 0), len(fr) - 1)] for i in range(180)], 12, DRIVE)
        del fr
    print(n, {k: v for k, v in r.items() if k not in ('label', 'what', 'group')}, flush=True)
    json.dump(res, open(res_path, 'w'), indent=1)
print('ARTDONE')

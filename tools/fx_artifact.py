"""Small copies of the flicker compare for the published artifact (size limits: 15 MB a file, 256 MB in all).
Driving: one 1280x720 video per effect on native's timeline (frame i shows effect frame i + d_offset from scores.json,
clamped), so the page plays native.mp4 beside it in sync. Parked: the first loop of the side-by-side video, copied.
Usage (from the repo root): fx_artifact.py <flicker-compare dir> <dest dir>"""
import sys, os, glob, json, subprocess, shutil

SRC, CMP, DEST = 'out/fx/drive', sys.argv[1], sys.argv[2]
scores = json.load(open(f'{CMP}/scores.json'))
for s in ('drive', 'static', 'heat'):
    os.makedirs(f'{DEST}/{s}', exist_ok=True)
for n in ['native'] + [k for k in scores if k != 'native']:
    out = f'{DEST}/drive/{n}.mp4'
    files = sorted(glob.glob(f'{SRC}/{n}/*.png'))
    if files and not os.path.exists(out):
        k = scores[n].get('d_offset', 0)
        lst = f'{DEST}/drive/{n}.txt'
        with open(lst, 'w') as f:
            for i in range(180):
                j = min(max(i + k, 0), len(files) - 1)
                f.write("file '%s'\nduration %.6f\n" % (os.path.abspath(files[j]).replace('\\', '/'), 1 / 12))
        subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', lst, '-vf', 'fps=12', '-c:v', 'libx264',
                        '-b:v', '4000k', '-maxrate', '5000k', '-bufsize', '8000k', '-preset', 'slow', '-g', '12', '-pix_fmt', 'yuv420p',
                        '-movflags', '+faststart', out + '.tmp.mp4'], check=True)
        os.replace(out + '.tmp.mp4', out); os.remove(lst)
        print('drive', n, os.path.getsize(out), flush=True)
    if n == 'native': continue
    if os.path.exists(f'{CMP}/static/{n}.mp4') and not os.path.exists(f'{DEST}/static/{n}.mp4'):
        subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', f'{CMP}/static/{n}.mp4', '-t', '2', '-c', 'copy', '-movflags', '+faststart',
                        f'{DEST}/static/{n}.mp4'], check=True)
    if os.path.exists(f'{CMP}/heat/{n}.png'):
        shutil.copyfile(f'{CMP}/heat/{n}.png', f'{DEST}/heat/{n}.png')
print('ARTDONE')

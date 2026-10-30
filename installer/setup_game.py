"""FM2 setup helper. The installer runs it with the Python bundled in the installer.

  setup_game.py check-iso <disc.iso>        exit 0 when the disc matches this build
  setup_game.py check-tu <title update>     exit 0 when the title update matches this build
  setup_game.py install <disc.iso> <game dir> <title update> <dlc folder or -> <fm2 documents dir>
                                            unpacks the title update, copies DLC, builds the game files
  setup_game.py space <game dir>            exit 0 when its drive has room for the game files
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# The files this build was made from. A disc or title update must match these to be used.
REF_DEFAULT_XEX = '9726066f0c546c4e51340ed02a32ae44a89d4175de82a2d55aaaf5451d9b120a'
REF_TITLE_UPDATE = 'b5fd1860c2fbab6e594baf7dd58cbfe867f51f65e41177e15e0b36da0f61aae5'
# The game files take 4.8 GB; with room to spare.
NEEDED_BYTES = 6 * 1024 ** 3


def free_bytes(path):
    """Free space on the drive a folder is (or will be) on."""
    path = os.path.abspath(path)
    while not os.path.exists(path):
        parent = os.path.dirname(path)
        if parent == path:
            break
        path = parent
    return shutil.disk_usage(path).free


def check_space(game_dir):
    free = free_bytes(game_dir)
    if free < NEEDED_BYTES:
        print('Not enough space for the game files in %s: they need %.1f GB, %.1f GB is free.' % (
            os.path.abspath(game_dir), NEEDED_BYTES / 1024 ** 3, free / 1024 ** 3))
        return False
    return True


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


class ToolFailed(Exception):
    pass


def run_tool(script, *args):
    """Runs a tool, its output passed on; a failure raises ToolFailed with its last error line."""
    p = subprocess.run([sys.executable, os.path.join(HERE, script)] + list(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, errors='replace')
    sys.stdout.write(p.stdout)
    if p.returncode != 0:
        last = next((line.strip() for line in reversed(p.stdout.splitlines()) if line.strip()), '')
        raise ToolFailed('%s failed (exit code %d)%s' % (script, p.returncode, ': ' + last if last else ''))


def check_iso(iso):
    tmp = tempfile.mkdtemp(prefix='fm2-check-')
    try:
        run_tool('xiso.py', 'extract', iso, tmp, 'default.xex')
        found = os.path.join(tmp, 'default.xex')
        return os.path.exists(found) and sha256(found) == REF_DEFAULT_XEX
    except (ToolFailed, OSError):
        return False  # not a disc image the tool can read
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def check_tu(path):
    return os.path.isfile(path) and sha256(path) == REF_TITLE_UPDATE


def install(iso, game_dir, tu, dlc, docs):
    # The title update and DLC first: they don't depend on the disc, and a disc build that failed
    # (an install on another drive without room for it) took the title update with it, so the
    # launcher reported none as well as no game files.
    # 1. The title update: the port loads update\default.xexp from the documents folder.
    update_dir = os.path.join(docs, 'update')
    os.makedirs(update_dir, exist_ok=True)
    run_tool('stfs.py', 'update', tu, update_dir)
    # 2. Optional DLC packages.
    if dlc and dlc != '-' and os.path.isdir(dlc):
        dlc_dir = os.path.join(docs, 'dlc')
        os.makedirs(dlc_dir, exist_ok=True)
        for name in os.listdir(dlc):
            src = os.path.join(dlc, name)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(dlc_dir, name))
    # 3. The game files, from the disc.
    if not check_space(game_dir):
        return 3
    os.makedirs(game_dir, exist_ok=True)
    run_tool('xiso.py', 'extract', iso, game_dir)
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == 'check-iso':
        return 0 if check_iso(argv[2]) else 1
    if cmd == 'check-tu':
        return 0 if check_tu(argv[2]) else 1
    if cmd == 'space':
        return 0 if check_space(argv[2]) else 1
    if cmd == 'install':
        try:
            return install(argv[2], argv[3], argv[4], argv[5], argv[6])
        except ToolFailed as e:
            print('Setup failed: %s' % e)
            return 4
        except OSError as e:
            print('Setup failed: %s' % e)
            return 5
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))

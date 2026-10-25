"""Builds out/FM2-<version>-patch.zip, the update the launcher downloads and unpacks over an install.

  make_patch.py <version> [launcher dir]

Contents (paths relative to the install): build/tu/fm2.exe, d3dcompiler_47.dll and SDL3.dll (racing wheels), the launcher (launcher/..., from the given folder or
installer/payload/launcher, which build-installer.ps1 publishes) and changelog.txt. Names are written with '/',
and the zip is read back and checked: no control characters, nothing outside the install.
"""
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    version = argv[1]
    launcher = argv[2] if len(argv) > 2 else os.path.join(ROOT, 'installer', 'payload', 'launcher')
    files = [(os.path.join(ROOT, 'build', 'tu', 'fm2.exe'), 'build/tu/fm2.exe'),
             (os.path.join(ROOT, 'build', 'tu', 'd3dcompiler_47.dll'), 'build/tu/d3dcompiler_47.dll'),
             (os.path.join(ROOT, 'build', 'tu', 'SDL3.dll'), 'build/tu/SDL3.dll'),
             (os.path.join(ROOT, 'out', 'release', 'FM2', 'changelog.txt'), 'changelog.txt')]
    if not os.path.isfile(os.path.join(launcher, 'FM2Launcher.exe')):
        print('no launcher at', launcher, '(run installer/build-installer.ps1 first)')
        return 1
    for dirpath, _, names in os.walk(launcher):
        for name in names:
            if name.endswith('.old') or name.endswith('.pdb'):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, launcher).replace(os.sep, '/')
            files.append((path, 'launcher/' + rel))
    for path, _ in files:
        if not os.path.isfile(path):
            print('missing', path)
            return 1

    out = os.path.join(ROOT, 'out', 'FM2-%s-patch.zip' % version)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for path, name in files:
            z.write(path, name)

    # read back: every name as intended
    with zipfile.ZipFile(out) as z:
        names = z.namelist()
        for n in names:
            if any(ord(c) < 32 for c in n) or '\\' in n or n.startswith('/') or '..' in n.split('/'):
                print('bad name in zip:', repr(n))
                return 1
        if 'build/tu/fm2.exe' not in names:
            print('fm2.exe missing from the zip')
            return 1
        bad = z.testzip()
        if bad:
            print('corrupt entry', bad)
            return 1
    print('%s: %d files, %.1f MB' % (out, len(names), os.path.getsize(out) / 1048576))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

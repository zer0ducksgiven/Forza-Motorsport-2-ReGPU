"""Builds out/FM2-Linux-<version>.tar.gz: the Windows fm2.exe and its DLLs (Microsoft's d3dcompiler_47.dll
among them) with the Proton scripts from linux/release, the setup helpers the Windows installer uses, and the
launcher built for Linux (launcher/FM2Launcher: the same launcher as Windows on WebKitGTK, self-contained, so no
.NET to install; it starts the game through Proton as play.sh does).

  make_linux.py <version>
"""
import io
import os
import subprocess
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    build = os.path.join(ROOT, 'build', 'tu')
    rel = os.path.join(ROOT, 'linux', 'release')
    files = [(os.path.join(build, 'fm2.exe'), 'fm2.exe', 0o644)]
    for name in sorted(os.listdir(build)):
        # DLSS needs NVIDIA's NGX, which vkd3d-proton doesn't provide.
        if name.lower().endswith('.dll') and name.lower() != 'nvngx_dlss.dll':
            files.append((os.path.join(build, name), name, 0o644))
    for name, mode in (('play.sh', 0o755), ('setup.sh', 0o755), ('README.md', 0o644)):
        files.append((os.path.join(rel, name), name, mode))
    for name in ('setup_game.py',):
        files.append((os.path.join(ROOT, 'installer', name), 'py/' + name, 0o644))
    for name in ('xiso.py', 'stfs.py'):
        files.append((os.path.join(ROOT, 'tools', name), 'py/' + name, 0o644))
    files.append((os.path.join(ROOT, 'out', 'release', 'FM2', 'changelog.txt'), 'changelog.txt', 0o644))
    launcher = os.path.join(ROOT, 'out', 'launcher-linux')
    r = subprocess.run(['dotnet', 'publish', os.path.join(ROOT, 'launcher', 'FM2Launcher', 'FM2Launcher.csproj'), '-c', 'Release',
                        '-r', 'linux-x64', '--self-contained', '-p:PublishSingleFile=true', '-p:EnableCompressionInSingleFile=true', '-o', launcher, '--nologo', '-v', 'q'])
    if r.returncode != 0:
        print('dotnet publish of the Linux launcher failed')
        return 1
    for dirpath, _, names in os.walk(launcher):
        for name in sorted(names):
            if name.endswith('.pdb'):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, launcher).replace(os.sep, '/')
            files.append((path, 'launcher/' + rel, 0o755 if rel in ('FM2Launcher', 'Photino.Native.so') else 0o644))
    if not any(n == 'd3dcompiler_47.dll' for _, n, _ in files):
        print('missing build/tu/d3dcompiler_47.dll (it is what makes Proton work)')
        return 1
    for path, _, _ in files:
        if not os.path.isfile(path):
            print('missing', path)
            return 1

    out = os.path.join(ROOT, 'out', 'FM2-Linux-%s.tar.gz' % argv[1])
    with tarfile.open(out, 'w:gz') as t:
        for path, name, mode in files:
            data = open(path, 'rb').read()
            if name.endswith('.sh'):
                data = data.replace(b'\r\n', b'\n')
            info = tarfile.TarInfo('FM2-Linux/' + name)
            info.size, info.mode = len(data), mode
            t.addfile(info, io.BytesIO(data))
        # The launcher's installed version: on Linux it can't read fm2.exe's version resource.
        data = argv[1].encode()
        info = tarfile.TarInfo('FM2-Linux/version.txt')
        info.size, info.mode = len(data), 0o644
        t.addfile(info, io.BytesIO(data))
    print('%s: %d files, %.1f MB' % (out, len(files), os.path.getsize(out) / 1e6))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

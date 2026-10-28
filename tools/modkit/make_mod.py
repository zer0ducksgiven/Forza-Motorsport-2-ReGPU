#!/usr/bin/env python3
"""make_mod.py - builds an FM2 mod package (an Xbox 360 STFS DLC package) from a folder, in one step.

  python make_mod.py <source folder> <out dir> --name "Display name" [--desc "text"] [--zip Name] [--pri N]

The source folder holds what goes inside the mod's zip, laid out as the game's files (media/db/patch/80000_execute.sql,
media/cars/..., ...). It is zipped (deflated, as Aiden's packs are) into Media/DLCZips/<Name>_pri_<N>.zip, any files in
<source folder>/../DLCZips-extra (zipmount.xml, stringtables.xml) are added beside it, any in
<source folder>/../LicenseMasks go to Media/LicenseMasks, and stfs_write.py packs that with
template.pkg's header. The package is written to <out dir>/<content id>: add that file in the launcher's Mods panel.
See docs/modding.md. stfs_*.py and template.pkg are Aiden's (FM2 modding tools), used with permission.
"""
import argparse, os, sys, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stfs_write  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source'); ap.add_argument('out_dir')
    ap.add_argument('--name', required=True, help='shown in the launcher and the game (up to 63 characters)')
    ap.add_argument('--desc', default='')
    ap.add_argument('--zip', help='the zip\'s name, letters and digits (default: the source folder\'s name)')
    ap.add_argument('--pri', type=int, default=67, help='the zip\'s priority (the _pri_ number; default 67)')
    a = ap.parse_args()
    src = os.path.abspath(a.source)
    if not os.path.isdir(os.path.join(src, 'media')):
        sys.exit('make_mod: %s has no media folder: lay it out as the game\'s files (media/db/patch/...)' % src)
    name = a.zip or os.path.basename(src.rstrip('/\\'))
    with tempfile.TemporaryDirectory() as tmp:
        dlczips = os.path.join(tmp, 'Media', 'DLCZips')
        os.makedirs(dlczips)
        zpath = os.path.join(dlczips, '%s_pri_%d.zip' % (name, a.pri))
        with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
            for root, dirs, files in os.walk(src):
                dirs.sort()
                rel = os.path.relpath(root, src).replace(os.sep, '/')
                if rel != '.':
                    z.writestr(rel + '/', b'')
                for f in sorted(files):
                    z.write(os.path.join(root, f), (f if rel == '.' else rel + '/' + f))
        extra = os.path.join(os.path.dirname(src), 'DLCZips-extra')
        if os.path.isdir(extra):
            for f in os.listdir(extra):
                with open(os.path.join(extra, f), 'rb') as i, open(os.path.join(dlczips, f), 'wb') as o:
                    o.write(i.read())
        # A car must be "licensed" to reach the dealership: Media/LicenseMasks/<bit>.sql marks it purchased and adds
        # it to StockGarage (see docs/modding.md). Files in <source folder>/../LicenseMasks go there.
        masks = os.path.join(os.path.dirname(src), 'LicenseMasks')
        if os.path.isdir(masks):
            os.makedirs(os.path.join(tmp, 'Media', 'LicenseMasks'))
            for f in os.listdir(masks):
                with open(os.path.join(masks, f), 'rb') as i, open(os.path.join(tmp, 'Media', 'LicenseMasks', f), 'wb') as o:
                    o.write(i.read())
        os.makedirs(a.out_dir, exist_ok=True)
        tmp_out = os.path.join(tmp, 'package')
        info = stfs_write.build(os.path.join(HERE, 'template.pkg'), tmp, tmp_out, name=a.name, desc=a.desc or a.name)
        out = os.path.join(a.out_dir, info['content_id'].upper())
        with open(tmp_out, 'rb') as i, open(out, 'wb') as o:
            o.write(i.read())
    print('built %s (%d bytes): add it in the launcher (Settings, Mods, Add mod)' % (out, info['size']))


if __name__ == '__main__':
    main()

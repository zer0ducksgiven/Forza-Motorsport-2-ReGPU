#!/usr/bin/env python3
"""stfs_extract.py - Python port of Extract-STFS.ps1 (https://github.com/jasonulbright/Extract-STFS).

Same algorithm (Velocity / Free60 STFS block math, file table walk, block chains),
for machines without PowerShell. Verified byte-identical against Extract-STFS output.

  python3 stfs_extract.py <package> <out dir>      extract
  python3 stfs_extract.py <package>                list
"""
import os, struct, sys

def extract(path, out_dir=None, log=print, resume=False):
    f = open(path, 'rb'); size = os.path.getsize(path)
    def rd(off, n): f.seek(off); return f.read(n)
    magic = rd(0, 4)
    if magic not in (b'CON ', b'LIVE', b'PIRS'): raise ValueError('Not an STFS file (magic: %r)' % magic)
    header_size = struct.unpack('>I', rd(0x340, 4))[0]
    first_ht = (header_size + 0xFFF) & 0xFFFFF000
    vd = rd(0x379, 8)
    block_sep = vd[2]
    shift = 0 if block_sep & 1 else 1
    ft_count = vd[3] | (vd[4] << 8)
    ft_block = vd[5] | (vd[6] << 8) | (vd[7] << 16)
    display = rd(0x411, 128).decode('utf-16-be', 'replace').rstrip('\0')

    def backing(b):
        r = (((b + 0xAA) // 0xAA) << shift) + b
        if b < 0xAA: return r
        if b < 0x70E4: return r + (((b + 0x70E4) // 0x70E4) << shift)
        return (1 << shift) + (r + (((b + 0x70E4) // 0x70E4) << shift))
    def addr(b): return (backing(b) << 12) + first_ht
    def l0hash(b):
        if b < 0xAA: return 0
        step0 = 0xAB if shift == 0 else 0xAC
        n = (b // 0xAA) * step0 + (((b // 0x70E4) + 1) << shift)
        return n if b // 0x70E4 == 0 else n + (1 << shift)
    def next_block(b):
        rec = (l0hash(b) << 12) + first_ht + (b % 0xAA) * 0x18
        x = rd(rec + 0x15, 3); n = (x[0] << 16) | (x[1] << 8) | x[2]
        return -1 if n >= 0xFFFFFF or n == 0xFFFFFE else n

    entries = []; dirs = {}; idx = 0; cur = ft_block; read = 0
    while read < ft_count:
        bo = addr(cur)
        for i in range(64):
            eo = bo + i * 0x40
            if eo + 0x40 > size: break
            e = rd(eo, 0x40)
            nl = e[0x28] & 0x3F
            if nl == 0 or any(c < 0x20 or c > 0x7E for c in e[:nl]): continue
            ent = dict(index=idx, name=e[:nl].decode('ascii'), isdir=bool(e[0x28] & 0x80), consec=bool(e[0x28] & 0x40),
                       nblocks=e[0x29] | (e[0x2A] << 8) | (e[0x2B] << 16), first=e[0x2F] | (e[0x30] << 8) | (e[0x31] << 16),
                       parent=(e[0x32] << 8) | e[0x33], size=struct.unpack('>I', e[0x34:0x38])[0])
            if ent['parent'] == 0xFFFF: ent['parent'] = -1
            entries.append(ent)
            if ent['isdir']: dirs[idx] = ent
            idx += 1
        read += 1
        if read < ft_count:
            nb = next_block(cur)
            if nb == -1: break
            cur = nb

    def relpath(ent):
        parts = [ent['name']]; p = ent['parent']
        while p != -1 and p in dirs:
            parts.insert(0, dirs[p]['name']); p = dirs[p]['parent']
        return parts

    log('Display name: %s  (%d entries)' % (display, len(entries)))
    files = []
    for ent in entries:
        rel = relpath(ent)
        if out_dir is None:
            log(('  DIR:  ' if ent['isdir'] else '  FILE: ') + '/'.join(rel) + ('' if ent['isdir'] else ' (%d bytes)' % ent['size']))
            continue
        target = os.path.join(out_dir, *rel)
        if ent['isdir']:
            os.makedirs(target, exist_ok=True); continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        files.append(('/'.join(rel), ent['size']))
        if resume and os.path.exists(target) and os.path.getsize(target) == ent['size'] and os.path.exists(target + '.ok'):
            continue
        with open(target, 'wb') as o:
            remaining = ent['size']; b = ent['first']; done = 0
            while remaining > 0 and done < ent['nblocks']:
                data = rd(addr(b), min(remaining, 0x1000))
                o.write(data); remaining -= len(data); done += 1
                if remaining > 0:
                    if ent['consec']: b += 1
                    else:
                        b = next_block(b)
                        if b == -1: log('WARNING: block chain ended early for ' + ent['name']); break
        if resume: open(target + '.ok', 'w').close()
    f.close()
    return display, files

if __name__ == '__main__':
    extract(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)

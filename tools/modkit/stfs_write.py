#!/usr/bin/env python3
"""stfs_write.py - repack a directory tree into an Xbox 360 STFS package using an existing package as the template
(header, thumbnails, title / content type kept; file table, data blocks, L0 / L1 / L2 hash tables and content id rebuilt).
 python3 stfs_write.py <template package> <source dir> <out file> [--name 'Display name'] [--desc 'text']
Layout verified against the template (43 k blocks: L0#g at g*171 (+sg+1, +1 if sg>0), L1#0 at 171, L1#s at s*170*171+s+1,
L2 at 170*171+1, file table = first data blocks).  The console signature (LIVE RSA) cannot be re-signed: it is left stale."""
import os, sys, struct, hashlib
BLK = 0x1000; PER = 170; SUPER = 0x70E4; HDR = 0xB000
def backing(b):
    r = ((b + 0xAA) // 0xAA) + b
    if b < 0xAA: return r
    if b < SUPER: return r + ((b + SUPER) // SUPER)
    return 1 + r + ((b + SUPER) // SUPER)
def l0pos(g):
    if g == 0: return 0
    sg = g // PER; return g * 171 + sg + 1 + (1 if sg > 0 else 0)
def l1pos(s): return 171 if s == 0 else s * PER * 171 + s + 1
L2POS = PER * 171 + 1
def collect(src):
    kids = {}
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src).replace('\\', '/'); rel = '' if rel == '.' else rel
        kids[rel] = sorted([(d, True) for d in dirs] + [(f, False) for f in files], key=lambda x: x[0].lower())
    return kids
def build(template, src, out, name=None, desc=None):
    hdr = bytearray(open(template, 'rb').read(HDR))
    kids = collect(src); ents = []         # (name, isdir, parent_index, relpath)
    queue = [('', -1)]
    while queue:
        rel, pidx = queue.pop(0)
        for nm, isd in kids.get(rel, []):
            p = (rel + '/' + nm) if rel else nm; ents.append((nm, isd, pidx, p))
            if isd: queue.append((p, len(ents) - 1))
    # children must be contiguous per directory: BFS order guarantees it
    nft = (len(ents) * 0x40 + BLK - 1) // BLK; blocks = [bytes(BLK)] * nft; start = {}
    for i, (nm, isd, pidx, p) in enumerate(ents):
        if isd: continue
        data = open(os.path.join(src, p), 'rb').read(); start[i] = (len(blocks), len(data))
        for k in range(0, max(len(data), 1) if data else 0, BLK):
            c = data[k:k + BLK]; blocks.append(c + bytes(BLK - len(c)))
    tab = bytearray(nft * BLK)
    for i, (nm, isd, pidx, p) in enumerate(ents):
        nb = nm.encode('ascii'); assert len(nb) <= 0x28, nm
        e = bytearray(0x40); e[:len(nb)] = nb; e[0x28] = len(nb) | 0x40 | (0x80 if isd else 0)
        if not isd:
            s, sz = start[i]; n = (sz + BLK - 1) // BLK
            e[0x29:0x2C] = n.to_bytes(3, 'little'); e[0x2C:0x2F] = n.to_bytes(3, 'little'); e[0x2F:0x32] = s.to_bytes(3, 'little'); e[0x34:0x38] = struct.pack('>I', sz)
        e[0x32:0x34] = struct.pack('>H', 0xFFFF if pidx < 0 else pidx); e[0x38:0x3C] = bytes.fromhex('2d6f0020'); e[0x3C:0x40] = bytes.fromhex('2d6f0020')
        tab[i * 0x40:(i + 1) * 0x40] = e
    for k in range(nft): blocks[k] = bytes(tab[k * BLK:(k + 1) * BLK])
    n = len(blocks); nL0 = (n + PER - 1) // PER; nL1 = (nL0 + PER - 1) // PER if nL0 > 1 else 0; hasL2 = nL1 > 1
    phys = n + nL0 + nL1 + (1 if hasL2 else 0); img = bytearray(phys * BLK)
    for b, c in enumerate(blocks): o = backing(b) * BLK; img[o:o + BLK] = c
    sha = lambda o: hashlib.sha1(bytes(img[o * BLK:(o + 1) * BLK])).digest()
    for g in range(nL0):
        t = bytearray(BLK)
        for k in range(PER):
            b = g * PER + k
            if b >= n: break
            nxt = b + 1 if b + 1 < n else 0xFFFFFF
            t[k * 0x18:k * 0x18 + 0x18] = hashlib.sha1(blocks[b]).digest() + b'\x80' + nxt.to_bytes(3, 'big')
        o = l0pos(g) * BLK; img[o:o + BLK] = t
    for s in range(nL1):
        t = bytearray(BLK)
        for k in range(PER):
            g = s * PER + k
            if g >= nL0: break
            t[k * 0x18:k * 0x18 + 20] = sha(l0pos(g))
        o = l1pos(s) * BLK; img[o:o + BLK] = t
    if hasL2:
        t = bytearray(BLK)
        for s in range(nL1): t[s * 0x18:s * 0x18 + 20] = sha(l1pos(s))
        o = L2POS * BLK; img[o:o + BLK] = t; top = sha(L2POS)
    elif nL1: top = sha(l1pos(0))
    else: top = sha(0)
    vd = bytearray(hdr[0x379:0x379 + 0x24]); vd[3:5] = nft.to_bytes(2, 'little'); vd[5:8] = (0).to_bytes(3, 'little'); vd[8:28] = top
    vd[28:32] = struct.pack('>I', n); vd[32:36] = struct.pack('>I', 0); hdr[0x379:0x379 + 0x24] = vd
    hdr[0x34C:0x354] = struct.pack('>Q', len(img))
    if name: nm = name.encode('utf-16-be')[:0x7E]; hdr[0x411:0x411 + 0x80] = nm + bytes(0x80 - len(nm))
    if desc: ds = desc.encode('utf-16-be')[:0x7E]; hdr[0xD11:0xD11 + 0x80] = ds + bytes(0x80 - len(ds))
    cid = hashlib.sha1(bytes(hdr[0x344:HDR])).digest(); hdr[0x32C:0x340] = cid
    open(out, 'wb').write(bytes(hdr) + bytes(img))
    return dict(entries=len(ents), data_blocks=n, nft=nft, L0=nL0, L1=nL1, L2=hasL2, size=HDR + len(img), content_id=cid.hex() + '4D')
if __name__ == '__main__':
    a = sys.argv[1:]; kw = {}
    for k in ('--name', '--desc'):
        if k in a: i = a.index(k); kw[k[2:]] = a[i + 1]; del a[i:i + 2]
    print(build(a[0], a[1], a[2], **kw))

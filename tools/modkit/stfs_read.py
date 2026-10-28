"""stfs_read.py - minimal STFS reader: list / read single files without extracting the package."""
import struct
BLK = 0x1000
def backing(b):
    r = ((b + 0xAA) // 0xAA) + b
    if b < 0xAA: return r
    if b < 0x70E4: return r + ((b + 0x70E4) // 0x70E4)
    return 1 + r + ((b + 0x70E4) // 0x70E4)
class Stfs:
    def __init__(s, path):
        s.f = open(path, 'rb'); h = s.f.read(0x400); hs = struct.unpack('>I', h[0x340:0x344])[0]; s.first = (hs + 0xFFF) & ~0xFFF
        vd = h[0x379:0x379 + 0x24]; s.nft = vd[3] | vd[4] << 8; s.ftb = vd[5] | vd[6] << 8 | vd[7] << 16
        s.name = h[0x411:0x411 + 0x80].decode('utf-16-be', 'replace').rstrip('\0')
        s.cache = {}
        raw = b''.join(s.block(s.ftb + k) for k in range(s.nft)); s.ents = []
        for i in range(len(raw) // 0x40):
            e = raw[i * 0x40:(i + 1) * 0x40]; nl = e[0x28] & 0x3F
            if nl == 0: break
            s.ents.append(dict(name=e[:nl].decode('latin1'), isdir=bool(e[0x28] & 0x80), n=e[0x29] | e[0x2A] << 8 | e[0x2B] << 16,
                               start=e[0x2F] | e[0x30] << 8 | e[0x31] << 16, parent=struct.unpack('>H', e[0x32:0x34])[0], size=struct.unpack('>I', e[0x34:0x38])[0]))
        for i, e in enumerate(s.ents):
            p = e['parent']; parts = [e['name']]
            while p != 0xFFFF: parts.append(s.ents[p]['name']); p = s.ents[p]['parent']
            e['path'] = '/'.join(reversed(parts))
    def block(s, b):
        s.f.seek(s.first + backing(b) * BLK); return s.f.read(BLK)
    def read(s, path):
        e = next(x for x in s.ents if x['path'].lower() == path.lower()); out = b''.join(s.block(e['start'] + k) for k in range(e['n']))
        return out[:e['size']]
    def files(s): return [e['path'] for e in s.ents if not e['isdir']]

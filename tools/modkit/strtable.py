"""FM2 string tables (.str, "LSB2"): read, add strings, write. Worked out from the game's own tables (2026-10-27).

Layout (big-endian after the first 12 bytes):
  'LSB2', u32le 1, u32le 2, u32 0x24, u32 4, then four section offsets: strings (A), symbols (B), X, key order (C);
  0 when a table has none. X (a few UI tables) isn't decoded: it's kept as it is.
  A: u32 size, u32 count, count x (u16 key, u32 offset in UTF-16 units), u16 0xFFFF, u32 total units,
     then the strings, UTF-16BE, each ending in 0, and one more 0. Entries are sorted by key (the game looks keys up by search).
  B: u32 size, u32 count, count x (u16 key, u16 index, u32 offset) sorted by key, then the symbol names (ASCII,
     0-ended) in index order; index is the symbol's 1-based position in its source (new strings go last).
  C: u32 size, u32 count, count x u16 key, in symbol order.
A string is referenced from the database as '_&<n>': n = table prefix << 16 | key; every entry of one table shares
the prefix (CareerEventTypes 0x1AF2, Events 0x9814, ...).

  python strtable.py <file.str>                 lists it
  python strtable.py --check <file.str>...      parses and rebuilds each, and says whether the bytes match
"""
import struct, sys


class Table:
    def __init__(s, data):
        assert data[:4] == b'LSB2'
        s.head = data[:0x14]
        a, b, x, c = struct.unpack_from('>4I', data, 0x14)
        s.x_raw = data[x:c or len(data)] if x else b''  # a third section some UI tables have: kept as it is
        size, n = struct.unpack_from('>II', data, a)
        ents = [struct.unpack_from('>HI', data, a + 8 + 6 * i) for i in range(n)]
        end_mark, total = struct.unpack_from('>HI', data, a + 8 + 6 * n)
        assert end_mark == 0xFFFF
        text = a + 8 + 6 * n + 6
        s.strings = {}
        for k, off in ents:
            o = text + 2 * off; e = o
            while data[e:e + 2] != b'\0\0': e += 2
            s.strings[k] = data[o:e].decode('utf-16-be')
        s.order_a = [k for k, _ in ents]
        size_b, nb = struct.unpack_from('>II', data, b)
        bents = [struct.unpack_from('>HHI', data, b + 8 + 8 * i) for i in range(nb)]
        names = b + 8 + 8 * nb
        s.symbols = {}; s.index = {}
        for k, idx, off in bents:
            s.index[k] = idx
            e = data.index(b'\0', names + off)
            s.symbols[k] = data[names + off:e].decode('ascii')
        s.order_b = [k for k, _, _ in bents]
        s.has_c = c != 0
        s.tail = data[c + struct.unpack_from('>I', data, c)[0]:] if c else b''

    def add(s, key, text, symbol):
        assert key not in s.strings, 'key %04x is taken' % key
        s.strings[key] = text; s.symbols[key] = symbol; s.index[key] = max(s.index.values(), default=0) + 1

    def free_key(s, start=0x0100):
        k = start
        while k in s.strings: k += 1
        return k

    def build(s, keep_order=False):
        keys = s.order_a if keep_order and set(s.order_a) == set(s.strings) else sorted(s.strings)
        units, offs = b'', {}
        for k in keys:
            offs[k] = len(units) // 2
            units += s.strings[k].encode('utf-16-be') + b'\0\0'
        a = struct.pack('>II', 0, len(keys)) + b''.join(struct.pack('>HI', k, offs[k]) for k in keys)
        a += struct.pack('>HI', 0xFFFF, len(units) // 2) + units
        a += bytes(2)  # the strings end with one more empty string
        a = struct.pack('>I', len(a)) + a[4:]
        syms = sorted(s.symbols, key=lambda k: s.index[k])  # the symbols' own order (as in the source file)
        bkeys = s.order_b if keep_order and set(s.order_b) == set(s.symbols) else sorted(s.symbols)
        names, noff = b'', {}
        for k in syms:
            noff[k] = len(names); names += s.symbols[k].encode('ascii') + bytes(1)
        idx = {k: i + 1 for i, k in enumerate(syms)}
        bb = struct.pack('>II', 0, len(bkeys)) + b''.join(struct.pack('>HHI', k, idx[k], noff[k]) for k in bkeys) + names
        bb = struct.pack('>I', len(bb)) + bb[4:]
        cc = struct.pack('>II', 8 + 2 * len(syms), len(syms)) + b''.join(struct.pack('>H', k) for k in syms) if s.has_c else b''
        a_off = 0x24; b_off = a_off + len(a); x_off = b_off + len(bb); c_off = x_off + len(s.x_raw)
        return (s.head + struct.pack('>4I', a_off, b_off, x_off if s.x_raw else 0, c_off if s.has_c else 0)
                + a + bb + s.x_raw + cc + s.tail)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[0] == '--check':
        for f in a[1:]:
            d = open(f, 'rb').read(); t = Table(d)
            print('%s: %d strings, rebuilt %s' % (f, len(t.strings), 'identical' if t.build(keep_order=True) == d else 'DIFFERENT'))
    else:
        t = Table(open(a[0], 'rb').read())
        for k in sorted(t.strings):
            print('%04x %-28s %s' % (k, t.symbols.get(k, ''), t.strings[k][:80].encode('ascii', 'replace').decode()))

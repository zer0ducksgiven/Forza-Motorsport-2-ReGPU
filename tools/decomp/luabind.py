"""Names Lua binding handlers from their registration calls: a call with a string in r4 (the name Lua sees) and
a function address in r5 (the handler), as tolua_function(L, "name", handler) and the game's own registrations
make. -> out/decomp/luabind.json {handler: [names]}, which symbols.py uses.

  luabind.py      reads out/tu4/image.bin and out/decomp/xrefs.json
"""
import json
import struct

BASE = 0x82000000
TEXT = (0x821E0000, 0x821E0000 + 0x0078BC08)


def main():
    img = open('out/tu4/image.bin', 'rb').read()
    x = json.load(open('out/decomp/xrefs.json'))
    starts = {int(a, 16) for a in x}

    def string_at(a):
        o = a - BASE
        if not 0 <= o < len(img):
            return None
        end = img.find(b'\0', o, o + 80)
        s = img[o:end]
        return s.decode() if 2 <= len(s) and all(32 < c < 127 for c in s) else None

    found = {}
    for a, f in x.items():
        start, size = int(a, 16), f['size']
        hi, val = {}, {}
        for p in range(start, start + size, 4):
            w = struct.unpack_from('>I', img, p - BASE)[0]
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 15 and ra == 0:  # lis
                hi[rd] = (imm << 16) & 0xFFFFFFFF
                val.pop(rd, None)
            elif op == 14 and ra in hi:  # addi rd, ra(lis), lo
                val[rd] = (hi[ra] + simm) & 0xFFFFFFFF
            elif op == 18 and (w & 3) == 1:  # bl: a registration if r4 is a name and r5 a function
                name, fn = string_at(val.get(4, 0)), val.get(5)
                if name and fn in starts and TEXT[0] <= fn < TEXT[1]:
                    found.setdefault('%08X' % fn, []).append(name)
                val.clear()
    json.dump(found, open('out/decomp/luabind.json', 'w'), indent=0)
    print(len(found), 'handlers named by their registrations')


if __name__ == '__main__':
    main()

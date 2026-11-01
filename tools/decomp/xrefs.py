"""Functions and what they reference: calls, strings and data addresses, from the TU4 image.

  xrefs.py [image.bin] [func_mapping.cpp] [out.json]
      defaults: out/tu4/image.bin, out/ppc_tu4/ppc_func_mapping.cpp -> out/decomp/xrefs.json

Function starts come from XenonRecomp's mapping (the functions it recompiled); a function runs to the next
start. Addresses are found the way the compiler builds them: lis rX,hi then addi/ori/load off rX (a small
per-register tracker, reset at each function). bl gives calls. Strings are ASCII runs of 4+ bytes, ending in 0.
"""
import bisect
import json
import os
import re
import struct
import sys

BASE = 0x82000000


def main(argv):
    path = argv[1] if len(argv) > 1 else 'out/tu4/image.bin'
    mapping = argv[2] if len(argv) > 2 else 'out/ppc_tu4/ppc_func_mapping.cpp'
    out = argv[3] if len(argv) > 3 else 'out/decomp/xrefs.json'
    img = open(path, 'rb').read()
    starts = sorted({int(a, 16) for a in re.findall(r'\{ 0x([0-9A-F]{8}), sub_', open(mapping).read())})

    def string_at(a):
        o = a - BASE
        if not 0 <= o < len(img):
            return None
        end = img.find(b'\0', o, o + 512)
        if end - o < 4:
            return None
        s = img[o:end]
        if all(32 <= c < 127 or c in (9, 10, 13) for c in s):
            return s.decode()
        return None

    funcs = {}
    for n, start in enumerate(starts):
        end = starts[n + 1] if n + 1 < len(starts) else start + 4
        hi = {}
        calls, strings, data = set(), set(), set()
        for a in range(start, end, 4):
            w = struct.unpack_from('>I', img, a - BASE)[0]
            op = w >> 26
            rd, ra = (w >> 21) & 31, (w >> 16) & 31
            imm = w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 15 and ra == 0:  # lis
                hi[rd] = (imm << 16) & 0xFFFFFFFF
                continue
            if op == 18 and (w & 3) == 1:  # bl
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                calls.add((a + li) & 0xFFFFFFFF)
                continue
            target = None
            if ra in hi:
                if op == 14:  # addi
                    target = (hi[ra] + simm) & 0xFFFFFFFF
                elif op == 24:  # ori
                    target = hi[ra] | imm
                elif op in (32, 34, 40, 42, 48, 50):  # lwz lbz lhz lha lfs lfd
                    target = (hi[ra] + simm) & 0xFFFFFFFF
            if target is not None and 0x82000000 <= target < BASE + len(img):
                s = string_at(target)
                if s is not None:
                    strings.add(s)
                else:
                    data.add(target)
            if op in (14, 15, 24, 32, 34, 40, 42) and rd in hi and rd != ra:
                hi.pop(rd, None)
        funcs['%08X' % start] = {'size': end - start, 'calls': sorted('%08X' % c for c in calls),
                                 'strings': sorted(strings), 'data': sorted('%08X' % d for d in data)}
    os.makedirs(os.path.dirname(out), exist_ok=True)
    json.dump(funcs, open(out, 'w'))
    withs = sum(1 for f in funcs.values() if f['strings'])
    print(f'{len(funcs)} functions, {withs} reference strings -> {out}')


if __name__ == '__main__':
    sys.exit(main(sys.argv))

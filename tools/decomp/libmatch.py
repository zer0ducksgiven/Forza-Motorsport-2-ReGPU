"""Functions FM2 shares with PGR4: the Xbox SDK libraries and C runtime both games were linked with.

  libmatch.py      -> out/decomp/shared_pgr4.json (FM2 TU4 addresses)

Each function is reduced to its instruction words with the parts that move between executables masked: branch
targets, and the 16-bit immediates of lis/addi/ori/loads/stores (addresses of globals). Functions under 16 bytes
are skipped (too many trivial collisions). A signature found in both games marks the FM2 function as shared.
"""
import hashlib
import json
import re
import struct
import sys

BASE = 0x82000000
GAMES = {
    'fm2': ('out/tu4/image.bin', 'out/ppc_tu4/ppc_func_mapping.cpp'),
    'pgr4': ('D:/PGR4-ReGPU/out/tu2/default_tu2.bin', 'D:/PGR4-ReGPU/out/ppc/ppc_func_mapping.cpp'),
}
IMM = {14, 15, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55}


def masked(w):
    op = w >> 26
    if op == 18:
        return w & 0xFC000003
    if op == 16:
        return w & 0xFFFF0003
    if op in IMM:
        return w & 0xFFFF0000
    return w


def signatures(image, mapping):
    img = open(image, 'rb').read()
    starts = sorted({int(a, 16) for a in re.findall(r'\{ 0x([0-9A-F]{8}), sub_', open(mapping).read())})
    out = {}
    for n, s in enumerate(starts[:-1]):
        size = starts[n + 1] - s
        if size < 16 or size > 0x10000 or s - BASE + size > len(img):
            continue
        words = struct.unpack_from('>%dI' % (size // 4), img, s - BASE)
        out['%08X' % s] = hashlib.sha1(struct.pack('>%dI' % len(words), *map(masked, words))).hexdigest()
    return out


def main():
    fm2 = signatures(*GAMES['fm2'])
    pgr4 = set(signatures(*GAMES['pgr4']).values())
    shared = sorted(a for a, h in fm2.items() if h in pgr4)
    json.dump(shared, open('out/decomp/shared_pgr4.json', 'w'))
    print(f'{len(shared)} of {len(fm2)} FM2 functions also in PGR4 -> out/decomp/shared_pgr4.json')


if __name__ == '__main__':
    sys.exit(main())

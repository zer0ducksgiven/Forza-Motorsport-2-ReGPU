"""C++ classes from the MSVC RTTI left in the image: class names, base classes, vtables and the virtual
functions in them.

  rtti.py [image.bin] [out.json]       default out/tu4/image.bin -> out/decomp/rtti.json

The image is flat from 0x82000000 and big-endian. On the 360 every RTTI pointer is a plain 32-bit address:
  TypeDescriptor      { vtable of type_info, spare, ".?AVName@Namespace@@" }
  CompleteObjectLocator { signature, offset, cdOffset, TypeDescriptor*, ClassHierarchyDescriptor* }
  ClassHierarchyDescriptor { signature, attributes, numBases, BaseClassDescriptor** }
  BaseClassDescriptor { TypeDescriptor*, numContained, mdisp, pdisp, vdisp, attributes }
A vtable is preceded by its locator's address, so the word after any word that points at a locator starts a
vtable; its entries run while they point into code.
"""
import json
import os
import re
import struct
import sys

BASE = 0x82000000
TEXT = (0x821E0000, 0x821E0000 + 0x0078BC08)


def demangle(name):
    """.?AVCCamFree@Camera@@ -> Camera::CCamFree (names only; templates kept as written)."""
    m = re.match(r'\.\?A[VUW](.*)@@$', name)
    if not m:
        return name
    if '?$' in m.group(1):  # templates: not worth a full demangler, keep the mangled body
        return m.group(1)
    parts = [p for p in m.group(1).split('@') if p]
    return '::'.join(reversed(parts))


def main(argv):
    path = argv[1] if len(argv) > 1 else 'out/tu4/image.bin'
    out = argv[2] if len(argv) > 2 else 'out/decomp/rtti.json'
    img = open(path, 'rb').read()

    def u32(a):
        o = a - BASE
        return struct.unpack_from('>I', img, o)[0] if 0 <= o <= len(img) - 4 else None

    def code(a):
        return a is not None and TEXT[0] <= a < TEXT[1] and a % 4 == 0

    # type descriptors: the name string, 8 bytes after the descriptor's start
    types = {}
    for m in re.finditer(rb'\.\?A[VUW][A-Za-z0-9_@?$<>,]+@@\x00', img):
        td = BASE + m.start() - 8
        types[td] = m.group()[:-1].decode()

    # locators: a word pointing at a type descriptor, 12 bytes into a 20-byte record with signature 0
    words = struct.unpack('>%dI' % (len(img) // 4), img[:len(img) // 4 * 4])
    locators = {}
    for i, w in enumerate(words):
        if w in types and i >= 3 and words[i - 3] == 0:
            col = BASE + (i - 3) * 4
            locators[col] = (w, words[i - 2], words[i + 1])  # type, offset of this subobject, hierarchy

    classes = {}
    for td, name in types.items():
        classes[td] = {'mangled': name, 'name': demangle(name), 'bases': [], 'vtables': []}

    for col, (td, offset, chd) in locators.items():
        cls = classes[td]
        if not cls['bases'] and u32(chd + 8) is not None:
            n, arr = u32(chd + 8), u32(chd + 12)
            if n is not None and n < 64 and arr:
                for k in range(1, n):  # entry 0 is the class itself
                    bcd = u32(arr + 4 * k)
                    btd = u32(bcd) if bcd else None
                    if btd in classes:
                        cls['bases'].append(classes[btd]['name'])

    # vtables: the word before each points at a locator
    for i, w in enumerate(words):
        if w in locators and code(words[i + 1] if i + 1 < len(words) else None):
            vt = BASE + (i + 1) * 4
            td, offset, _ = locators[w]
            funcs = []
            j = i + 1
            while j < len(words) and code(words[j]) and (j == i + 1 or words[j] not in locators):
                funcs.append(words[j])
                j += 1
                if j < len(words) and words[j] in locators:
                    break
            classes[td]['vtables'].append({'address': '%08X' % vt, 'offset': offset,
                                           'functions': ['%08X' % f for f in funcs]})

    result = sorted(classes.values(), key=lambda c: c['name'])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    json.dump(result, open(out, 'w'), indent=1)
    nv = sum(len(c['vtables']) for c in result)
    nf = len({f for c in result for v in c['vtables'] for f in v['functions']})
    print(f'{len(result)} classes, {len(locators)} locators, {nv} vtables, {nf} distinct virtual functions -> {out}')


if __name__ == '__main__':
    sys.exit(main(sys.argv))

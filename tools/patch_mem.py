#!/usr/bin/env python3
"""Route the recompiled code's memory accesses that skip PPC_LOAD/PPC_STORE through macros of their own.

  patch_mem.py <recompiled sources dir>

XenonRecomp writes vector loads and stores, lwarx/stwcx. and dcbz straight to guest memory
(simde_mm_load_si128/simde_mm_store_si128 on base + ea, a plain load, __sync_bool_compare_and_swap,
memset). They become FM2_VLOAD, FM2_VSTORE, FM2_RESERVE32/64, FM2_CAS and FM2_MEMSET (src/ppc_hooks.h),
which are exactly those calls in a normal build; an FM2_STORE_LOG build points them at the lifted
functions' check overlay and store log (src/decomp/lift.h) like the scalar loads and stores.

Idempotent: files already patched are left alone. CMake runs it when configuring.
"""
import os
import re
import sys

RULES = [
    ('simde_mm_load_si128((simde__m128i*)(base + ', 'FM2_VLOAD((simde__m128i*)(base + '),
    ('simde_mm_store_si128((simde__m128i*)(base + ', 'FM2_VSTORE((simde__m128i*)(base + '),
    ('__sync_bool_compare_and_swap(reinterpret_cast<uint', 'FM2_CAS(reinterpret_cast<uint'),
    ('\tmemset(base + ', '\tFM2_MEMSET(base + '),
]
RESERVE = re.compile(r'^(\treserved\.u(32|64) = )\*\(uint\2_t\*\)\((base \+ [^\n]*)\);$', re.M)


def main():
    root = sys.argv[1]
    changed = 0
    for name in sorted(os.listdir(root)):
        if not (name.startswith('ppc_recomp.') and name.endswith('.cpp')):
            continue
        path = os.path.join(root, name)
        with open(path, encoding='utf-8', newline='') as f:
            text = f.read()
        new = text
        for old, rep in RULES:
            new = new.replace(old, rep)
        new = RESERVE.sub(lambda m: '%sFM2_RESERVE%s((uint%s_t*)(%s));' % (m.group(1), m.group(2), m.group(2), m.group(3)), new)
        if new != text:
            with open(path, 'w', encoding='utf-8', newline='') as f:
                f.write(new)
            changed += 1
    print('patched %d files' % changed)


main()

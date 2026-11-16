#!/usr/bin/env python3
"""Route the recompiled code's timebase reads through Fm2Timebase().

  patch_mftb.py <recompiled sources dir>

Only for older XenonRecomp output: ReXenon emits PPC_MFTB(), which src/ppc_hooks.h maps to Fm2Timebase().
XenonRecomp turns mftb into the host's raw __rdtsc(): 3-5 GHz, counted since the PC booted.
The console's timebase is 49.875 MHz from boot. The title multiplies ticks by 1,000,000 in 64 bits
to get microseconds, which overflows within the first minutes of PC uptime with the raw counter, so
its microsecond clock (loading tips, timers) ran wild. Fm2Timebase() (src/kernel/runtime.cpp)
returns 49.875 MHz ticks since the process started, and KeQueryPerformanceFrequency says so too.

Idempotent: files already patched are left alone. Run it again after regenerating the sources
(CMake does it when configuring).
"""
import os
import re
import sys

DECL = 'uint64_t Fm2Timebase(); // (tools/patch_mftb.py)\n'


def main():
    root = sys.argv[1]
    changed = 0
    for name in sorted(os.listdir(root)):
        if not (name.startswith('ppc_recomp.') and name.endswith('.cpp')):
            continue
        path = os.path.join(root, name)
        with open(path, encoding='utf-8', newline='') as f:
            text = f.read()
        if '__rdtsc()' not in text:
            continue
        new = text.replace('__rdtsc()', 'Fm2Timebase()')
        if DECL not in new:
            # After the first #include line.
            m = re.search(r'^#include [^\n]*\n', new, re.M)
            new = new[:m.end()] + DECL + new[m.end():]
        with open(path, 'w', encoding='utf-8', newline='') as f:
            f.write(new)
        changed += 1
    print('patched %d files' % changed)


main()

"""Each recompiled function's real inputs: the argument registers (r3-r10, f1-f13) it reads before writing them.

  py -3 tools/decomp/livein.py      -> out/decomp/livein.json {address: [registers]}

Ghidra's C names a call's arguments from its own guess at the callee, and it drops some: lift.py compares a call
with the registers the callee really reads (this) and the caller really sets up. Worked out by liveness over the
XenonRecomp output (out/ppc_tu4), whose comments give each instruction and whose C gives what it reads and writes:
a call reads what the callee reads and leaves the volatile registers undefined, repeated over the whole program
until nothing changes. Kernel imports read what their C++ in src/kernel reads; a call through a pointer reads
r3-r10 and f1-f13 (anything could be behind it).
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# argument registers as bits: r3-r10 -> 0-7, f1-f13 -> 8-20
def bit(kind, n):
    if kind == 'r' and 3 <= n <= 10:
        return 1 << (n - 3)
    if kind == 'f' and 1 <= n <= 13:
        return 1 << (8 + n - 1)
    return 0


ALL = (1 << 21) - 1
REG = re.compile(r'\bctx\.([rf])(\d+)\.')
WRITE = re.compile(r'^\s*ctx\.([rf])(\d+)\.\w+\s*=[^=]')
CALL = re.compile(r'\b(sub_[0-9A-F]{8}|__imp__\w+)\(ctx, base\)')


def names(bits):
    out = []
    for i in range(8):
        if bits >> i & 1:
            out.append('r%d' % (i + 3))
    for i in range(13):
        if bits >> (8 + i) & 1:
            out.append('f%d' % (i + 1))
    return out


def parse(body):
    """[(reads, writes, calls, gotos, ends)] per instruction, and {label: index}"""
    instrs, labels = [], {}
    cur = None
    for line in body.split('\n'):
        s = line.strip()
        m = re.match(r'^(loc_[0-9A-F]+):$', s)
        if m:
            labels[m.group(1)] = len(instrs)
            continue
        if s.startswith('// '):
            cur = {'reads': 0, 'writes': 0, 'calls': [], 'gotos': [], 'ends': False, 'cond': False, 'indirect': False}
            instrs.append(cur)
            continue
        if cur is None or not s:
            continue
        w = WRITE.match(s)
        rhs = s.split('=', 1)[1] if w else s
        for k, n in REG.findall(rhs):
            cur['reads'] |= bit(k, int(n)) & ~cur['writes']
        if w:
            cur['writes'] |= bit(w.group(1), int(w.group(2)))
        for c in CALL.findall(s):
            cur['calls'].append(c)
        if 'PPC_CALL_INDIRECT_FUNC' in s:
            cur['indirect'] = True
        for g in re.findall(r'goto (loc_[0-9A-F]+);', s):
            cur['gotos'].append(g)
        if s.startswith('if (') or s.startswith('case ') or s.startswith('switch'):
            cur['cond'] = True
        if s == 'return;' or (s.startswith('goto ') and not cur['cond']):
            cur['ends'] = True
    return instrs, labels


VOLATILE = ALL  # a call leaves every argument register undefined (r3 and f1 hold its result)


def livein(instrs, labels, summary, imports):
    n = len(instrs)
    succ = []
    for i, ins in enumerate(instrs):
        s = [labels[g] for g in ins['gotos'] if g in labels]
        if not ins['ends'] and i + 1 < n:
            s.append(i + 1)
        succ.append(s)
    gen, kill = [], []
    for ins in instrs:
        g, k = ins['reads'], ins['writes']
        for c in ins['calls']:
            if c.startswith('sub_'):
                g |= summary.get(int(c[4:], 16), 0) & ~k
            elif c.startswith('__imp__') and not c.startswith('__imp__sub_'):
                g |= imports.get(c[7:], ALL) & ~k
            k |= VOLATILE
        if ins['indirect']:
            g |= ALL & ~k
            k |= VOLATILE
        gen.append(g)
        kill.append(k)
    live = [0] * n
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            out = 0
            for j in succ[i]:
                out |= live[j]
            v = gen[i] | (out & ~kill[i])
            if v != live[i]:
                live[i] = v
                changed = True
    return live[0] if n else 0


def import_inputs():
    """{import name: registers its C++ reads} from src/kernel's GUEST_EXPORT bodies"""
    out = {}
    for d, _, files in os.walk(os.path.join(ROOT, 'src')):
        for f in files:
            if not f.endswith('.cpp'):
                continue
            t = open(os.path.join(d, f), encoding='utf-8', errors='replace').read()
            for m in re.finditer(r'GUEST_EXPORT\((\w+)\)\s*\{(.*?)\n\}', t, re.S):
                bits = 0
                for k, n in REG.findall(m.group(2)):
                    bits |= bit(k, int(n))
                if 'StackArg' in m.group(2) or 'ctx, base)' in m.group(2):
                    bits = ALL  # stack arguments, or hands the call on
                out[m.group(1)] = bits
    return out


def main():
    ppc = os.path.join(ROOT, 'out', 'ppc_tu4')
    funcs = {}
    for f in sorted(os.listdir(ppc)):
        if f.startswith('ppc_recomp.') and f.endswith('.cpp'):
            t = open(os.path.join(ppc, f), encoding='utf-8', errors='replace').read()
            for m in re.finditer(r'PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\) \{(.*?)\n\}', t, re.S):
                funcs[int(m.group(1), 16)] = parse(m.group(2))
    imports = import_inputs()
    print('%d functions, %d imports' % (len(funcs), len(imports)), flush=True)
    summary = {}
    for rnd in range(12):
        changed = 0
        for a, (instrs, labels) in funcs.items():
            v = livein(instrs, labels, summary, imports)
            if v != summary.get(a, 0):
                summary[a] = v
                changed += 1
        print('round %d: %d changed' % (rnd + 1, changed), flush=True)
        if not changed:
            break
    out = os.path.join(ROOT, 'out', 'decomp', 'livein.json')
    json.dump({'%08X' % a: names(v) for a, v in summary.items()}, open(out, 'w'))
    print('wrote', out)


if __name__ == '__main__':
    sys.exit(main())

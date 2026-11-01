"""Each recompiled function's real inputs: the argument registers (r3-r10, f1-f13) it reads before writing them.

  py -3 tools/decomp/livein.py      -> out/decomp/livein.json {address: [registers]}, and livein.txt for Ghidra
                                       (ghidra/ApplyParamsFm2.java)

Ghidra's C names a call's arguments from its own guess at the callee, and it drops some: lift.py compares a call
with the registers the callee really reads (this) and the caller really sets up. Worked out by liveness over the
XenonRecomp output (out/ppc_tu4), whose comments give each instruction and whose C gives what it reads and writes:
a call reads what the callee reads and leaves the volatile registers undefined, repeated over the whole program
until nothing changes. Kernel imports read what their C++ in src/kernel reads; a call through a pointer reads
r3-r10 (anything could be behind it, but a float argument passed through untouched is rare, and counting them all
made a third of the game's functions take 21 parameters).
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
INTS = (1 << 8) - 1  # r3-r10
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


VOLATILE = ALL  # a call through a pointer or to an import leaves every argument register undefined


def clobbers(funcs):
    """{address: argument registers the function can change}, its own writes and its callees'. With link-time code
    generation a caller relies on a callee leaving the rest as they were (827F5390 keeps its r3 in r6 across a call
    and passes it on): a direct call kills only these."""
    own, calls = {}, {}
    for a, (instrs, _) in funcs.items():
        w, cs = 0, set()
        for ins in instrs:
            w |= ins['writes']
            if ins['indirect']:
                w |= VOLATILE
            for c in ins['calls']:
                if c.startswith('sub_'):
                    cs.add(int(c[4:], 16))
                else:
                    w |= VOLATILE
        own[a], calls[a] = w, cs
    out = dict(own)
    changed = True
    while changed:
        changed = False
        for a, cs in calls.items():
            v = out[a]
            for c in cs:
                v |= out.get(c, VOLATILE)
            if v != out[a]:
                out[a] = v
                changed = True
    return out


def livein(instrs, labels, summary, imports, clob=None):
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
                k |= clob.get(int(c[4:], 16), VOLATILE) if clob is not None else VOLATILE
            else:
                if c.startswith('__imp__') and not c.startswith('__imp__sub_'):
                    g |= imports.get(c[7:], ALL) & ~k
                k |= VOLATILE
        if ins['indirect']:
            g |= INTS & ~k
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
    # what the code after each direct call reads before writing: the callee's results it uses, beyond r3/f1 often
    # (with link-time code generation a caller can know which volatile registers a callee leaves set)
    after = {}
    for i, ins in enumerate(instrs):
        for c in ins['calls']:
            if c.startswith('sub_'):
                a = int(c[4:], 16)
                if ins['ends']:  # a tail call: the callee's results are this function's
                    after.setdefault(a, set()).add('tail')
                else:
                    v = 0
                    for j in succ[i]:
                        v |= live[j]
                    after.setdefault(a, set()).add(v)
    return (live[0] if n else 0), after


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
    clob = clobbers(funcs)
    print('%d functions, %d imports; a direct call kills %.1f argument registers on average (of 21)'
          % (len(funcs), len(imports), sum(bin(v).count('1') for v in clob.values()) / max(1, len(clob))), flush=True)
    summary = {}
    for rnd in range(40):
        changed = 0
        for a, (instrs, labels) in funcs.items():
            v = livein(instrs, labels, summary, imports, clob)[0]
            if v != summary.get(a, 0):
                summary[a] = v
                changed += 1
        print('round %d: %d changed' % (rnd + 1, changed), flush=True)
        if not changed:
            break
    out = os.path.join(ROOT, 'out', 'decomp', 'livein.json')
    json.dump({'%08X' % a: names(v) for a, v in summary.items()}, open(out, 'w'))
    # for ghidra/ApplyParamsFm2.java
    with open(os.path.join(ROOT, 'out', 'decomp', 'livein.txt'), 'w') as f:
        for a, v in sorted(summary.items()):
            f.write(' '.join(['%08X' % a] + names(v)) + '\n')
    # liveout.json: {callee: registers some caller reads after it returns} (r3 and f1 included: for a void function
    # they aren't results either); a tail call passes its caller's on
    uses, tails = {}, {}
    for a, (instrs, labels) in funcs.items():
        for c, vs in livein(instrs, labels, summary, imports, clob)[1].items():
            for v in vs:
                if v == 'tail':
                    tails.setdefault(a, set()).add(c)
                else:
                    uses[c] = uses.get(c, 0) | v
    changed = True
    while changed:
        changed = False
        for a, cs in tails.items():
            for c in cs:
                v = uses.get(c, 0) | uses.get(a, 0)
                if v != uses.get(c, 0):
                    uses[c] = v
                    changed = True
    json.dump({'%08X' % a: names(v) for a, v in uses.items() if v}, open(os.path.join(ROOT, 'out', 'decomp', 'liveout.json'), 'w'))
    print('wrote', out, 'livein.txt and liveout.json')


if __name__ == '__main__':
    sys.exit(main())

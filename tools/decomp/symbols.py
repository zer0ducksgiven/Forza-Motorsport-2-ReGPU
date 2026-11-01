"""Names and sorts every recompiled function into out/decomp/symbols.json (+ symbols.txt, one per line), from:

  - RTTI vtables (rtti.py): Class::vf<N> for each virtual function slot
  - Lua bindings: the 32-byte start-up functions that register a name and a handler (handler -> lua_<Name>)
  - Named constants: the 72-byte start-up functions that build a hashed name ("height") -> init_<name>
  - Libraries: functions referencing a library's source path or messages (FMOD, Microsoft's shader compiler,
    the XTS networking client, R1 fonts, Bink, Lua), widened over contiguous runs (an object file's code is
    linked in one piece, so functions between two of the same library's belong to it)
  - Our hooks: every sub_XXXXXXXX named in src/ (TU addresses), with the file that uses it

  symbols.py            needs out/decomp/rtti.json and xrefs.json (run rtti.py and xrefs.py first)
"""
import collections
import glob
import json
import os
import re
import sys

LIBS = [
    ('fmod', lambda s: 'fmod' in s),
    ('xgraphics', lambda s: 'xgraphics' in s or '\\ucode\\' in s),
    ('xts', lambda s: 'xtsclient' in s or 'bencode' in s),
    ('r1font', lambda s: 'fontsandtext' in s or 'fontcache' in s),
    ('bink', lambda s: 'bink' in s),
    ('lua', lambda s: bool(re.search(r"attempt to (call|index|compare|perform|concatenate)|c stack overflow|'__index'|"
                                     r"_loaded|bad argument #|lua_|lual_|'for' (initial|limit|step)", s))),
]
GAP = 24  # functions between two of the same library's that are taken as the library's too


def main():
    x = json.load(open('out/decomp/xrefs.json'))
    rtti = json.load(open('out/decomp/rtti.json'))
    by_name = {c['name']: c for c in rtti}
    addrs = sorted(x, key=lambda a: int(a, 16))
    sym = {a: {'name': 'sub_' + a, 'kind': 'unknown', 'size': x[a]['size']} for a in addrs}

    # start-up block: Lua bindings and named constants
    for a in addrs:
        f = x[a]
        if not 0x82830000 <= int(a, 16) < 0x82960000:
            continue
        if f['size'] == 72 and len(f['strings']) == 1:
            sym[a].update(name='init_' + re.sub(r'\W', '_', f['strings'][0])[:60], kind='init')
        elif f['size'] == 32 and len(f['strings']) == 1:
            sym[a].update(name='luareg_' + f['strings'][0], kind='init')
            handler = next((d for d in f['data'] if d in sym), None)
            if handler and sym[handler]['kind'] == 'unknown':
                sym[handler].update(name='lua_' + f['strings'][0], kind='lua')
        else:
            sym[a]['kind'] = 'init'

    # what each Lua binding runs: a handler that calls one function besides the Lua API helpers every handler
    # uses names it (lua_IsProfileLoaded -> IsProfileLoaded), unless other handlers call it too
    handlers = [a for a in addrs if sym[a]['kind'] == 'lua']
    use = collections.Counter(c for a in handlers for c in x[a]['calls'])
    helpers = {c for c, n in use.items() if n >= 8}
    for a in handlers:
        rest = [c for c in x[a]['calls'] if c not in helpers]
        if len(rest) == 1 and use[rest[0]] == 1 and rest[0] in sym and sym[rest[0]]['kind'] == 'unknown':
            sym[rest[0]].update(name=sym[a]['name'][len('lua_'):], kind='luaimpl')

    # handlers named by their registration calls (luabind.py): lua.<the name Lua sees>, since several share
    # plain names like Save or new
    if os.path.exists('out/decomp/luabind.json'):
        for a, names in json.load(open('out/decomp/luabind.json')).items():
            ok = [n for n in names if re.fullmatch(r'[.A-Za-z_][A-Za-z0-9_:!.]*', n)]
            if a in sym and sym[a]['kind'] == 'unknown' and len(set(ok)) == 1:
                sym[a].update(name='lua.' + ok[0].replace('::', '.'), kind='luaimpl')

    # virtual functions. One that several classes share (inherited, not overridden) is named after the class
    # the others derive from when there is one, else marked shared: naming it after whichever class came first
    # read as if, say, a database getter belonged to CAudioThread.
    owners = collections.defaultdict(list)
    for c in rtti:
        for v in c['vtables']:
            for i, f in enumerate(v['functions']):
                owners[f].append((c, i, v['offset']))
    for f, own in owners.items():
        if f not in sym or sym[f]['kind'] != 'unknown':
            continue
        names = {c['name'] for c, _, _ in own}
        c, i, offset = own[0]
        if len(names) > 1:
            base = next((o for o in own if all(o[0]['name'] == n or o[0]['name'] in by_name[n]['bases'] for n in names)), None)
            if base is None:
                sym[f].update(name='shared::vf%d_%d_classes' % (i, len(names)), kind='virtual')
                continue
            c, i, offset = base
        tag = '' if offset == 0 else '@%X' % offset
        sym[f].update(name='%s::vf%d%s' % (c['name'], i, tag), kind='virtual')

    # libraries: direct evidence, then contiguous runs
    lib = {}
    for a in addrs:
        for s in x[a]['strings']:
            l = s.lower()
            hit = next((n for n, test in LIBS if test(l)), None)
            if hit:
                lib[a] = hit
                break
    # SQLite (the game's database: careers, garages, cars): its messages, in its own address range only, since the
    # game's wrapper around it (near 825E0000) mentions PRAGMA and journals too
    sqlite = re.compile(r"sqlite|no such table|syntax error|database is locked|malformed|SQL logic error|"
                        r"unrecognized token|misuse of aggregate|no such column|PRAGMA|journal", re.I)
    for a in addrs:
        if 0x826F0000 <= int(a, 16) < 0x82740000 and any(sqlite.search(s) for s in x[a]['strings']):
            lib.setdefault(a, 'sqlite')
    hits = sorted(int(a, 16) for a in lib if lib[a] == 'sqlite')
    if hits:  # SQLite is one contiguous block: everything between its first and last messages but Lua bindings
        for a in addrs:
            if hits[0] <= int(a, 16) <= hits[-1] and not any('in function' in t for t in x[a]['strings']):
                lib.setdefault(a, 'sqlite')
    for c in rtti:
        if c['name'].startswith('FMOD'):
            for v in c['vtables']:
                for f in v['functions']:
                    lib.setdefault(f, 'fmod')
    for a in addrs:
        if 0x8296C800 <= int(a, 16) < 0x8297CF4C:  # the BINK section
            lib[a] = 'bink'
    tagged = [i for i, a in enumerate(addrs) if a in lib]
    for i, j in zip(tagged, tagged[1:]):
        if lib[addrs[i]] == lib[addrs[j]] and j - i <= GAP:
            for k in range(i + 1, j):
                lib.setdefault(addrs[k], lib[addrs[i]])
    for a, n in lib.items():
        if sym[a]['kind'] in ('unknown', 'virtual'):
            sym[a]['lib'] = n
            if sym[a]['kind'] == 'unknown':
                sym[a]['kind'] = 'library'

    # our hooks: src/ names disc addresses (the TU build remaps them), so map each through out/tu4/map.json
    disc = {f[0]: f[2] for f in json.load(open('out/tu4/map.json'))['functions']}
    for path in glob.glob('src/**/*.cpp', recursive=True) + glob.glob('src/**/*.h', recursive=True):
        for a in set(re.findall(r'sub_(8[0-9A-F]{7})', open(path, errors='replace').read())):
            tu = '%08X' % disc[int(a, 16)] if int(a, 16) in disc else a
            if tu in sym:
                sym[tu].setdefault('hooked', []).append(os.path.relpath(path).replace('\\', '/'))

    # code shared with PGR4 (libmatch.py): the Xbox SDK and C runtime. Not widened over runs, and only from 48
    # bytes: small helpers (std containers, accessors) match by chance and sit among the game's own code.
    if os.path.exists('out/decomp/shared_pgr4.json'):
        for a in json.load(open('out/decomp/shared_pgr4.json')):
            if a in sym and sym[a]['kind'] == 'unknown' and sym[a]['size'] >= 48 and not any(
                    '@@' in t or 'Forza' in t or 'Volume' in t for t in x[a]['strings']):  # game names: same shape, not SDK
                sym[a].update(kind='library', lib='xdk')

    # the hand-kept names (names.txt) win over everything worked out here
    for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'names.txt')):
        f = line.split('#')[0].split()
        if len(f) >= 2 and f[0] in sym:
            sym[f[0]].update(name=f[1], kind='named')

    os.makedirs('out/decomp', exist_ok=True)
    json.dump(sym, open('out/decomp/symbols.json', 'w'), indent=0)
    with open('out/decomp/symbols.txt', 'w') as t:
        for a in addrs:
            s = sym[a]
            t.write('%s %6d %-8s %-9s %s\n' % (a, s['size'], s['kind'], s.get('lib', ''), s['name']))

    kinds = collections.Counter(s['kind'] for s in sym.values())
    size = collections.Counter()
    for s in sym.values():
        size[s['kind']] += s['size']
    libs = collections.Counter(s['lib'] for s in sym.values() if 'lib' in s)
    total = sum(size.values())
    print('%-9s %7s %8s' % ('kind', 'funcs', 'code %'))
    for k, n in kinds.most_common():
        print('%-9s %7d %7.1f%%' % (k, n, 100 * size[k] / total))
    print('libraries:', ', '.join('%s %d' % kv for kv in libs.most_common()))
    print('hooked by us:', sum(1 for s in sym.values() if 'hooked' in s))


if __name__ == '__main__':
    sys.exit(main())

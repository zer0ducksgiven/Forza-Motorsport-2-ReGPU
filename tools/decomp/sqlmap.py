"""Which functions run which SQL: every SQL string in the executable, the functions that reference it, and their
names (symbols.py). Most of the career, garage and shop logic is SQL against gamedb.slt and the career database,
so this reads as a map of it.

  sqlmap.py       -> out/decomp/sql_map.md (game text: keep it out of the repository)
"""
import collections
import json
import re


def main():
    x = json.load(open('out/decomp/xrefs.json'))
    s = json.load(open('out/decomp/symbols.json'))
    sql = re.compile(r'^\s*(SELECT|INSERT|UPDATE|DELETE|CREATE|ATTACH|DROP|REPLACE|BEGIN|COMMIT)\b', re.I)
    users = collections.defaultdict(list)
    for a, f in x.items():
        for t in f['strings']:
            if sql.match(t):
                users[t].append(a)
    tables = collections.defaultdict(set)
    lines = ['# SQL in FM2 (TU4)', '', '%d statements in %d functions.' % (len(users), len({a for v in users.values() for a in v})), '']
    for q in sorted(users, key=lambda q: (re.findall(r'(?:FROM|INTO|UPDATE|JOIN)\s+%?s?(\w+)', q) or [''])[0]):
        for t in re.findall(r'(?:FROM|INTO|UPDATE|JOIN|TABLE)\s+(?:%s)?\[?(\w+)', q):
            tables[t].update(users[q])
        who = ', '.join('%s %s' % (a, s[a]['name']) for a in users[q])
        lines += ['```sql', q.strip(), '```', '- ' + who, '']
    lines += ['## Tables and the functions that use them', '']
    for t in sorted(tables, key=lambda t: -len(tables[t])):
        lines.append('- **%s**: %s' % (t, ', '.join(sorted(s[a]['name'] for a in tables[t]))))
    open('out/decomp/sql_map.md', 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    print(len(users), 'statements,', len(tables), 'tables -> out/decomp/sql_map.md')


if __name__ == '__main__':
    main()

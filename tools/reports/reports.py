"""FM2 reports: the launcher's bug reports and shared play.logs in the private repo (server/reports files them).

Everything goes through the GitHub CLI (`gh`, already logged in), so no token is kept here. Triage state lives
on GitHub as labels and comments: the desk (app.py) and this CLI see the same thing.

  reports.py list [--all] [--label L]     issues, newest first
  reports.py show <n>                     an issue with its hardware, comments and log path
  reports.py log <n | path> [--errors]    the play.log of an issue, or a log by its path
  reports.py logs [--install ID]          shared logs, newest first
  reports.py stats                        hardware and versions across all logs
  reports.py label <n> <label> [--remove]
  reports.py close <n> [reason] | reopen <n>
  reports.py comment <n> <text>
"""
import json
import re
import subprocess
import sys
import threading

REPO = 'zer0ducksgiven/fm2-reports'
# Triage labels the desk offers (created on first use).
STATUSES = {
    'needs info': 'fbca04',
    'confirmed': 'd93f0b',
    'fixed next': '0e8a16',
    'duplicate': 'cfd3d7',
    'not a bug': 'cfd3d7',
}


class GhError(Exception):
    pass


def gh(path, method='GET', body=None, raw=False):
    """A GitHub API call through gh. path is relative to the repo unless it starts with '/'."""
    url = path if path.startswith('/') else f'/repos/{REPO}/{path}'
    cmd = ['gh', 'api', '-X', method, url]
    if raw:
        cmd += ['-H', 'Accept: application/vnd.github.raw']
    if body is not None:
        cmd += ['--input', '-']
    p = subprocess.run(cmd, input=json.dumps(body).encode() if body is not None else None, capture_output=True)
    if p.returncode != 0:
        raise GhError((p.stderr or p.stdout).decode('utf-8', 'replace').strip()[:400])
    out = p.stdout.decode('utf-8', 'replace')
    return out if raw else (json.loads(out) if out.strip() else None)


def gh_pages(path):
    items, page = [], 1
    while True:
        sep = '&' if '?' in path else '?'
        batch = gh(f'{path}{sep}per_page=100&page={page}')
        items += batch
        if len(batch) < 100:
            return items
        page += 1


# ---- issues

ROW = re.compile(r'^\| (.+?) \| (.*) \|$', re.M)
LOG_LINK = re.compile(r'play\.log: \[([^\]]+)\]')


def parse_issue(i):
    """The Worker's issue body: the quoted description, a table of fields, the log link."""
    body = i.get('body') or ''
    fields = {k.strip().lower(): v.strip() for k, v in ROW.findall(body) if k.strip() not in ('', '---')}
    description = '\n'.join(l[2:] if l.startswith('> ') else l[1:] for l in body.splitlines() if l.startswith('>'))
    m = LOG_LINK.search(body)
    return {
        'number': i['number'],
        'title': i['title'],
        'state': i['state'],
        'stateReason': i.get('state_reason'),
        'labels': [l['name'] for l in i.get('labels', [])],
        'created': i['created_at'],
        'updated': i['updated_at'],
        'closed': i.get('closed_at'),
        'comments': i.get('comments', 0),
        'url': i['html_url'],
        'description': description,
        'version': fields.get('version', ''),
        'os': fields.get('os', ''),
        'gpu': fields.get('gpu', ''),
        'driver': fields.get('gpu driver', ''),
        'zeroCopy': fields.get('zero-copy', ''),
        'install': fields.get('install', ''),
        'contact': fields.get('contact', ''),
        'report': fields.get('report', ''),
        'log': m.group(1) if m else None,
    }


def issues(state='all'):
    return [parse_issue(i) for i in gh_pages(f'issues?state={state}') if 'pull_request' not in i]


def issue(n):
    i = parse_issue(gh(f'issues/{n}'))
    i['commentList'] = [{'author': c['user']['login'], 'created': c['created_at'], 'body': c['body']}
                        for c in gh_pages(f'issues/{n}/comments')]
    return i


def ensure_label(name):
    try:
        gh(f'labels/{name}')
    except GhError:
        gh('labels', 'POST', {'name': name, 'color': STATUSES.get(name, 'ededed')})


def add_label(n, name):
    ensure_label(name)
    gh(f'issues/{n}/labels', 'POST', {'labels': [name]})


def remove_label(n, name):
    gh(f'issues/{n}/labels/{name}', 'DELETE')


def set_state(n, state, reason=None):
    body = {'state': state}
    if reason:
        body['state_reason'] = reason  # completed | not_planned
    gh(f'issues/{n}', 'PATCH', body)


def comment(n, text):
    gh(f'issues/{n}/comments', 'POST', {'body': text})


# ---- logs

_log_cache = {}
_cache_lock = threading.Lock()
LOG_NAME = re.compile(r'^logs/(\d{8})/([0-9a-z]+)-([0-9a-f]+)\.log$')


def logs():
    """Every stored log, newest first: path, date, install, id."""
    try:
        tree = gh('git/trees/main?recursive=1')['tree']
    except GhError:
        return []
    out = []
    for t in tree:
        m = LOG_NAME.match(t['path'])
        if m:
            out.append({'path': t['path'], 'date': m.group(1), 'install': m.group(2), 'id': m.group(3), 'size': t.get('size', 0)})
    out.sort(key=lambda l: (l['date'], l['path']), reverse=True)
    return out


def log_text(path):
    if not LOG_NAME.match(path):
        raise GhError('Not a log path.')
    with _cache_lock:
        if path in _log_cache:
            return _log_cache[path]
    text = gh(f'contents/{path}', raw=True)
    with _cache_lock:
        _log_cache[path] = text  # logs never change once stored
    return text


ERROR = re.compile(r'error|fail|crash|exception|assert|not mappable|dropped|timed out|hang|stall', re.I)


def log_summary(text):
    """What a log says about its PC and session: the boot's system lines, errors, fps."""
    system, errors, fps = [], [], []
    for line in text.splitlines():
        body = line[line.find(']') + 1:].strip() if line.startswith('[') else line
        if body.startswith('system:') and len(system) < 40:
            system.append(body[7:].strip())
        if ERROR.search(body) and len(errors) < 200:
            errors.append(line)
        m = re.match(r'perf: ([\d.]+) fps', body)
        if m:
            fps.append(float(m.group(1)))
    def field(prefix):
        for s in system:
            if s.lower().startswith(prefix):
                return s[len(prefix):].strip(' :')
        return ''
    # several GPUs: the one in use is marked; "Name: vendor:device rev ..., driver x" -> name and driver
    gpu = field('gpu (used)') or field('gpu')
    gpu_name, _, rest = gpu.partition(':')
    driver = rest.rsplit('driver', 1)[1].strip() if 'driver' in rest else ''
    return {
        'system': system,
        'errors': errors,
        'cpu': field('cpu').split(',')[0],
        'gpu': gpu_name.strip(),
        'driver': driver,
        'os': field('os'),
        'wine': field('wine'),
        'version': field('forza motorsport 2 regpu'),
        'settings': field('settings'),
        'zeroCopy': None if 'guest memory' not in text else ('not mappable' not in text),
        'fps': {'samples': len(fps), 'median': sorted(fps)[len(fps) // 2] if fps else None},
        'lines': text.count('\n'),
    }


# ---- CLI

def _arg(args, name):
    if name in args:
        i = args.index(name)
        return args[i + 1] if i + 1 < len(args) else ''
    return None


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd, args = argv[1], argv[2:]
    if cmd == 'list':
        rows = issues('all' if '--all' in args else 'open')
        label = _arg(args, '--label')
        for i in rows:
            if label and label not in i['labels']:
                continue
            tags = ','.join(l for l in i['labels'] if l != 'bug report')
            print(f"#{i['number']:<4} {i['state']:<6} {i['created'][:10]} {i['version']:<7} {i['gpu'][:28]:<28} {i['title'][:70]}  [{tags}]")
    elif cmd == 'show':
        i = issue(int(args[0]))
        print(f"#{i['number']} {i['title']} ({i['state']}{', ' + i['stateReason'] if i['stateReason'] else ''})")
        print(f"labels: {', '.join(i['labels'])}\n")
        print(i['description'] + '\n')
        for k in ('version', 'os', 'gpu', 'driver', 'zeroCopy', 'install', 'contact', 'report', 'log'):
            if i[k]:
                print(f'{k:>9}: {i[k]}')
        for c in i['commentList']:
            print(f"\n-- {c['author']} {c['created'][:16]}\n{c['body']}")
    elif cmd == 'log':
        target = args[0]
        path = issue(int(target.lstrip('#')))['log'] if target.lstrip('#').isdigit() else target
        if not path:
            print('No log attached.')
            return 1
        text = log_text(path)
        if '--errors' in args:
            s = log_summary(text)
            print('\n'.join(s['system']) + '\n')
            print('\n'.join(s['errors']))
        else:
            sys.stdout.write(text)
    elif cmd == 'logs':
        install = _arg(args, '--install')
        for l in logs():
            if not install or l['install'] == install:
                print(f"{l['date']} {l['install']} {l['size']:>8}  {l['path']}")
    elif cmd == 'stats':
        from collections import Counter
        all_logs = logs()
        installs = {}
        for l in all_logs:
            installs.setdefault(l['install'], l)  # newest per install
        gpus, oses, zc = Counter(), Counter(), Counter()
        for l in installs.values():
            s = log_summary(log_text(l['path']))
            gpus[s['gpu'] or '?'] += 1
            oses[s['os'] or '?'] += 1
            zc[str(s['zeroCopy'])] += 1
        print(f'{len(all_logs)} logs from {len(installs)} installs\n')
        for title, c in (('GPU', gpus), ('OS', oses), ('zero-copy', zc)):
            print(title)
            for k, v in c.most_common():
                print(f'  {v:>4}  {k}')
    elif cmd == 'label':
        n, name = int(args[0]), args[1]
        remove_label(n, name) if '--remove' in args else add_label(n, name)
    elif cmd == 'close':
        n = int(args[0])
        if len(args) > 1:
            comment(n, ' '.join(args[1:]))
        set_state(n, 'closed', 'completed')
    elif cmd == 'reopen':
        set_state(int(args[0]), 'open')
    elif cmd == 'comment':
        comment(int(args[0]), ' '.join(args[1:]))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv))
    except GhError as e:
        print('gh:', e, file=sys.stderr)
        sys.exit(1)

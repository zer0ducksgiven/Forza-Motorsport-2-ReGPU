"""The reports desk: a local page for triaging FM2 bug reports and reading shared play.logs.

  py tools/reports/app.py [port]      then open http://127.0.0.1:8790 (reports-desk.cmd does both)

Serves desk.html and a JSON API over reports.py, so anything the page does is a gh call on the private repo.
Only listens on 127.0.0.1. The same API is handy from scripts:
  GET  /api/issues?state=all|open|closed      GET /api/issue/<n>
  GET  /api/logs                              GET /api/log?path=logs/...   (text and summary)
  GET  /api/stats                             hardware across installs (newest log of each)
  POST /api/issue/<n>  {"state": "open"|"closed", "reason"?, "addLabel"?, "removeLabel"?, "comment"?}
  POST /api/log/delete {"path": "logs/..."}

Logs with nothing in them (reports.is_empty) are deleted from the repo in the background as the log list loads.
"""
import json
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reports  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
_stats = {'at': 0, 'data': None}
_checked = set()  # logs already read for the empty check
_empty = set()    # empty logs found (deleted, or being deleted)
_purge_lock = threading.Lock()


def purge_empty(all_logs):
    """Reads each log not yet checked and deletes the empty ones. Runs in the background."""
    if not _purge_lock.acquire(blocking=False):
        return
    try:
        todo = [l for l in all_logs if l['path'] not in _checked]
        def check(l):
            try:
                empty = reports.is_empty(reports.log_text(l['path']))
            except reports.GhError:
                return
            _checked.add(l['path'])
            if empty:
                _empty.add(l['path'])
                try:
                    reports.delete_log(l['path'], l['sha'])
                    print(f'Deleted empty log {l["path"]}', flush=True)
                except reports.GhError as e:
                    print(f'Could not delete empty log {l["path"]}: {e}', flush=True)
        with ThreadPoolExecutor(8) as pool:
            list(pool.map(check, todo))
        if todo:
            _stats['at'] = 0
    finally:
        _purge_lock.release()


def logs():
    all_logs = reports.logs()
    threading.Thread(target=purge_empty, args=(all_logs,), daemon=True).start()
    return [l for l in all_logs if l['path'] not in _empty]


def stats():
    # Each install's newest log that names its hardware (a session can end before the system lines),
    # summarised (logs are cached once read); at most once a minute. Installs that never logged their
    # hardware (most never got the game running) are only counted, with the reason their newest log gives.
    if _stats['data'] and time.time() - _stats['at'] < 60:
        return _stats['data']
    per_install = {}
    all_logs = logs()
    for l in all_logs:
        per_install.setdefault(l['install'], []).append(l)  # newest first
    def summary(install_logs):
        for l in install_logs:
            s = reports.log_summary(reports.log_text(l['path']))
            if s['gpu'] or s['os']:
                return {'install': l['install'], 'date': l['date'], 'path': l['path'], 'gpu': s['gpu'], 'cpu': s['cpu'], 'os': s['os'],
                        'wine': s['wine'], 'version': s['version'], 'zeroCopy': s['zeroCopy'], 'fps': s['fps']['median'],
                        'settings': s['settings'], 'errors': len(s['errors'])}
        text = reports.log_text(install_logs[0]['path'])
        fatal = next((line[line.find(']') + 1:].strip() for line in text.splitlines() if 'FATAL' in line), '')
        return {'install': install_logs[0]['install'], 'noHardware': True, 'reason': 'title update missing' if 'title update' in fatal else 'other'}
    with ThreadPoolExecutor(8) as pool:
        results = list(pool.map(summary, per_install.values()))
    data = {'logs': len(all_logs), 'installs': [r for r in results if not r.get('noHardware')],
            'noHardware': [r for r in results if r.get('noHardware')]}
    _stats.update(at=time.time(), data=data)
    return data


class Handler(BaseHTTPRequestHandler):
    def send(self, code, body, kind='application/json'):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header('content-type', kind)
        self.send_header('cache-control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        try:
            if u.path in ('/', '/index.html'):
                with open(os.path.join(HERE, 'desk.html'), 'rb') as f:
                    return self.send(200, f.read(), 'text/html; charset=utf-8')
            if u.path == '/api/issues':
                return self.send(200, reports.issues(q.get('state', 'all')))
            if u.path.startswith('/api/issue/'):
                return self.send(200, reports.issue(int(u.path.rsplit('/', 1)[1])))
            if u.path == '/api/logs':
                # Each log with when it arrived and its PC (the CPU and GPU its install logged).
                rows = logs()
                try:
                    times = reports.log_times()
                except reports.GhError:
                    times = {}
                hw = {i['install']: i for i in stats()['installs']}
                for l in rows:
                    l['time'] = times.get(l['id'])
                    h = hw.get(l['install'])
                    l['cpu'], l['gpu'] = (h['cpu'], h['gpu']) if h else ('', '')
                return self.send(200, rows)
            if u.path == '/api/log':
                text = reports.log_text(q.get('path', ''))
                return self.send(200, {'path': q['path'], 'text': text, 'summary': reports.log_summary(text)})
            if u.path == '/api/stats':
                return self.send(200, stats())
            return self.send(404, {'error': 'Not found.'})
        except reports.GhError as e:
            return self.send(502, {'error': str(e)})
        except (ValueError, KeyError) as e:
            return self.send(400, {'error': str(e)})

    def do_POST(self):
        u = urlparse(self.path)
        # The page only, not other sites the browser has open (they can't set this header without a preflight).
        if self.headers.get('x-desk') != '1':
            return self.send(403, {'error': 'Forbidden.'})
        try:
            body = json.loads(self.rfile.read(int(self.headers.get('content-length') or 0)) or b'{}')
            if u.path.startswith('/api/issue/'):
                n = int(u.path.rsplit('/', 1)[1])
                if body.get('comment'):
                    reports.comment(n, body['comment'])
                if body.get('addLabel'):
                    reports.add_label(n, body['addLabel'])
                if body.get('removeLabel'):
                    reports.remove_label(n, body['removeLabel'])
                if body.get('state'):
                    reports.set_state(n, body['state'], body.get('reason'))
                return self.send(200, reports.issue(n))
            if u.path == '/api/log/delete':
                reports.delete_log(body['path'])
                _empty.add(body['path'])  # keeps it out of the list until GitHub's tree catches up
                _stats['at'] = 0
                return self.send(200, {'deleted': body['path']})
            return self.send(404, {'error': 'Not found.'})
        except reports.GhError as e:
            return self.send(502, {'error': str(e)})
        except (ValueError, KeyError) as e:
            return self.send(400, {'error': str(e)})

    def log_message(self, *a):
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8790
    print(f'Reports desk: http://127.0.0.1:{port}  (Ctrl+C to stop)', flush=True)
    ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()


if __name__ == '__main__':
    main()

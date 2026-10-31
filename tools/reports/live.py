"""FM2 Live for the desk: the Worker's status and activity (GET /admin/status) and today's Cloudflare usage against
the free plan's daily limits (Cloudflare's GraphQL analytics API).

Two tokens, each a file in %LOCALAPPDATA%\\fm2-reports, never in the repo:
  live-admin.token      the Worker's ADMIN_TOKEN secret (server/live: npx wrangler secret put ADMIN_TOKEN)
  cloudflare-api.token  a Cloudflare API token with Account Analytics: Read (dash.cloudflare.com, My Profile,
                        API Tokens, Create Custom Token)
Requests go through curl: this PC's Python has an expired certificate store.
"""
import json
import os
import subprocess
import time
from datetime import datetime, timezone

SERVER = os.environ.get('FM2_LIVE_URL', 'https://fm2-live.fm2-reports.workers.dev')
ACCOUNT = 'ec627d123dc696d13551efdd4039c656'
DIR = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'fm2-reports')

# The free plan's daily allowances (reset at midnight UTC). Durable Object WebSocket messages count 20 to a request.
LIMITS = {
    'workerRequests': (100_000, 'Worker requests'),
    'doRequests': (100_000, 'Durable Object requests'),
    'doGbSeconds': (13_000, 'Durable Object awake time (GB-s)'),
    'd1RowsRead': (5_000_000, 'D1 rows read'),
    'd1RowsWritten': (100_000, 'D1 rows written'),
}

_cache = {'status': (0, None), 'usage': (0, None)}


def token(name):
    env = {'live-admin.token': 'FM2_LIVE_ADMIN_TOKEN', 'cloudflare-api.token': 'FM2_CLOUDFLARE_TOKEN'}[name]
    if os.environ.get(env):  # scripts and testing against wrangler dev
        return os.environ[env]
    try:
        with open(os.path.join(DIR, name), encoding='utf-8') as f:
            return f.read().strip()
    except OSError:
        return ''


def curl(url, headers=(), data=None):
    cmd = ['curl', '-s', '-m', '20', '-w', '\n%{http_code}', url]
    for h in headers:
        cmd += ['-H', h]
    if data is not None:
        cmd += ['--data-binary', '@-']
    p = subprocess.run(cmd, input=data, capture_output=True, text=True, encoding='utf-8',
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    body, _, code = p.stdout.rpartition('\n')
    return int(code or 0), body


def status():
    """The Worker's /admin/status, or {'error': ...}. Cached 20 seconds."""
    at, data = _cache['status']
    if data and time.time() - at < 20:
        return data
    t = token('live-admin.token')
    if not t:
        return {'error': f'No admin token: put the Worker\'s ADMIN_TOKEN in {os.path.join(DIR, "live-admin.token")}.'}
    code, body = curl(SERVER + '/admin/status', [f'Authorization: Bearer {t}'])
    data = json.loads(body) if code == 200 else {'error': f'FM2 Live answered {code or "nothing"}: {body[:200]}'}
    _cache['status'] = (time.time(), data)
    return data


QUERIES = {
    'workerRequests': ('workersInvocationsAdaptive', 'datetime', 'requests', lambda s: s['requests']),
    'doRequests': ('durableObjectsInvocationsAdaptiveGroups', 'datetime', 'requests', lambda s: s['requests']),
    'doPeriodic': ('durableObjectsPeriodicGroups', 'datetime', 'activeTime inboundWebsocketMsgCount', None),
    'd1': ('d1AnalyticsAdaptiveGroups', 'date', 'rowsRead rowsWritten', None),
}


def usage():
    """Today's (UTC) usage against LIMITS: {'metrics': [...], 'errors': [...]} or {'error': ...}. Cached 5 minutes."""
    at, data = _cache['usage']
    if data and time.time() - at < 300:
        return data
    t = token('cloudflare-api.token')
    if not t:
        return {'error': f'No Cloudflare API token: create one with Account Analytics: Read and put it in {os.path.join(DIR, "cloudflare-api.token")}.'}
    now = datetime.now(timezone.utc)
    start = now.replace(hour=0, minute=0, second=0, microsecond=0)
    sums, errors = {}, []
    for key, (dataset, kind, fields, _) in QUERIES.items():
        f = (f'{{date_geq: "{start:%Y-%m-%d}", date_leq: "{now:%Y-%m-%d}"}}' if kind == 'date' else
             f'{{datetime_geq: "{start:%Y-%m-%dT%H:%M:%SZ}", datetime_leq: "{now:%Y-%m-%dT%H:%M:%SZ}"}}')
        q = f'{{ viewer {{ accounts(filter: {{accountTag: "{ACCOUNT}"}}) {{ {dataset}(limit: 10000, filter: {f}) {{ sum {{ {fields} }} }} }} }} }}'
        code, body = curl('https://api.cloudflare.com/client/v4/graphql',
                          [f'Authorization: Bearer {t}', 'Content-Type: application/json'], json.dumps({'query': q}))
        try:
            r = json.loads(body)
        except ValueError:
            errors.append(f'{dataset}: HTTP {code}')
            continue
        if r.get('errors'):
            errors.append(f'{dataset}: {r["errors"][0].get("message", "error")}')
            continue
        groups = ((r.get('data') or {}).get('viewer') or {}).get('accounts') or [{}]
        total = {}
        for g in groups[0].get(dataset) or []:
            for k, v in (g.get('sum') or {}).items():
                total[k] = total.get(k, 0) + (v or 0)
        sums[key] = total
    values = {}
    if 'workerRequests' in sums:
        values['workerRequests'] = sums['workerRequests'].get('requests', 0)
    if 'doRequests' in sums or 'doPeriodic' in sums:
        values['doRequests'] = sums.get('doRequests', {}).get('requests', 0) + sums.get('doPeriodic', {}).get('inboundWebsocketMsgCount', 0) / 20
    if 'doPeriodic' in sums:  # activeTime is microseconds; objects are billed at 128 MB
        values['doGbSeconds'] = sums['doPeriodic'].get('activeTime', 0) / 1e6 * 0.125
    if 'd1' in sums:
        values['d1RowsRead'] = sums['d1'].get('rowsRead', 0)
        values['d1RowsWritten'] = sums['d1'].get('rowsWritten', 0)
    metrics = [{'key': k, 'label': label, 'used': round(values[k], 1), 'limit': limit,
                'percent': round(100 * values[k] / limit, 1)} for k, (limit, label) in LIMITS.items() if k in values]
    # How far through the UTC day it is, to tell a busy morning from a day that will run out
    day = (now - start).total_seconds() / 86400
    data = {'at': time.time(), 'dayFraction': round(day, 3), 'metrics': metrics, 'errors': errors}
    _cache['usage'] = (time.time(), data)
    return data

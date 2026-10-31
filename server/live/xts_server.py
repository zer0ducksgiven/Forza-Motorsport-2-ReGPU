"""Stand-in FM2 title server: Xbox Live title services (XtsClient, the auction house) over TCP. The port's
title server lookup gives the game 127.0.0.1 ("xetv.b.fm2"); its connections go to FM2_TITLE_SERVER (default
127.0.0.1:3074, this server). Data in data/ next to this file. Run: py -3 server/live/xts_server.py

Client -> server frames (one type byte, then):
  04 (the first byte only)                protocol version
  04 + 153 bytes                          logon: XNADDR, title id, user count, XUID, gamertag, ...
  03 01                                   topology request
  01                                      keepalive
  00 sub + 24-byte header + bencode + params   web call; sub 4, or 8 for transacted calls (Create, Cancel, ...).
                                          Header: u24 id (LE), u16 params length (BE), u8, u8 bencode length,
                                          0x3C, 16-byte transaction id (sub 8). Bencode d3:url<class>5:mname<method>4:ctli0ee
Server -> client:
  04 + u32 status + u16 keepalive ms + u8 n + name     session established
  03 01 + u8 count + u32 + count * 37-byte records      topology (u8 id, char[33], u16 port, u8 flags)
  00 sub + u8 id + u32 length + body                    web reply. Body: u32 status (0 = success; a nonzero
                                                        one reads as "profile tampered with"), u32 code,
                                                        u32 message length, u64 cache duration, results

Params and results are records packed big-endian in field order (the layouts are the game's XTS_* type
descriptors). Strings and dates: u32 length counting the terminating zero, the text, the zero (dates in ISO
8601); byte arrays: u32 length + bytes; arrays: u32 count + elements.
"""
import datetime, socket, struct, threading, time

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', 3074))
srv.listen(4)
print('listening on 3074', flush=True)


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def bdecode(b, i=0):
    c = b[i:i + 1]
    if c == b'd':
        i += 1
        out = {}
        while b[i:i + 1] != b'e':
            k, i = bdecode(b, i)
            v, i = bdecode(b, i)
            out[k.decode()] = v
        return out, i + 1
    if c == b'i':
        e = b.index(b'e', i)
        return int(b[i + 1:e]), e + 1
    colon = b.index(b':', i)
    n = int(b[i:colon])
    return b[colon + 1:colon + 1 + n], colon + 1 + n


def s32(v): return struct.pack('>i', v)
def u32(v): return struct.pack('>I', v)
def s64(v): return struct.pack('>q', v)
def text(v): v = v.encode() + b'\0'; return u32(len(v)) + v  # the length counts the terminating zero; 0 is an error
def when(dt): return text(dt.strftime('%Y-%m-%dT%H:%M:%SZ'))
def player(xuid, tag): return struct.pack('>Q', xuid) + text(tag)


# Auctions players created (Auction.Create), kept in data/auctions.json (next to this file) with their car blobs.
import base64, json, os, re
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')
os.makedirs(DATA, exist_ok=True)
STORE = os.path.join(DATA, 'auctions.json')
lock = threading.Lock()


def load():
    if not os.path.exists(STORE):
        return []
    items = json.load(open(STORE))
    for a in items:
        a['closes'] = datetime.datetime.fromisoformat(a['closes'])
        for k in ('car', 'stats', 'livery', 'snapshot'):
            a[k] = base64.b64decode(a[k])
    return items


def save():
    out = [dict(a, closes=a['closes'].isoformat(), **{k: base64.b64encode(a[k]).decode() for k in ('car', 'stats', 'livery', 'snapshot')})
           for a in AUCTIONS]
    json.dump(out, open(STORE, 'w'), indent=1)


AUCTIONS = load()
# Transacted calls' replies by transaction id (hex), for Transaction.Reconcile
TX_STORE = os.path.join(DATA, 'transactions.json')
TRANSACTIONS = json.load(open(TX_STORE)) if os.path.exists(TX_STORE) else {}


class Reader:
    def __init__(self, b): self.b, self.i = b, 0
    def take(self, n): v = self.b[self.i:self.i + n]; self.i += n; return v
    def u8(self): return self.take(1)[0]
    def u32(self): return struct.unpack('>I', self.take(4))[0]
    def i64(self): return struct.unpack('>q', self.take(8))[0]
    def blob(self): return self.take(self.u32())


def auction(a, snapshot=True):
    now = datetime.datetime.now(datetime.timezone.utc)
    base = (s32(a['id']) + player(a['xuid'], a['tag']) + s64(a['opening']) + s64(a['buyout']) + when(a['closes']) +
            bytes([a['featured']]) + s32(a.get('status', 0)) + u32(0) +  # IsFeatured, Status (0 active, 3 cancelled), nBids
            player(0, '-') + s64(0) + when(now))              # TopBid: Bidder, Amount, Date
    car = a['car'] + u32(len(a['stats'])) + u32(len(a['livery']))   # the car summary as Create sent it, cbStats, cbLiveryDetails
    return base + car + (u32(len(a['snapshot'])) + a['snapshot'] if snapshot else u32(0))


def auction_car(a):
    # XTS_FORZA_AUCTION_CAR: Id, Reason, AuctionId, then the car summary with its blob sizes
    return s32(a['id']) + s32(a.get('status', 0)) + s32(a['id']) + a['car'] + u32(len(a['stats'])) + u32(len(a['livery']))


def auction_list(items):
    return u32(len(items)) + b''.join(auction(a, False) for a in items) + u32(len(items))


def by_id(i):
    return next((a for a in AUCTIONS if a['id'] == i), None)


def results(cls, method, params, who):
    r = Reader(params)
    if method == b'Create':
        opening, buyout, featured, duration = r.i64(), r.i64(), r.u8(), r.i64()
        car = r.take(34)  # Make, CarId, Region, Class, Year, Power, CurbWeight, DriveType, EnginePosition, Level, LiveryLocked, ContentFlags
        stats, livery, snapshot = r.blob(), r.blob(), r.blob()
        with lock:
            a = dict(id=max([a['id'] for a in AUCTIONS] + [1000]) + 1, xuid=who[0], tag=who[1], opening=opening,
                     buyout=buyout, featured=featured,
                     closes=datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(milliseconds=duration),
                     car=car, stats=stats, livery=livery, snapshot=snapshot)
            AUCTIONS.append(a)
            save()
        log('created auction %d: car %s, stats %d, livery %d, snapshot %d bytes, %d..%d CR' % (
            a['id'], car[:4].hex(), len(stats), len(livery), len(snapshot), opening, buyout))
        return s32(a['id'])
    if method == b'SearchForAuction':
        return auction_list([a for a in AUCTIONS if a.get('status', 0) == 0])
    if method == b'GetSubmitted':
        return auction_list([a for a in AUCTIONS if a['xuid'] == who[0] and not a.get('returned')])
    if method == b'GetDetailsAndBids':
        a = by_id(r.u32())
        if not a:
            return b''
        return (auction(a) + u32(len(a['stats'])) + a['stats'] + u32(len(a['livery'])) + a['livery'] +
                u32(0) + u32(0))  # rgBids (none yet), nAvailableBids
    if method == b'Cancel':
        a = by_id(r.u32())
        if a and a['xuid'] == who[0] and a.get('status', 0) == 0:
            with lock:
                a['status'] = 3
                save()
            log('auction %d cancelled' % a['id'])
        return b''
    if method == b'RetrievePendingCars':
        mine = [a for a in AUCTIONS if a['xuid'] == who[0] and a.get('status', 0) in (1, 3) and not a.get('returned')]
        return u32(len(mine)) + b''.join(auction_car(a) for a in mine) + u32(len(mine))
    if method == b'RetrieveCarForAuction':
        a = by_id(r.u32())
        if not a or a['xuid'] != who[0] or a.get('status', 0) == 0:
            return b''
        with lock:
            a['returned'] = True
            save()
        log('auction %d: car returned to %s' % (a['id'], who[1]))
        return auction_car(a) + u32(len(a['stats'])) + a['stats'] + u32(len(a['livery'])) + a['livery']
    if method == b'GetDetails':
        a = by_id(r.u32())
        return auction(a) if a else b''
    if method == b'GetLiverySnapshots':
        n = r.u32()
        found = [a for a in (by_id(struct.unpack('>i', r.take(4))[0]) for _ in range(n)) if a]
        return u32(len(found)) + b''.join(s32(a['id']) + u32(len(a['snapshot'])) + a['snapshot'] for a in found)
    if method == b'GetCarStatsAndLiveryDetails':
        a = by_id(r.u32())
        return u32(len(a['stats'])) + a['stats'] + u32(len(a['livery'])) + a['livery'] if a else u32(0) + u32(0)
    if method == b'GetServerTime':
        return when(datetime.datetime.now(datetime.timezone.utc))
    if method == b'HasPermission':
        return b'\x01'
    return b''


def client(c, a):
    log('connected', a)
    name = b'FM2 Live'
    c.sendall(bytes([4]) + struct.pack('>IHB', 0, 30000, len(name)) + name)
    buf = b''
    first = True
    who = (0, '?')

    def need(n):
        nonlocal buf
        while len(buf) < n:
            d = c.recv(65536)
            if not d:
                raise EOFError
            buf += d

    def take(n):
        nonlocal buf
        need(n)
        out, buf = buf[:n], buf[n:]
        return out

    try:
        while True:
            t = take(1)[0]
            if first and t == 4:
                first = False
                log('version 4')
                continue
            if t == 4:
                body = take(153)
                xuid = struct.unpack('>Q', body[45:53])[0]
                tag = re.search(rb'[\x20-\x7e]{2,15}', body[53:]).group().decode()
                who = (xuid, tag)
                log('logon %016X %s' % who)
            elif t == 3:
                sub = take(1)[0]
                log('topology request', sub)
                recs = b''.join(bytes([i]) + (b'fm2.%d' % i).ljust(33, b'\0') + struct.pack('>HB', 1065, 0)
                                for i in range(64))
                c.sendall(bytes([3, 1, 64]) + struct.pack('>I', 0) + recs)
            elif t == 1:
                pass  # keepalive
            elif t == 0:
                sub = take(1)[0]
                h = take(24)
                # header: u24 id (LE), u16 params length (BE), u8 ?, u8 bencode length, u8 0x3C, then (sub-type 8,
                # transacted calls) the 16-byte transaction id
                rid = h[0] | (h[1] << 8) | (h[2] << 16)
                plen = (h[3] << 8) | h[4]
                blen = h[6]
                bmsg, _ = bdecode(take(blen))
                params = take(plen)
                cls, method = bmsg.get('url', b''), bmsg.get('mname', b'')
                log('call %d (sub %d): %s . %s header %s params %s' % (rid, sub, cls.decode(), method.decode(), h.hex(), params[:96].hex()))
                if method == b'Reconcile':
                    # The reply the transaction got, kept by its id (a 20-byte reply header and results), as the
                    # WebRequestPayload; cbMaxAvailable its size
                    tx = params[:16].hex()
                    payload = bytes.fromhex(TRANSACTIONS.get(tx, ''))
                    log('reconcile %s: %s' % (tx, '%d bytes' % len(payload) if payload else 'unknown'))
                    res = u32(len(payload)) + payload + s32(len(payload))
                else:
                    res = results(cls, method, params, who)
                body = struct.pack('>IIIQ', 0, 0, 0, 36000000000) + res + bytes(256)  # status, code, message length, cache duration
                if sub == 8 and method != b'Reconcile':
                    with lock:
                        TRANSACTIONS[h[8:24].hex()] = (struct.pack('>IIIQ', 0, 0, 0, 0) + res).hex()
                        json.dump(TRANSACTIONS, open(TX_STORE, 'w'), indent=1)
                c.sendall(bytes([0, sub, rid & 0xFF]) + struct.pack('>I', len(body)) + body)
            else:
                log('unknown frame type', t, 'rest', buf[:64].hex())
                buf = b''
    except (EOFError, OSError) as e:
        log('closed', a, e)


while True:
    c, a = srv.accept()
    threading.Thread(target=client, args=(c, a), daemon=True).start()

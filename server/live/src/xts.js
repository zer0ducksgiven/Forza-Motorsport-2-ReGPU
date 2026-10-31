// FM2's Xbox Live title services (Microsoft's XtsClient: the auction house) over a WebSocket: the port tunnels the
// game's title server TCP connection here (src/kernel/net.cpp, FM2_TITLE_SERVER=wss://.../xts), each binary
// message a piece of the byte stream. server/live/xts_server.py is the same server for running locally over TCP;
// its top comment has the protocol.

const ACTIVE = 0, EXPIRED = 1, SOLD = 2, CANCELLED = 3;
const OUTBID = 1, SALE = 2; // payment reasons

export function xtsSocket(request, env) {
  const [client, server] = Object.values(new WebSocketPair());
  server.accept();
  const session = new Session(server, env.DB);
  let queue = Promise.resolve();
  // the game (or the port) went away: close our end too, or the runtime takes the request for hung
  // (1005 and 1006 are only reported, never sent)
  server.addEventListener("close", () => { try { server.close(1000, "bye"); } catch (e) {} });
  server.addEventListener("error", () => { try { server.close(1011, "error"); } catch (e) {} });
  server.addEventListener("message", (event) => {
    const data = typeof event.data === "string" ? new TextEncoder().encode(event.data) : new Uint8Array(event.data);
    queue = queue.then(() => session.receive(data)).catch((e) => {
      console.log("xts: " + (e && e.stack ? e.stack : e));
      server.close(1011, "error");
    });
  });
  return new Response(null, { status: 101, webSocket: client });
}

// --- Packing: big-endian records in field order -------------------------------------------------------------

class Writer {
  constructor() { this.parts = []; this.size = 0; }
  bytes(b) { this.parts.push(b); this.size += b.length; return this; }
  u8(v) { return this.bytes(Uint8Array.of(v & 0xff)); }
  u16(v) { const b = new Uint8Array(2); new DataView(b.buffer).setUint16(0, v); return this.bytes(b); }
  u32(v) { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, v >>> 0); return this.bytes(b); }
  i32(v) { const b = new Uint8Array(4); new DataView(b.buffer).setInt32(0, v); return this.bytes(b); }
  i64(v) { const b = new Uint8Array(8); new DataView(b.buffer).setBigInt64(0, BigInt(v)); return this.bytes(b); }
  xuid(hex) { const b = new Uint8Array(8); new DataView(b.buffer).setBigUint64(0, BigInt("0x" + hex)); return this.bytes(b); }
  // strings and dates: u32 length counting the terminating zero, the text, the zero (a length of 0 is an error)
  text(s) { const t = new TextEncoder().encode(s); return this.u32(t.length + 1).bytes(t).u8(0); }
  date(ms) { return this.text(new Date(ms).toISOString().replace(/\.\d{3}Z$/, "Z")); }
  blob(b) { return this.u32(b.length).bytes(b); }
  player(xuid, tag) { return this.xuid(xuid).text(tag); }
  done() {
    const out = new Uint8Array(this.size);
    let at = 0;
    for (const p of this.parts) { out.set(p, at); at += p.length; }
    return out;
  }
}

class Reader {
  constructor(b) { this.b = b; this.at = 0; this.view = new DataView(b.buffer, b.byteOffset, b.byteLength); }
  take(n) { const v = this.b.subarray(this.at, this.at + n); this.at += n; return v; }
  u8() { return this.b[this.at++]; }
  u32() { const v = this.view.getUint32(this.at); this.at += 4; return v; }
  i32() { const v = this.view.getInt32(this.at); this.at += 4; return v; }
  i64() { const v = this.view.getBigInt64(this.at); this.at += 8; return Number(v); }
  blob() { return this.take(this.u32()).slice(); }
}

function hex(b) { return [...b].map((x) => x.toString(16).padStart(2, "0")).join(""); }

function bdecode(b) {
  // the request's bencoded dictionary: d3:url<class>5:mname<method>4:ctlri0ee. Malformed input throws (never loops).
  let at = 0;
  const text = new TextDecoder("latin1");
  function value() {
    const c = b[at];
    if (c === 0x64) { // d
      at++;
      const out = {};
      while (b[at] !== 0x65) {
        if (at >= b.length) throw new Error("bencode: unterminated dictionary");
        const k = value();
        out[k] = value();
      }
      at++;
      return out;
    }
    if (c === 0x69) { // i
      const end = b.indexOf(0x65, at);
      if (end < 0) throw new Error("bencode: unterminated integer");
      const n = Number(text.decode(b.subarray(at + 1, end)));
      at = end + 1;
      return n;
    }
    const colon = b.indexOf(0x3a, at);
    const n = Number(text.decode(b.subarray(at, colon)));
    if (colon < 0 || !Number.isInteger(n) || n < 0 || colon + 1 + n > b.length) throw new Error("bencode: bad string");
    const s = text.decode(b.subarray(colon + 1, colon + 1 + n));
    at = colon + 1 + n;
    return s;
  }
  return value();
}

// --- A game session: the byte stream's frames ---------------------------------------------------------------

class Session {
  constructor(socket, db) {
    this.socket = socket;
    this.db = db;
    this.buffer = new Uint8Array(0);
    this.first = true;
    this.who = { xuid: "0000000000000000", tag: "?" };
    // type 4: session established (u32 status, u16 keepalive ms, u8 length, name)
    const name = new TextEncoder().encode("FM2 Live");
    this.send(new Writer().u8(4).u32(0).u16(30000).u8(name.length).bytes(name).done());
  }

  send(b) { this.socket.send(b); }

  async receive(data) {
    const all = new Uint8Array(this.buffer.length + data.length);
    all.set(this.buffer);
    all.set(data, this.buffer.length);
    this.buffer = all;
    for (;;) {
      const used = await this.frame(this.buffer);
      if (!used)
        break;
      this.buffer = this.buffer.slice(used);
    }
  }

  // One frame from the front of the buffer: its length, or 0 when it isn't all here yet
  async frame(b) {
    if (b.length < 1)
      return 0;
    const type = b[0];
    if (this.first && type === 4) { this.first = false; return 1; } // protocol version
    if (type === 4) { // logon: XNADDR, title id, user count, XUID, gamertag, ...
      if (b.length < 154)
        return 0;
      const body = b.subarray(1, 154);
      this.who.xuid = hex(body.subarray(45, 53)).toUpperCase();
      const tag = new TextDecoder("latin1").decode(body.subarray(53)).match(/[\x20-\x7e]{2,15}/);
      this.who.tag = tag ? tag[0] : "?";
      return 154;
    }
    if (type === 3) { // topology request: one record per service id, all here
      if (b.length < 2)
        return 0;
      const w = new Writer().u8(3).u8(1).u8(64).u32(0);
      for (let i = 0; i < 64; i++) {
        const name = new Uint8Array(33);
        name.set(new TextEncoder().encode("fm2." + i));
        w.u8(i).bytes(name).u16(1065).u8(0);
      }
      this.send(w.done());
      return 2;
    }
    if (type === 1)
      return 1; // keepalive
    if (type === 0) { // web call: sub, 24-byte header, bencode, params
      if (b.length < 26)
        return 0;
      const sub = b[1], h = b.subarray(2, 26);
      const id = h[0] | (h[1] << 8) | (h[2] << 16), paramsLength = (h[3] << 8) | h[4], bencodeLength = h[6];
      const total = 26 + bencodeLength + paramsLength;
      if (b.length < total)
        return 0;
      const call = bdecode(b.subarray(26, 26 + bencodeLength));
      const params = b.subarray(26 + bencodeLength, total).slice();
      await this.call(sub, id, h.subarray(8, 24), call.url || "", call.mname || "", params);
      return total;
    }
    console.log("xts: unknown frame type " + type);
    return b.length; // drop it
  }

  async call(sub, id, transaction, cls, method, params) {
    let results;
    if (method === "Reconcile") {
      // the reply the transaction got (kept by its id); one this server never saw reads as done
      const tx = hex(params.subarray(0, 16));
      const row = await this.db.prepare("SELECT payload FROM transactions WHERE id = ?1").bind(tx).first();
      const payload = row ? new Uint8Array(row.payload) : new Writer().u32(0).u32(0).u32(0).i64(0).i32(0).done();
      results = new Writer().blob(payload).i32(payload.length).done();
    } else {
      results = await calls(this.db, method, new Reader(params), this.who);
    }
    // reply body: u32 status, u32 code, u32 message length, u64 cache duration, results (zero padded)
    const body = new Writer().u32(0).u32(0).u32(0).i64(36000000000).bytes(results).bytes(new Uint8Array(256)).done();
    if (sub === 8 && method !== "Reconcile") {
      const payload = new Writer().u32(0).u32(0).u32(0).i64(0).bytes(results).done();
      await this.db.prepare("INSERT OR REPLACE INTO transactions (id, payload, created) VALUES (?1, ?2, ?3)")
        .bind(hex(transaction), payload, Date.now()).run();
    }
    this.send(new Writer().u8(0).u8(sub).u8(id & 0xff).u32(body.length).bytes(body).done());
  }
}

// --- The services --------------------------------------------------------------------------------------------

function auctionRecord(w, a, bids, withSnapshot) {
  const top = bids.length ? bids[bids.length - 1] : null;
  w.i32(a.id).player(a.xuid, a.tag).i64(a.opening).i64(a.buyout).date(a.closes)
    .u8(a.featured).i32(a.status).u32(bids.length);
  if (top) w.player(top.xuid, top.tag).i64(top.amount).date(top.date);
  else w.player("0000000000000000", "-").i64(0).date(Date.now());
  // the car summary as Create sent it, then its blob sizes
  const stats = new Uint8Array(a.stats), livery = new Uint8Array(a.livery);
  w.bytes(new Uint8Array(a.car)).u32(stats.length).u32(livery.length);
  if (withSnapshot) w.blob(new Uint8Array(a.snapshot));
  else w.u32(0);
}

function auctionCar(w, a) {
  // XTS_FORZA_AUCTION_CAR: Id, Reason, AuctionId, the car summary with its blob sizes
  w.i32(a.id).i32(a.status).i32(a.id).bytes(new Uint8Array(a.car))
    .u32(new Uint8Array(a.stats).length).u32(new Uint8Array(a.livery).length);
}

function bidRecord(w, b) { w.player(b.xuid, b.tag).i64(b.amount).date(b.date); }

async function bidsOf(db, id) {
  return (await db.prepare("SELECT * FROM bids WHERE auction = ?1 ORDER BY amount").bind(id).all()).results;
}

async function auctionList(db, rows) {
  const w = new Writer().u32(rows.length);
  for (const a of rows) auctionRecord(w, a, await bidsOf(db, a.id), false);
  return w.u32(rows.length).done();
}

async function byId(db, id) {
  return await db.prepare("SELECT * FROM auctions WHERE id = ?1").bind(id).first();
}

function winnerOf(a, bids) {
  return a.status === SOLD && bids.length ? bids[bids.length - 1].xuid : null;
}

// Auctions past their closing time: sold to the top bidder (the seller is paid) or expired (the car goes back)
async function settle(db) {
  const ended = (await db.prepare("SELECT id, xuid FROM auctions WHERE status = 0 AND closes <= ?1").bind(Date.now()).all()).results;
  for (const a of ended) {
    const bids = await bidsOf(db, a.id);
    const top = bids[bids.length - 1];
    const batch = [db.prepare("UPDATE auctions SET status = ?2 WHERE id = ?1").bind(a.id, top ? SOLD : EXPIRED)];
    if (top)
      batch.push(db.prepare("INSERT INTO payments (auction, xuid, reason, amount) VALUES (?1, ?2, ?3, ?4)").bind(a.id, a.xuid, SALE, top.amount));
    await db.batch(batch);
  }
}

async function calls(db, method, r, who) {
  await settle(db);
  switch (method) {
    case "Create": {
      const opening = r.i64(), buyout = r.i64(), featured = r.u8(), duration = r.i64();
      const car = r.take(34).slice(); // Make, CarId, Region, Class, Year, Power, CurbWeight, DriveType, ... ContentFlags
      const stats = r.blob(), livery = r.blob(), snapshot = r.blob();
      const next = await db.prepare("SELECT COALESCE(MAX(id), 1000) + 1 AS id FROM auctions").first();
      await db.prepare(`INSERT INTO auctions (id, xuid, tag, opening, buyout, featured, closes, car, stats, livery, snapshot)
                        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)`)
        .bind(next.id, who.xuid, who.tag, opening, buyout, featured, Date.now() + duration, car, stats, livery, snapshot).run();
      return new Writer().i32(next.id).done();
    }
    case "SearchForAuction":
      return auctionList(db, (await db.prepare("SELECT * FROM auctions WHERE status = 0 ORDER BY closes LIMIT 30").all()).results);
    case "GetSubmitted":
      // the seller's auctions until they're done with: active, their car to take back, or money to collect
      return auctionList(db, (await db.prepare(`SELECT * FROM auctions a WHERE xuid = ?1 AND (status = 0
          OR (status IN (1, 3) AND returned = 0)
          OR EXISTS (SELECT 1 FROM payments p WHERE p.auction = a.id AND p.xuid = ?1 AND collected = 0))
          ORDER BY id LIMIT 30`).bind(who.xuid).all()).results);
    case "GetBidOn": {
      // auctions the player bid on, until they've collected a car they won
      const rows = (await db.prepare(`SELECT * FROM auctions a WHERE EXISTS (SELECT 1 FROM bids b WHERE b.auction = a.id
          AND b.xuid = ?1) ORDER BY id LIMIT 30`).bind(who.xuid).all()).results;
      const shown = [];
      for (const a of rows)
        if (!(a.won_collected && winnerOf(a, await bidsOf(db, a.id)) === who.xuid)) shown.push(a);
      return auctionList(db, shown);
    }
    case "GetDetails": {
      const a = await byId(db, r.u32());
      if (!a) return new Uint8Array(0);
      const w = new Writer();
      auctionRecord(w, a, await bidsOf(db, a.id), true);
      return w.done();
    }
    case "GetDetailsAndBids": {
      const a = await byId(db, r.u32());
      if (!a) return new Uint8Array(0);
      const bids = await bidsOf(db, a.id);
      const w = new Writer();
      auctionRecord(w, a, bids, true);
      w.blob(new Uint8Array(a.stats)).blob(new Uint8Array(a.livery)).u32(bids.length);
      for (const b of [...bids].reverse()) bidRecord(w, b);
      return w.u32(bids.length).done();
    }
    case "GetBids": {
      const bids = await bidsOf(db, r.u32());
      const w = new Writer().u32(bids.length);
      for (const b of [...bids].reverse()) bidRecord(w, b);
      return w.u32(bids.length).done();
    }
    case "GetLiverySnapshots": {
      const n = r.u32(), w = new Writer(), found = [];
      for (let i = 0; i < n; i++) {
        const a = await db.prepare("SELECT id, snapshot FROM auctions WHERE id = ?1").bind(r.i32()).first();
        if (a) found.push(a);
      }
      w.u32(found.length);
      for (const a of found) w.i32(a.id).blob(new Uint8Array(a.snapshot));
      return w.done();
    }
    case "GetCarStatsAndLiveryDetails": {
      const a = await byId(db, r.u32());
      return a ? new Writer().blob(new Uint8Array(a.stats)).blob(new Uint8Array(a.livery)).done() : new Writer().u32(0).u32(0).done();
    }
    case "PlaceBid": {
      const a = await byId(db, r.u32()), amount = r.i64();
      if (!a || a.status !== ACTIVE || a.xuid === who.xuid) return new Uint8Array(0);
      const bids = await bidsOf(db, a.id), top = bids[bids.length - 1];
      if (amount < a.opening || (top && amount <= top.amount)) return new Uint8Array(0);
      const batch = [db.prepare("INSERT INTO bids (auction, xuid, tag, amount, date) VALUES (?1, ?2, ?3, ?4, ?5)")
        .bind(a.id, who.xuid, who.tag, amount, Date.now())];
      if (top) // the outbid bidder's credits back
        batch.push(db.prepare("INSERT INTO payments (auction, xuid, reason, amount) VALUES (?1, ?2, ?3, ?4)").bind(a.id, top.xuid, OUTBID, top.amount));
      if (a.buyout && amount >= a.buyout) {
        batch.push(db.prepare("UPDATE auctions SET status = 2 WHERE id = ?1").bind(a.id));
        batch.push(db.prepare("INSERT INTO payments (auction, xuid, reason, amount) VALUES (?1, ?2, ?3, ?4)").bind(a.id, a.xuid, SALE, amount));
      }
      await db.batch(batch);
      return new Uint8Array(0);
    }
    case "Cancel": {
      const id = r.u32();
      await db.prepare(`UPDATE auctions SET status = 3 WHERE id = ?1 AND xuid = ?2 AND status = 0
                        AND NOT EXISTS (SELECT 1 FROM bids WHERE auction = ?1)`).bind(id, who.xuid).run();
      return new Uint8Array(0);
    }
    case "RetrievePendingCars": {
      const rows = (await db.prepare("SELECT * FROM auctions WHERE (xuid = ?1 AND status IN (1, 3) AND returned = 0) OR (status = 2 AND won_collected = 0)")
        .bind(who.xuid).all()).results;
      const mine = [];
      for (const a of rows)
        if (a.xuid === who.xuid && a.status !== SOLD || winnerOf(a, await bidsOf(db, a.id)) === who.xuid) mine.push(a);
      const w = new Writer().u32(mine.length);
      for (const a of mine) auctionCar(w, a);
      return w.u32(mine.length).done();
    }
    case "RetrieveCarForAuction": {
      const a = await byId(db, r.u32());
      if (!a) return new Uint8Array(0);
      const toSeller = a.xuid === who.xuid && (a.status === EXPIRED || a.status === CANCELLED) && !a.returned;
      const toWinner = !a.won_collected && winnerOf(a, await bidsOf(db, a.id)) === who.xuid;
      if (!toSeller && !toWinner) return new Uint8Array(0);
      await db.prepare(`UPDATE auctions SET ${toSeller ? "returned" : "won_collected"} = 1 WHERE id = ?1`).bind(a.id).run();
      const w = new Writer();
      auctionCar(w, a);
      return w.blob(new Uint8Array(a.stats)).blob(new Uint8Array(a.livery)).done();
    }
    case "RetrievePendingPayments":
    case "RetrievePaymentsForAuction": {
      const only = method === "RetrievePaymentsForAuction" ? r.u32() : null;
      const rows = (await db.prepare(`SELECT * FROM payments WHERE xuid = ?1 AND collected = 0 ${only === null ? "" : "AND auction = ?2"}`)
        .bind(...(only === null ? [who.xuid] : [who.xuid, only])).all()).results;
      if (only !== null && rows.length)
        await db.prepare("UPDATE payments SET collected = 1 WHERE xuid = ?1 AND auction = ?2").bind(who.xuid, only).run();
      const w = new Writer().u32(rows.length);
      for (const p of rows) w.i32(p.id).i32(p.reason).i64(p.amount).i32(p.auction);
      return w.u32(rows.length).done();
    }
    case "GetServerTime":
      return new Writer().date(Date.now()).done();
    case "HasPermission":
      return Uint8Array.of(1);
  }
  return new Uint8Array(0); // anything else: empty results
}

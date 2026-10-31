import { claimName } from "./names.js";
// FM2 Live sessions on Cloudflare: tools/online/fm2_live_server.py as a Durable Object. Players connect a WebSocket
// to GET /live (the port's kernel/net.cpp with FM2_SERVER=wss://.../live); each message is one packet of that
// server's protocol ('RLV1', type, body). The race traffic doesn't come here: players send it to each other over
// UDP, at the public addresses they found with STUN and sent in their HELLO. Relay is kept as a fallback for a
// pair that can't reach each other, within a small per-player budget.
//
// One object holds every connection (the hibernation API: no cost while idle). Each socket's attachment is its
// player (id, XUID, gamertag, room, addresses, presence); the larger presence extras and matchmaking records are in
// the object's storage.

const MAGIC = [0x52, 0x4c, 0x56, 0x31]; // 'RLV1'
const VERSION = 1;
const [HELLO, WELCOME, ERROR, BROADCAST, RELAY, FORWARD, PUNCH, PUNCH_ACK, DATA, BYE, LIST, PEERS, PRESENCE, INVITE,
  MATCH_PUBLISH, MATCH_SEARCH, MATCH_RESULTS] = Array.from({ length: 17 }, (_, i) => i + 1);
const SESSION_INFO_SIZE = 0x3c;
const MAX_MATCH_RECORD = 1300, MAX_PRESENCE_EXTRA = 1024, MAX_PAYLOAD = 1500;
const MAX_NAME = 15, MAX_ROOM = 64;
const MOTD = "Welcome to FM2 Live";
const RELAY_BYTES_PER_SECOND = 32 * 1024; // the fallback relay, per player

export class LiveRoom {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.db = env.DB;
    this.budgets = new Map(); // player id -> { tokens, time }: the relay budget (resets when the object sleeps)
  }

  async fetch(request) {
    if (request.headers.get("Upgrade") !== "websocket")
      return new Response("FM2 Live\n");
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({ ip: ipToU32(request.headers.get("CF-Connecting-IP") || "127.0.0.1") });
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, message) {
    if (typeof message === "string")
      return;
    const p = new Uint8Array(message);
    if (p.length < 5 || p.length > 2048 || MAGIC.some((b, i) => p[i] !== b))
      return;
    try {
      await this.packet(ws, p[4], new DataView(p.buffer, p.byteOffset + 5, p.length - 5), p.subarray(5));
    } catch (e) {
      // a malformed packet must never end the room
    }
  }

  async webSocketClose(ws) { await this.left(ws); }
  async webSocketError(ws) { await this.left(ws); }

  async left(ws) {
    const me = ws.deserializeAttachment();
    if (me && me.id)
      await this.ctx.storage.delete([`extra:${me.id}`, `match:${me.id}`]);
    try { ws.close(1000, "bye"); } catch (e) {}
  }

  send(ws, type, body) {
    const out = new Uint8Array(5 + body.length);
    out.set(MAGIC);
    out[4] = type;
    out.set(body, 5);
    try { ws.send(out); } catch (e) {}
  }

  players() {
    const out = [];
    for (const ws of this.ctx.getWebSockets()) {
      const a = ws.deserializeAttachment();
      if (a && a.id)
        out.push({ ws, a });
    }
    return out;
  }

  roomOf(me) { return this.players().filter((p) => p.a.room === me.room && p.a.id !== me.id); }

  async epoch() {
    let e = await this.ctx.storage.get("epoch");
    if (!e) {
      e = 1 + Math.floor(Math.random() * 0xfffe);
      await this.ctx.storage.put("epoch", e);
    }
    return e;
  }

  spend(id, bytes) {
    const now = Date.now() / 1000;
    const b = this.budgets.get(id) || { tokens: 4 * RELAY_BYTES_PER_SECOND, time: now };
    b.tokens = Math.min(4 * RELAY_BYTES_PER_SECOND, b.tokens + (now - b.time) * RELAY_BYTES_PER_SECOND);
    b.time = now;
    this.budgets.set(id, b);
    if (b.tokens < bytes)
      return false;
    b.tokens -= bytes;
    return true;
  }

  forwardHeader(a) {
    return new W().u32(a.id).u32(a.pubIp).u16(a.pubPort).u32(a.localIp).u16(a.localPort).done();
  }

  async packet(ws, type, v, b) {
    const me = ws.deserializeAttachment() || {};
    if (type === HELLO)
      return this.hello(ws, me, v, b);
    if (!me.id) {
      this.send(ws, ERROR, errorBody(1, "not registered"));
      return;
    }
    switch (type) {
      case BROADCAST: {
        if (b.length < 4 || b.length - 4 > MAX_PAYLOAD)
          return;
        const room = this.roomOf(me);
        if (!this.spend(me.id, b.length * Math.max(1, room.length)))
          return;
        const packet = concat(this.forwardHeader(me), b);
        for (const p of room)
          this.send(p.ws, FORWARD, packet);
        return;
      }
      case RELAY: {
        if (b.length < 8 || b.length - 8 > MAX_PAYLOAD || !this.spend(me.id, b.length))
          return;
        const to = v.getUint32(0);
        const dst = this.roomOf(me).find((p) => p.a.id === to);
        if (dst)
          this.send(dst.ws, FORWARD, concat(this.forwardHeader(me), b.subarray(4)));
        return;
      }
      case LIST:
        return this.list(ws, me);
      case PRESENCE: {
        // state u32, XSESSION_INFO, status (u8 length + UTF-8), then the clients' own bytes (slots, QoS)
        const status = b[4 + SESSION_INFO_SIZE];
        const end = 5 + SESSION_INFO_SIZE + status;
        if (b.length < end)
          return;
        me.state = v.getUint32(0);
        me.session = hex(b.subarray(4, 4 + SESSION_INFO_SIZE));
        me.status = new TextDecoder().decode(b.subarray(5 + SESSION_INFO_SIZE, end));
        ws.serializeAttachment(me);
        await this.ctx.storage.put(`extra:${me.id}`, b.slice(end, end + MAX_PRESENCE_EXTRA));
        return;
      }
      case INVITE: {
        // target XUID u64 + the inviter's XSESSION_INFO; delivered as inviter id, XUID, gamertag, XSESSION_INFO
        if (b.length < 8 + SESSION_INFO_SIZE)
          return;
        const target = hex(b.subarray(0, 8));
        const name = new TextEncoder().encode(me.name);
        const body = concat(new W().u32(me.id).hex64(me.xuid).u8(name.length).done(), name, b.subarray(8, 8 + SESSION_INFO_SIZE));
        for (const p of this.roomOf(me))
          if (p.a.xuid === target)
            this.send(p.ws, INVITE, body);
        return;
      }
      case BYE:
        return this.left(ws);
      case MATCH_PUBLISH:
      case MATCH_SEARCH:
        return this.match(ws, me, type, v, b);
    }
  }

  async hello(ws, me, v, b) {
    const version = v.getUint16(0);
    const token = hex(b.subarray(2, 10)), xuid = hex(b.subarray(10, 18));
    const localIp = v.getUint32(18), localPort = v.getUint16(22);
    let at = 24;
    const room = new TextDecoder().decode(b.subarray(at + 1, at + 1 + b[at])).slice(0, MAX_ROOM);
    at += 1 + b[at];
    const name = new TextDecoder().decode(b.subarray(at + 1, at + 1 + b[at])).slice(0, MAX_NAME);
    at += 1 + b[at];
    // the public UDP address the client found with STUN (a server behind a WebSocket can't see it)
    let pubIp = 0, pubPort = 0;
    if (b.length >= at + 6) {
      pubIp = v.getUint32(at);
      pubPort = v.getUint16(at + 4);
    }
    if (version !== VERSION) {
      this.send(ws, ERROR, errorBody(2, `server speaks protocol ${VERSION}, client ${version}`));
      return;
    }
    // the name is one player's (src/names.js)
    if (this.db && name && !(await claimName(this.db, xuid, name).catch(() => true))) {
      this.send(ws, ERROR, errorBody(3, `The name ${name} is already taken on FM2 Live`));
      return;
    }
    if (!me.id || me.token !== token) {
      const used = new Set(this.players().map((p) => p.a.id));
      let id = (await this.ctx.storage.get("next")) || 1 + Math.floor(Math.random() * 0xfff);
      while (used.has(id) || id === 0)
        id = (id + 1) & 0xffffff;
      await this.ctx.storage.put("next", (id + 1) & 0xffffff);
      me.id = id;
    }
    Object.assign(me, {
      token, xuid, name, room, localIp, localPort,
      pubIp: pubIp || me.ip || 0, pubPort: pubPort || localPort,
      state: me.state || 0, session: me.session || "00".repeat(SESSION_INFO_SIZE), status: me.status || "",
    });
    ws.serializeAttachment(me);
    const motd = new TextEncoder().encode(MOTD);
    this.send(ws, WELCOME, concat(new W().u32(me.id).u16(await this.epoch()).u32(me.pubIp).u16(me.pubPort).u16(motd.length).done(),
      motd, new W().u16(0).done()));
  }

  async list(ws, me) {
    // the room's players, the asker first: id, XUID, gamertag, presence state, XSESSION_INFO, status, extra
    const all = [{ ws, a: me }, ...this.roomOf(me)].slice(0, 100);
    const entries = [];
    for (const { a } of all) {
      const name = new TextEncoder().encode(a.name), status = new TextEncoder().encode(a.status || "").slice(0, 255);
      const extra = (await this.ctx.storage.get(`extra:${a.id}`)) || new Uint8Array(0);
      entries.push(concat(new W().u32(a.id).hex64(a.xuid).u8(name.length).done(), name, new W().u32(a.state || 0).done(),
        unhex(a.session || "00".repeat(SESSION_INFO_SIZE)), Uint8Array.of(status.length), status,
        new W().u16(extra.length).done(), extra));
    }
    let start = 0;
    for (;;) {
      const part = [];
      let size = 6;
      while (start + part.length < entries.length && size + entries[start + part.length].length <= 1200) {
        size += entries[start + part.length].length;
        part.push(entries[start + part.length]);
      }
      this.send(ws, PEERS, concat(new W().u16(entries.length).u16(start).u16(part.length).done(), ...part));
      start += part.length;
      if (start >= entries.length || !part.length)
        break;
    }
  }

  async match(ws, me, type, v, b) {
    if (type === MATCH_PUBLISH) {
      // op u8: 0 withdraw, 1 publish (then search key u32 + the record)
      if (b[0] === 0) {
        await this.ctx.storage.delete(`match:${me.id}`);
        return;
      }
      const key = v.getUint32(1), record = b.slice(5, 5 + MAX_MATCH_RECORD);
      const old = await this.ctx.storage.get(`match:${me.id}`);
      const since = old && hex(old.record.subarray(0, 8)) === hex(record.subarray(0, 8)) ? old.since : Date.now();
      await this.ctx.storage.put(`match:${me.id}`, { key, record, since });
      return;
    }
    // search key u32, request number u32. Reply: request number, count u16, then per session (oldest first) the
    // host's id, XUID and record (u16 length + bytes). A searcher with a session of its own only sees older ones.
    const key = v.getUint32(0), request = v.getUint32(4);
    const own = await this.ctx.storage.get(`match:${me.id}`);
    const found = [];
    for (const p of this.roomOf(me)) {
      const m = await this.ctx.storage.get(`match:${p.a.id}`);
      if (m && m.key === key && !(own && own.key === key && m.since >= own.since))
        found.push({ a: p.a, m });
    }
    found.sort((x, y) => x.m.since - y.m.since);
    const entries = [];
    let size = 6;
    for (const { a, m } of found) {
      const entry = concat(new W().u32(a.id).hex64(a.xuid).u16(m.record.length).done(), m.record);
      if (size + entry.length > 1400)
        break;
      entries.push(entry);
      size += entry.length;
    }
    this.send(ws, MATCH_RESULTS, concat(new W().u32(request).u16(entries.length).done(), ...entries));
  }
}

class W {
  constructor() { this.bytes = []; }
  u8(v) { this.bytes.push(v & 0xff); return this; }
  u16(v) { return this.u8(v >> 8).u8(v); }
  u32(v) { return this.u16((v >>> 16) & 0xffff).u16(v & 0xffff); }
  hex64(h) { for (let i = 0; i < 16; i += 2) this.u8(parseInt(h.slice(i, i + 2), 16)); return this; }
  done() { return Uint8Array.from(this.bytes); }
}

function concat(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) { out.set(p, at); at += p.length; }
  return out;
}

function hex(b) { return [...b].map((x) => x.toString(16).padStart(2, "0")).join("").toUpperCase(); }
function unhex(h) { return Uint8Array.from(h.match(/../g).map((x) => parseInt(x, 16))); }

function ipToU32(ip) {
  const parts = ip.split(".").map(Number);
  return parts.length === 4 && parts.every((n) => n >= 0 && n < 256) ? ((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]) >>> 0 : 0;
}

function errorBody(code, message) {
  const text = new TextEncoder().encode(message);
  return concat(new W().u8(code).u16(text.length).done(), text);
}

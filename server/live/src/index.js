// FM2 Live: the port's stand-in for Xbox Live: leaderboards (src/kernel/leaderboards.cpp is the client), and
// GET /xts as a WebSocket: the title services (the auction house; src/xts.js, the port's kernel/net.cpp the client),
// GET /live as a WebSocket: sessions, presence and matchmaking (src/live_room.js; replaces tools/online/fm2_live_server.py).
// Plain text in and out, a line each, so the C++ side needs no JSON:
//
// POST /stats/write   "<xuid hex> <gamertag>" then one line per view: "<view> rating=<n> car=<n> ..."
//                     (rating keeps the highest, time the lowest, the rest the latest; like the game's XLAST)
// POST /stats/read    "rank <first rank> <count>" | "around <xuid hex> <count>" | "xuids - 0", then the view
//                     ids on one line, then (xuids) the XUIDs on one line. Answers per view "view <id> <total>",
//                     then "row <xuid> <rank> <rating> <car> <assists> <datetime> <time> <mode> <pi> <gamertag>"
//                     ("-" for a column never written).

import { xtsSocket } from "./xts.js";
export { LiveRoom } from "./live_room.js";

const COLUMNS = ["car", "assists", "datetime", "time", "mode", "pi"];
const INTEGER = /^-?\d{1,20}$/;
const XUID = /^[0-9A-F]{16}$/;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/xts" && request.headers.get("Upgrade") === "websocket")
      return xtsSocket(request, env);
    if (url.pathname === "/live") // sessions: one Durable Object for everyone
      return env.LIVE.get(env.LIVE.idFromName("live")).fetch(request);
    if (request.method !== "POST") return text("FM2 Live\n");
    const body = (await request.text()).slice(0, 64 * 1024);
    try {
      if (url.pathname === "/stats/write") return text(await write(env.DB, body));
      if (url.pathname === "/stats/read") return text(await read(env.DB, body));
    } catch (e) {
      return text(`error ${e.message}\n`, 400);
    }
    return text("not found\n", 404);
  },
};

function text(s, status = 200) {
  return new Response(s, { status, headers: { "content-type": "text/plain" } });
}

async function write(db, body) {
  const lines = body.split("\n").map((l) => l.trim()).filter(Boolean);
  const [xuid, ...nameParts] = (lines.shift() || "").split(" ");
  const name = nameParts.join(" ").slice(0, 15);
  if (!XUID.test(xuid) || !name) throw new Error("bad player");
  const now = Date.now();
  const statements = [];
  for (const line of lines.slice(0, 64)) {
    const [view, ...pairs] = line.split(" ");
    const id = Number(view);
    if (!(id >= 1 && id <= 1884)) continue;
    const values = {};
    for (const pair of pairs) {
      const [k, v] = pair.split("=");
      if ((k === "rating" || COLUMNS.includes(k)) && INTEGER.test(v)) values[k] = v;
    }
    if (values.rating === undefined) continue;
    statements.push(
      db
        .prepare(
          `INSERT INTO stats (view, xuid, name, rating, car, assists, datetime, time, mode, pi, updated)
           VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)
           ON CONFLICT (view, xuid) DO UPDATE SET
             name = excluded.name,
             rating = max(rating, excluded.rating),
             car = coalesce(excluded.car, car), assists = coalesce(excluded.assists, assists),
             datetime = coalesce(excluded.datetime, datetime),
             time = CASE WHEN excluded.time IS NULL THEN time WHEN time IS NULL THEN excluded.time ELSE min(time, excluded.time) END,
             mode = coalesce(excluded.mode, mode), pi = coalesce(excluded.pi, pi),
             updated = excluded.updated`
        )
        .bind(id, xuid, name, values.rating, ...COLUMNS.map((c) => values[c] ?? null), now)
    );
  }
  if (statements.length) await db.batch(statements);
  return `ok ${statements.length}\n`;
}

const ROW = `SELECT xuid, name, CAST(rating AS TEXT) AS rating, ${COLUMNS.map((c) => `CAST(${c} AS TEXT) AS ${c}`).join(", ")},
  (SELECT count(*) FROM stats o WHERE o.view = s.view AND (o.rating > s.rating OR (o.rating = s.rating AND o.updated < s.updated))) + 1 AS rank
  FROM stats s`;

async function read(db, body) {
  const lines = body.split("\n").map((l) => l.trim());
  const [mode, arg, countText] = (lines[0] || "").split(" ");
  const views = (lines[1] || "").split(" ").map(Number).filter((v) => v >= 1 && v <= 1884).slice(0, 16);
  const count = Math.min(Math.max(Number(countText) || 0, 0), 100);
  let out = "";
  for (const view of views) {
    const total = (await db.prepare("SELECT count(*) AS n FROM stats WHERE view = ?1").bind(view).first()).n;
    let rows = [];
    if (mode === "rank") {
      const first = Math.max(Number(arg) || 1, 1);
      rows = (await db.prepare(`${ROW} WHERE view = ?1 ORDER BY s.rating DESC, s.updated LIMIT ?2 OFFSET ?3`).bind(view, count, first - 1).all()).results;
    } else if (mode === "around" && XUID.test(arg)) {
      const me = await db.prepare(`${ROW} WHERE view = ?1 AND xuid = ?2`).bind(view, arg).first();
      // a page with the player in the middle (from the top when not on the board)
      const first = me ? Math.max(me.rank - Math.floor(count / 2), 1) : 1;
      rows = (await db.prepare(`${ROW} WHERE view = ?1 ORDER BY s.rating DESC, s.updated LIMIT ?2 OFFSET ?3`).bind(view, count, first - 1).all()).results;
    } else if (mode === "xuids") {
      const xuids = (lines[2] || "").split(" ").filter((x) => XUID.test(x)).slice(0, 100);
      for (const x of xuids) {
        const r = await db.prepare(`${ROW} WHERE view = ?1 AND xuid = ?2`).bind(view, x).first();
        if (r) rows.push(r);
      }
    }
    out += `view ${view} ${total}\n`;
    for (const r of rows)
      out += `row ${r.xuid} ${r.rank} ${r.rating} ${COLUMNS.map((c) => r[c] ?? "-").join(" ")} ${r.name}\n`;
  }
  return out;
}

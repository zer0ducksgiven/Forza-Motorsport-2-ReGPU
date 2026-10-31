// FM2 Live's activity log and status for the FM2 Reports Desk (tools/reports): sign-ins and sign-outs, auctions,
// bids, gifts and leaderboard results, a row each in D1 (kept 30 days), and GET /admin/status (who's online,
// live auctions, pending gifts, the latest activity). The admin routes need the ADMIN_TOKEN secret as a bearer
// token; the Desk keeps the same token in %LOCALAPPDATA%\fm2-reports\live-admin.token.

const KEEP_MS = 30 * 24 * 3600 * 1000;

// The title calls worth a line (src/xts.js): what the player did, and where the call's id is
export const TITLE_ACTIVITY = {
  Create: ["listed an auction", "result"],
  PlaceBid: ["bid on an auction", "params"],
  Cancel: ["cancelled an auction", "params"],
  RetrieveCarForAuction: ["collected a car from the auction house", "params"],
  RetrievePaymentsForAuction: ["collected auction credits", "params"],
  SendGift: ["sent a car gift", "result"],
  RetrieveGift: ["collected a car gift", "params"],
};

export async function logActivity(db, kind, xuid, tag, detail = "") {
  try {
    await db.prepare("INSERT INTO activity (at, kind, xuid, tag, detail) VALUES (?1, ?2, ?3, ?4, ?5)")
      .bind(Date.now(), kind, (xuid || "").toUpperCase(), tag || "", String(detail).slice(0, 200)).run();
  } catch (e) {}
}

export function authorised(request, env) {
  return !!env.ADMIN_TOKEN && request.headers.get("Authorization") === `Bearer ${env.ADMIN_TOKEN}`;
}

let pruned = 0;

export async function status(env) {
  const db = env.DB, now = Date.now(), day = now - 24 * 3600 * 1000;
  if (now - pruned > 3600 * 1000) { // a day's worth of old rows at a time, hourly at most
    pruned = now;
    await db.prepare("DELETE FROM activity WHERE at < ?1").bind(now - KEEP_MS).run().catch(() => {});
  }
  const live = await env.LIVE.get(env.LIVE.idFromName("live")).fetch(new Request("https://live/status")).then((r) => r.json());
  const one = (sql, ...a) => db.prepare(sql).bind(...a).first();
  const [auctions, gifts, names, signins, recent, kinds] = await Promise.all([
    one("SELECT count(*) AS n FROM auctions WHERE status = 0 AND closes > ?1", now),
    one("SELECT count(*) AS n FROM gifts WHERE retrieved = 0"),
    one("SELECT count(*) AS n FROM names"),
    one("SELECT count(DISTINCT xuid) AS n FROM activity WHERE kind = 'signed in' AND at >= ?1", day),
    db.prepare("SELECT at, kind, tag, detail FROM activity ORDER BY id DESC LIMIT 300").all(),
    db.prepare("SELECT kind, count(*) AS n FROM activity WHERE at >= ?1 GROUP BY kind").bind(day).all(),
  ]);
  return {
    at: now,
    online: live.players,
    sessions: live.sessions,
    auctionsLive: auctions.n,
    giftsPending: gifts.n,
    playersKnown: names.n,
    playersToday: signins.n,
    today: Object.fromEntries(kinds.results.map((k) => [k.kind, k.n])),
    activity: recent.results,
  };
}

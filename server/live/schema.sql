-- One row per leaderboard (the game's stats views, 1-1884) and player. All of FM2's views have the same
-- columns: Rating (max; the order), Car, Assists, DateTime, Time (min), Mode, Perf Index. Numbers are kept
-- as integers but passed as decimal text (XUIDs and FILETIMEs don't fit a JS number).
CREATE TABLE IF NOT EXISTS stats (
    view INTEGER NOT NULL,
    xuid TEXT NOT NULL,
    name TEXT NOT NULL,
    rating INTEGER NOT NULL,
    car INTEGER, assists INTEGER, datetime INTEGER, time INTEGER, mode INTEGER, pi INTEGER,
    updated INTEGER NOT NULL,
    PRIMARY KEY (view, xuid)
);
CREATE INDEX IF NOT EXISTS stats_order ON stats (view, rating DESC, updated);

-- The auction house (src/xts.js; the game reaches it through the port's title server WebSocket). XUIDs as 16 hex
-- digits; credits and times (ms) as integers; the car blobs exactly as the game's Auction.Create sent them.
-- status: 0 active, 1 expired (no bids), 2 sold, 3 cancelled.
CREATE TABLE IF NOT EXISTS auctions (
    id INTEGER PRIMARY KEY,
    xuid TEXT NOT NULL, tag TEXT NOT NULL,
    opening INTEGER NOT NULL, buyout INTEGER NOT NULL, featured INTEGER NOT NULL, closes INTEGER NOT NULL,
    car BLOB NOT NULL, stats BLOB NOT NULL, livery BLOB NOT NULL, snapshot BLOB NOT NULL,
    status INTEGER NOT NULL DEFAULT 0,
    returned INTEGER NOT NULL DEFAULT 0,      -- the seller has their car back (expired, cancelled)
    won_collected INTEGER NOT NULL DEFAULT 0  -- the winner has the car (sold)
);
CREATE INDEX IF NOT EXISTS auctions_status ON auctions (status, closes);
CREATE TABLE IF NOT EXISTS bids (
    auction INTEGER NOT NULL, xuid TEXT NOT NULL, tag TEXT NOT NULL, amount INTEGER NOT NULL, date INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS bids_auction ON bids (auction, amount);
-- Credits to collect: reason 1 outbid (the bid back), 2 sold.
CREATE TABLE IF NOT EXISTS payments (
    id INTEGER PRIMARY KEY, auction INTEGER NOT NULL, xuid TEXT NOT NULL, reason INTEGER NOT NULL,
    amount INTEGER NOT NULL, collected INTEGER NOT NULL DEFAULT 0
);
-- Transacted calls' replies (a 20-byte reply header and results) by the game's transaction id, for Reconcile.
CREATE TABLE IF NOT EXISTS transactions (id TEXT PRIMARY KEY, payload BLOB NOT NULL, created INTEGER NOT NULL);

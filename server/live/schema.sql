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

// Gamertags (profile names) on FM2 Live: one player each, first come first served, so adding a friend by name
// finds the right player. A name is the first XUID's to sign in with it; it's free again after its owner has
// been away for six months. Names match without case ("scott" is "Scott"). A player who renames lets go of the
// old name.

const KEEP_MS = 183 * 24 * 3600 * 1000;

// Sign-in with this name: true when it's the player's (now claimed and seen), false when someone else has it.
export async function claimName(db, xuid, tag) {
  xuid = xuid.toUpperCase();
  const key = tag.toLowerCase(), now = Date.now();
  const owner = await db.prepare("SELECT xuid, seen FROM names WHERE name = ?1").bind(key).first();
  if (owner && owner.xuid !== xuid && owner.seen > now - KEEP_MS)
    return false;
  await db.batch([
    db.prepare("DELETE FROM names WHERE xuid = ?1 AND name <> ?2").bind(xuid, key),
    db.prepare("INSERT INTO names (name, xuid, tag, seen) VALUES (?1, ?2, ?3, ?4) ON CONFLICT (name) DO UPDATE SET xuid = excluded.xuid, tag = excluded.tag, seen = excluded.seen")
      .bind(key, xuid, tag, now),
  ]);
  return true;
}

// "free", "yours" or "taken" (the launcher asks before it creates or renames a profile)
export async function checkName(db, tag, xuid) {
  const owner = await db.prepare("SELECT xuid, seen FROM names WHERE name = ?1").bind(tag.toLowerCase()).first();
  if (!owner || owner.seen <= Date.now() - KEEP_MS)
    return "free";
  return owner.xuid === (xuid || "").toUpperCase() ? "yours" : "taken";
}

// The name's owner: { xuid, tag, seen } or null
export async function findName(db, tag) {
  return await db.prepare("SELECT xuid, tag, seen FROM names WHERE name = ?1").bind(tag.toLowerCase()).first();
}

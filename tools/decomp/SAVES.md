# FM2 profile saves

Worked out from the decompiled game on 2026-11-01 (TU4 addresses; `names.txt` has the functions). Checked on a
real Xbox 360 save and on the port's own: both decrypt, and an edited one loads in the game.

A profile's content folder (`profiles\<name>\content\00000001\ForzaProfile\`, or the STFS package on a 360)
holds:

| file | what | layer |
|---|---|---|
| `ForzaProfile` | key-value settings and career stats (`Main/Credits`, `Main/Level`, options...) | secure stream |
| `PlayerDatabase` | the career, garage, results: a SQLite database | secure stream, then raw deflate |
| `Liveries\N.liv`, `Thumbnails\*.xdc` | paint jobs, photo thumbnails | not worked out yet |

## Secure stream (`IOSys::CSecurityStream`)

```
0..19   HMAC-SHA1(hmacKey, bytes 20..39 + body)
20..23  plain length XOR the four big-endian words of the IV
24..39  IV (random each save)
40..    AES-128-CBC(aesKey, IV) of the plain data, zero-padded to 16
```

The keys are 16 bytes each, made from the profile's XUID (8 bytes, big endian) and two constants in the
executable: `key[i] = constant[i] ^ xuid[(i + shift) & 7]`.

| file | AES constant, shift | HMAC constant, shift | built in |
|---|---|---|---|
| ForzaProfile | 820076A0, +1 | 820076B0, 0 | 82232A00 |
| PlayerDatabase | 820076C0, +3 | 820076D0, +2 | (CDatabase::vf13's caller) |
| liveries | 8200D1E0, -3 | 8200D1D0, -4 | 8226AF90 |

The console's `XeKeysObscureKey`/`XeKeysAesCbcUsingKey`/`XeKeysHmacShaUsingKey` only hide the keys in memory, so
nothing depends on the console: a save moves between consoles and this port as long as the XUID goes with it
(which is why the launcher's Xbox 360 import keeps it).

## PlayerDatabase

After decryption: `[u32 size][u32 packed size][u32 size]`, big endian, then the SQLite file as raw deflate
(`IOSys::CCompressionStream`). Tables: `Career_Garage`, `Career_EventResults`, `Career_RaceResults`,
`Career_UnlockedCars`, `Career_PurchasedParts`, `Career_AIHistory`, `Career_SeriesPoints`, `Career_SeriesProgress`,
`Arcade_Garage`, `Arcade_Exhibition`, `Arcade_TimeTrial`. The game's own data (`Data_Car`, `Events`, `Races`, ...,
169 tables) is in `Media\DB\gamedb.slt`, a plain SQLite file.

## ForzaProfile

After decryption, entries of `[u32 name length][name][u32 0x20][u32 0][u8 type][value]`, grouped under section
names (`Main`, `Arcade`, ...). Type 3 is a big-endian u32 (`Credits` is at the entry's +20).

## Tools

```
py -3 -I tools/decomp/savecrypt.py decrypt ForzaProfile <xuid>        # -> ForzaProfile.plain
py -3 -I tools/decomp/savecrypt.py encrypt ForzaProfile.plain <xuid> ForzaProfile
py -3 -I tools/decomp/savecrypt.py unpack PlayerDatabase <xuid>       # -> PlayerDatabase.sqlite
py -3 -I tools/decomp/savecrypt.py pack PlayerDatabase.sqlite <xuid> PlayerDatabase
```

Back the profile up first (the launcher's Back up). `FM2_TRACE_KEYS=1` logs every AES key the game uses, with
its caller.

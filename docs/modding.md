# Making mods for Forza Motorsport 2 (ReGPU)

Mods for the port are packaged as **DLC**: the same Xbox 360 package files (STFS) that Xbox Live sold car packs as.
The game loads them the way it loaded real DLC, so a mod can do anything a car pack could, such as adding cars or
patching the game database. You don't change the game's files.

The port comes with two mods, both installed switched off:

- The **Engine Swap Pack** by Aiden adds engine swap upgrades to 190 cars. Its source is in
  [`tools/modkit/examples/EngineSwapPack`](../tools/modkit/examples/EngineSwapPack).
- **Random Events** adds a career category of four generated events that change every two races, with prizes that
  follow the player's level and garage. It holds the game's own string tables, so it isn't shipped: the launcher
  builds it from the player's game files the first time it starts
  ([`tools/modkit/career/random_events.py`](../tools/modkit/career/random_events.py); the port's
  `src/hooks/random_events.cpp` does the race counts, prizes and prize car).

## Installing a mod (players)

1. In the launcher, open **Settings**. In the **Mods** panel, choose **Add mod** and pick the package file.
2. Switch the mod on. It's loaded the next time the game starts.

To switch off a pack you've installed, use the switch. **Remove** deletes it. A removed example mod stays removed.

> **Keep a mod on once a career uses it.** If you buy an engine swap or a mod's car and then switch the mod off, the
> save points at something the game no longer has. Use a separate profile to try mods out.

The installed files live in `Documents\fm2\dlc` (switched off: `dlc-disabled`). Each mod has a `<name>.mod` file
beside it, which separates it from real DLC. While **Forza Plus 2** is on, no DLC or mods are loaded, because that mod
has its own database.

## How a mod is laid out

A package holds a `Media/DLCZips` folder with one or more zips:

```
<package>                         (STFS, title 4D5307EA, content type 2 = marketplace content)
└── Media/DLCZips/
    ├── EngineSwaps_pri_67.zip    the mod's files
    ├── zipmount.xml              optional: mounts a zip somewhere else (see below)
    └── stringtables.xml          optional: string tables the mod adds to
```

Inside each zip the paths are the game's own, starting at `media/`:

```
media/db/patch/80000_execute.sql      SQL run against the game database
media/db/patch/91000_merge.slt        or: a SQLite database whose rows are merged in
media/cars/<car folder>/...           a car's model, textures and physics (car mods)
media/UI/Textures/Thumbnails/...      its thumbnail
```

- **Database patches** (`media/db/patch/`): `NNNNN_execute.sql` holds SQL statements (`INSERT INTO [Table] ...`).
  `NNNNN_merge.slt` is a SQLite file whose tables are merged into the game's. The number orders the patches. Pick
  one well away from other mods (the Engine Swap Pack uses 80000, the backported cars 91000 and up).
- **`_pri_<N>` in the zip's name** is its priority. When two zips have the same file, the higher number wins. A
  pack's "Overrides" zip, for example, is 161 against the car's 71.
- **`zipmount.xml`** mounts a zip at another path instead of `game:\Media\`. For example, a car pack mounts its
  overrides zip at `game:\Media\StringTables\` so that the car's name gets into the string tables:

  ```xml
  <?xml version="1.0" ?>
  <zipmount>
      <zip Name="CadillacEscaladeOverrides1_pri_161.zip" Mount="game:\Media\StringTables\" />
  </zipmount>
  ```

- **`Media/LicenseMasks/<bit>.sql`** (beside `DLCZips`, not in it): a new car only reaches the dealership once
  this runs. It sets `IsPurchased=1` on the car's `Data_Car` row and adds the car to `StockGarage`. Copy one from an
  official car pack and change the car IDs. Put it in a `LicenseMasks` folder next to your mod folder and
  `make_mod.py` adds it.

The engine swaps are an example of a database patch. Each row of the pack's `80000_execute.sql` adds one upgrade:

```sql
INSERT INTO [List_UpgradeEngine] ([Id],[Ordinal],[Level],[EngineID],[IsStock],[ManufacturerID],[Price],[MassDiff],[DragScale],[WindInstabilityScale])
VALUES ('554001','554','1','119','0','48','11900','100','1','1');
```

A swap screen shows the engine's `EngineName` from `Data_Engine`. Engines that were only fitted as stock have it
blank, and the screen then shows no name. The pack sets one for each engine like that:

```sql
UPDATE [Data_Engine] SET [EngineName]='5.7 V10' WHERE [EngineID]='46';
```

`Ordinal` is the car, `EngineID` the engine it gets, and `Level` the upgrade slot. `Id` is the ordinal times 1000
plus the level, and must be unique. To see the tables and the values in use, open the game's database
(`Media\DB\gamedb.slt` in the game files, a SQLite file) in a SQLite browser.

## Building a package

You need Python 3. The tools are in [`tools/modkit`](../tools/modkit):

| File | What it does |
|---|---|
| `make_mod.py` | Makes a package from a folder in one step |
| `stfs_write.py` | Packs a folder into a package, reusing another package's header (Aiden's) |
| `stfs_extract.py` | Unpacks a package, or lists it without an output folder (Aiden's) |
| `stfs_read.py` | Reads single files from a package without unpacking it (Aiden's) |
| `template.pkg` | The header new packages are built on (the Engine Swap Pack) |

1. Make a folder for the mod and lay it out like the inside of the zip, for example
   `MyMod/media/db/patch/80100_execute.sql`.
2. Build it:

   ```sh
   python tools/modkit/make_mod.py MyMod out --name "My Mod" --desc "What it does" --pri 67
   ```

   This zips the folder into `Media/DLCZips/MyMod_pri_67.zip` and packs it. The package is written to `out/` under
   its content ID, a 42-character name such as `A24B25A95D4DA3F3D248A87D409075C21FEA05FE4D`. Leave that name as it is.
   `--zip Name` sets the zip's name; it's the folder's name by default.
3. If you need `zipmount.xml` or `stringtables.xml`, put them in a `DLCZips-extra` folder next to `MyMod`, and
   they're added beside the zip.
4. Add the package in the launcher (**Settings › Mods › Add mod**) and start the game.

To build the example again:

```sh
python tools/modkit/make_mod.py tools/modkit/examples/EngineSwapPack out --name "Engine Swap Pack" --desc "Adds engine swaps to 190 cars." --zip EngineSwaps
```

To look inside any package:

```sh
python tools/modkit/stfs_extract.py <package>
python tools/modkit/stfs_extract.py <package> unpacked
```

The first lists its files and the second unpacks them.

The repacker can't sign packages. The port doesn't check signatures, but an Xbox 360 or Xenia may reject a modded
package.

## Testing and sharing

- If a patch doesn't seem to apply, check `out\play.log`. A `content: DLC <name>: Media found, 1 car zip(s)` line
  means the package was found. The patches are unpacked to `Documents\fm2\dlc\<name>\Media\DLCZips\<zip name>\`
  when the game starts.
- An error in your SQL can stop the game loading its database. Test on a separate profile.
- To share a mod, share the package file. Players add it with **Add mod**.

## Credits

The Engine Swap Pack and the packaging tools (`stfs_extract.py`, `stfs_read.py`, `stfs_write.py`) are by Aiden, and
are used and shipped with permission. `stfs_extract.py` is a Python port of
[Extract-STFS](https://github.com/jasonulbright/Extract-STFS).

# Decompiling FM2

XenonRecomp still runs the game. This folder is for understanding it, and for replacing pieces of it with real C++
one function at a time, each checked against the original.

## Pipeline

All addresses are TU4 addresses (the executable we run), not the disc addresses `src/hooks` uses.

1. **Index the code** (Python, seconds):
   ```
   py -3 -I tools/decomp/rtti.py        # C++ classes, base classes, vtables  -> out/decomp/rtti.json
   py -3 -I tools/decomp/xrefs.py       # every function's calls, strings, data -> out/decomp/xrefs.json
   py -3 -I tools/decomp/libmatch.py    # code shared with PGR4 = Xbox SDK      -> out/decomp/shared_pgr4.json
   py -3 -I tools/decomp/luabind.py     # handlers named by their registrations -> out/decomp/luabind.json
   py -3 -I tools/decomp/symbols.py     # names and kinds for every function    -> out/decomp/symbols.txt
   ```
   `symbols.py` names functions from RTTI vtables (`Class::vfN`), Lua bindings (`lua_X` handlers and the one
   function each calls, `X`), the start-up constants (`init_<name>`), libraries (FMOD, the Xbox SDK, XTS, Bink,
   Lua), and the hand-kept `names.txt`, which wins over everything.

2. **Ghidra** (12.2, needs JDK 25; `D:\ghidra`):
   ```
   py -3 -I tools/decomp/ghidra/install_xenon.py   # once per Ghidra install: the Xenon language (VMX128)
   tools\decomp\ghidra\import.cmd                  # project D:\ghidra\projects\FM2, named, analysed (~4 min)
   tools\decomp\ghidra\export.cmd                  # C for every function + report.tsv -> D:\ghidra\export (~10 min)
   tools\decomp\ghidra\decompile.cmd out.c 8247B008 ...   # just some functions
   ```
   Ghidra's own launcher settings (`support/launch.properties`): `JAVA_HOME_OVERRIDE` = the JDK 25, and
   `application.settingsdir/cachedir/tempdir` under `D:\ghidra` (its plugin loader fails on the linked AppData
   folder here).

3. **Replace and check** (`src/decomp/`): a decompiled function goes in as a strong `PPC_FUNC` over XenonRecomp's
   weak one, through `DecompCheck`: `FM2_DECOMP_CHECK=1` runs the original too, on the same registers and memory,
   and logs any difference; `FM2_DECOMP_OFF=1` runs only the originals. Names use `TU_SUB`/`TU_IMP` so the disc->TU
   remapper leaves them alone.

## The Xenon language

Ghidra knows PowerPC and AltiVec but not the 360's VMX128 (128 vector registers, the `...128` instructions) or the
Cell loads (`lvlx`, `lvrx`, ...). `vmx128.py sleigh` generates SLEIGH for them from XenonRecomp's own disassembler
table; `install_xenon.py` adds a `PowerPC:BE:64:Xenon` language built from Ghidra's 64-bit AltiVec spec plus that,
with the Power ISA 2.07/3.0 instructions that share encodings with VMX128 left out. Arithmetic is kept as named
operations (`vmulfp128(a, b)`); loads and stores have real memory semantics.

Check it after any change: `run_script.cmd DumpVectorFm2.java vec.tsv` then `vmx128.py check vec.tsv`
(2026-11-01: 13,988 of 13,988 VMX128 instructions decode as XenonRecomp does).

The compiler's register save/restore helpers (`savegprlr_14..31` and co., from `config/fm2_tu4.toml`) get a no-op
call fixup, and Ghidra's guess at functions that never return is off: both cut functions short otherwise.

## Where it stands (2026-11-01)

| | functions |
|---|---|
| total (XenonRecomp's list) | 50,729 |
| start-up constants and Lua registrations | 17,543 |
| C++ virtual functions (1,154 classes) | 3,645 |
| libraries (Xbox SDK, FMOD, SQLite, XTS, Bink, Lua, fonts) | 3,689 |
| Lua bindings and the functions they run | 868 + 551 |
| hand-named (`names.txt`) | 54 |
| still unnamed game code | 24,391 (63% of the code) |
| Ghidra decompiles cleanly (no warnings, or only harmless ones) | 51,067 of 51,099 (99.94%) |
| replaced in the game, checked against the original | 13 (`src/decomp`): the C runtime, the name hash, a D3D matrix-vector product |

The replaced C runtime (`memcpy`, `memset`, string functions, `floor`/`ceil`) matched the original on every one of
about 50 million calls checked, and is about 5% faster on the benchmark (184 and 185 fps against 176 and 176, RTX
4070, alternating runs).

What the game is made of, found along the way: its data is SQLite (`Media\DB\gamedb.slt`, 169 tables: cars,
engines, parts, events, races, AI), its careers are SQLite too (inside `PlayerDatabase`), and its profile saves
are AES and HMAC keyed by the XUID: [SAVES.md](SAVES.md), with `savecrypt.py` to read and edit them. The career
menu's "next event" is one SQL query (`GetNextCareerEvent`, 8231AE70).

Caveats: the linker folded identical small functions together, so an 8-byte getter named `ClassA::vf4` may also
serve unrelated classes. Shared vtable slots are named after the base class when there is one, else `shared::`.

Finding hot functions: `FM2_SAMPLE_THREAD=7` (the render thread; 1 is the main thread, idle in races) logs the
busiest functions every 5 s.

Decompiled code is the game's code: keep exported C (`D:\ghidra\export`) out of the public repository.

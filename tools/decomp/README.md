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

## Lifting functions into the port

The road to a build without the recompiled code: game functions turned from Ghidra's C into C++ that replaces
them, checked against the originals. Lifted code goes to `out/lift` (never committed: it's the game's code).

1. Measure what runs: configure a build folder with `-DFM2_COVERAGE=ON`, run the game, and
   `py -3 tools/decomp/coverage.py title=a.tsv race=b.tsv` sizes the set and orders it bottom-up.
2. `ghidra/run_script.cmd LiftInfoFm2.java <addresses.txt> <info.jsonl>`: Ghidra's C plus each parameter's
   register and each global's type.
3. `py -3 tools/decomp/lift.py <info.jsonl>` lifts what it can (out/lift/report.tsv counts why the rest isn't),
   only functions whose call trees a check can see (no kernel imports, calls through pointers, hooks or vector
   stores).
4. Build with `-DFM2_STORE_LOG=ON -DFM2_LIFTED=ON` and run with `FM2_DECOMP_CHECK=lift`: a lifted function's
   calls run it against an overlay, then the original for real, and compare (src/decomp/lift.h), 64 calls once
   every branch of it has run (lift.py marks them, LIFT_BLOCK), else up to 1,024. The game plays as the original.
   Results: out/lift_check.tsv.
5. `py -3 tools/decomp/lift.py <info.jsonl> --keep a.tsv,b.tsv` keeps the functions that never differed and had
   every branch run in a passing check, across the routes given.
   `FM2_LIFT_ENABLE=<file of addresses>` runs only some lifted, for bisecting a bad one.

tools/patch_mem.py routes the recompiled code's vector loads and stores, lwarx/stwcx., dcbz and setjmp/longjmp
through macros, so they use the overlay too (a lifted run may longjmp only to a setjmp of its own: Lua's error throw
blocked 1,155 functions otherwise). Hooks that only add to their original (a log line, a counter, a cheat applied
once) start with FM2_SHADOW_ORIGINAL(__imp__sub_X), which runs the original instead inside a check run, and checks
follow calls into them; and so do the critical sections (src/kernel/imports_rtl.cpp; one held by another thread
ends the run) and mtmsrd (the interrupt lock is skipped there). A check's lifted run is abandoned if it reaches any other
kernel import, a hook (directly or through a pointer), setjmp/longjmp, faults, or spins (2 million loads): it hasn't
touched guest memory yet. A fault or a spin rules the function out; an import or a hook only leaves that call
unchecked (its branches don't count as covered; after 16, FM2_LIFT_UNCHECKED, the function isn't checked again).
out/lift_check.tsv says which (escaped_by, unchecked).

A fault in a lifted run is logged with its exception code, the host address (exe+offset, for llvm-symbolizer) and
the guest address it touched; C++ exceptions other than the check's own escapes are logged by what they say. A
function is kept only if at least one call was judged and passed (not raced, not unchecked): three without branches
whose every call reached an import were kept unchecked, and crashed boot.

A difference only counts when nothing the lifted run read changed meanwhile: the check records every byte of real
memory the lifted run reads, and when the results differ, a byte that another thread has since changed (or that the
original found different before its first store there) makes the call "raced" and not judged. Without this, two
check rounds on the FX disagreed on 1,894 functions; with it, on 67. A lifted function called during another check's original run is checked there too (its stores go into the outer
check's log as well; FM2_LIFT_NESTED=0 turns it off): checks can last a whole route, and 2,192 functions only ever
called inside them were never checked. Besides memory and r3/f1 the check compares
the registers callers read after a call returns (livein.py's liveout.json: with link-time code generation a caller
can rely on a callee leaving r4 or f2 set, and one lifted function that didn't froze the menus).
FM2_LIFT_SHADOW_CS=0 and FM2_LIFT_SHADOW_MSR=0 end runs at those again. `--strict` leaves out functions that call imports or through pointers at all (2,107 kept against
3,733 with them followed).

Ghidra's C drops call arguments, so `py -3 tools/decomp/livein.py` works out each function's real inputs (the
argument registers it reads before writing, by liveness over the recompiled code). A direct call only kills the
registers the callee can change (its writes and its callees', worked out first): with link-time code generation a
caller keeps values in registers a callee leaves alone, and passes them on (827F5390 passes its caller's r5 through a
call to 827F5280; lifted 82802070 didn't set that r5, and the menus froze). `ghidra/apply_params.cmd` writes
them into the Ghidra project as each function's parameters (32-bit pieces of the registers, custom storage; the
project before is in D:\ghidra\projects\FM2-before-params), which fixes most of Ghidra's dropped arguments and a
constant it folded through a call, and makes the C read better. Registers Ghidra still sees no setup for (in_r6,
extraout_r4) are read where used; where Ghidra casts a 32-bit parameter to 64 bits the lifted code reads the whole
register (8237E0D8 compares a pointer as 64 bits, and Ghidra's sign extension of it froze boot), unless the original
extends that register itself (extsw/clrldi/rldicl: then the cast is that extension, as 8263E068's extsw before a
fcfid). The lifter also rejects a call that leaves out a register the original sets up and the callee
reads (82297C38 lost a memmove's source and
length, 824EBB68 a virtual call's this). From what else has bitten so far it rejects `void` functions whose r3
differs (callers use it); stack locals whose address goes to a callee are left to the check (LIFT_STACK_CALLS=1;
before Ghidra had the real parameters it folded one's value through the call), and it keeps a function only once every branch of it has run in a passing check.

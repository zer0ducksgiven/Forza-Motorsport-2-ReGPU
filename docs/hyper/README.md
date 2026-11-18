# reGPU_Hyper: transfer package

**Hyper** is a low-spec, high-performance mode for **ReGPU**, the Xbox 360 GPU replacement used by XenonRecomp ports. It is prototyped
inside the Forza Motorsport 2 port, whose renderer is ReGPU's origin. This package holds everything needed to keep going on a
second machine.

- **Packed:** 7 Nov 2026, from the dev laptop: Ryzen 3 5300U, Radeon Vega iGPU, 7.3 GB RAM.
- **Next machine:** Core i3-7100 (2 cores, 4 threads), Intel HD 630, 16 GB DDR4.

**Agent resuming this work:** read this file, then `HYPER.md`, then `HYPER-CHANGES.md`, then `_handoff/agent-memory/`. To resume,
give your agent the short prompt in `RESUME-PROMPT.md`.

## Update package (7 Nov 2026, evening)

If you got `reGPU_Hyper_update_2026-11-07.zip`, extract it **over** the first package (`reGPU_Hyper_transfer.zip`) and let it replace
files. It holds only what changed since the first package: source, ledger, README, experiment patches, a rebuilt `fm2.exe`, benchmark
logs and the picture comparisons, and a refreshed `_handoff/` (patch, agent memory, transcript).

What happened since the first package (details in `HYPER-CHANGES.md`):

- **Native potato:** at 720p native with no resolution drop, the strongest settings of every kept native cut (`REGPU_HYPER_ANISO=0
  REGPU_HYPER_HELPERS=25 REGPU_HYPER_RESOLVE1010=1 REGPU_HYPER_DECIMATE=10`) run **+34% race fps** (42 → 56), GPU 23.5 → 17.6 ms.
- **The laptop's bottleneck is pixel shading.** About 10 shades per screen pixel. FM2 draws its **cars with alpha blending on**:
  they cost ~3.3 ms of shading, while real transparent effects cost ~0 ms in the parked scene.
- **Rejected or parked** (each logged with a patch):
  - Decimation: welding (made it worse) and sloppy simplification. Cars are hundreds of tiny pieces that per-draw decimation
    can't simplify, and sloppy simplification removed the sky, reflections and HUD parts.
  - A half-size transparent layer: not built, nothing to gain here.
  - Heavy-shader mip bias: −0.4 ms. Retest on Intel, where bandwidth is scarcer.
  - Lossless fast texture fetch: no gain.
  - Zeroed texel offsets: no gain.
- **New diagnostics** (`FM2_PERF=1`):
  - texture-fetch, blend, indexed-draw and heavy-shader censuses;
  - a shader dump (`REGPU_MEASURE_DUMP_PS`);
  - new ceiling switches: `REGPU_MEASURE_SKIP_BLENDED=3|4`, `REGPU_MEASURE_TFETCH_MAX`, `REGPU_MEASURE_FAST_TFETCH`,
    `REGPU_MEASURE_NO_TEXEL_OFFSET`.
- **Open lead:** `REGPU_MEASURE_FAST_TFETCH` (a stripped texture fetch) saved ~2 ms, and neither the helper's checks nor the texel
  offsets explain it. The suspect is the per-instruction filter override (linear vs point). Worth one test on the Intel box.

## The rules (set by the user, non-negotiable)

1. **ReGPU only.** Never change FM2 game code. The cuts must carry over to every game on ReGPU.
2. **Degrade, never remove.** Every effect still exists. On the potato preset it may look ugly, but it must stay recognisable.
3. **Strictly generic.** No per-game hints.
4. **Every cut is gated.**
   - With Hyper off, nothing changes.
   - Every cut has a `REGPU_HYPER_*` switch and a `settings.ini` key, so it can scale with the hardware.
5. **No measured gain, no code.** A cut that doesn't pay off is reverted and kept as a patch in `experiments/`.
6. **Log every experiment**, failures included. Each entry says how to recreate it and carries one of these flags:
   - **[PROVEN]:** at least 3% fps or 0.5 ms GPU on the Vega laptop.
   - **[RETEST ON INTEL]:** smaller than that on the laptop.
7. **Potato target:** 720p native, with fps as high as possible. Lowering the resolution is the last resort.
8. **Bench method:** 2 parked pairs per experiment. Report to the user in batches of 4–5 experiments, each with a summary table.

## Layout

| Path | What |
|---|---|
| `README.md`, `RESUME-PROMPT.md` | This entry point and the prompt for the next agent |
| `HYPER.md` | Design: rules, per-cut limits, presets |
| `HYPER-CHANGES.md` | **The ledger.** Kept cuts, tooling, diagnostics, parked experiments, ceiling measurements, flags |
| `experiments/` | A patch script or diff for every experiment, kept or parked, so each can be redone (`python experiments/X.py Forza-Motorsport-2-ReGPU`) |
| `retro/hyper-retrospective.html` | Retrospective of rounds 1–2 (also published as a private claude.ai artifact) |
| `fm2-regpu-triage.md` | FM2/ReGPU triage notes, including AMD shader fixes to raise with Scott, the ReGPU author |
| `profiles/` | Radeon GPU Profiler captures from the laptop (AMD only) |
| `_ref/` | Reference copies: ReGPU, PGR4-ReGPU, ReIGlue |
| `_dl/` | Build logs and the `--racenow` flow XML. The large AMD tools and the duplicate ffmpeg/sdl downloads were left out |
| `data/` | Title update file. **The 7.3 GB disc ISO is not included**: it's already extracted in `Forza-Motorsport-2-ReGPU/game/` |
| `Forza-Motorsport-2-ReGPU/` | The FM2 port (a git repo, `.git` included) with all the Hyper work **uncommitted** in the working tree |
| `Forza-Motorsport-2-ReGPU/game/` | The extracted game (`default.xex` + `Media/`) |
| `Forza-Motorsport-2-ReGPU/build/rel/` | The laptop's built `fm2.exe` + DLLs + `.pdb` only. Object files were left out, so rebuild on the new machine |
| `Forza-Motorsport-2-ReGPU/out/` | `bench.txt` (every benchmark line), `bench_*.err` logs, `*_compare.png` picture checks |
| `_handoff/hyper-uncommitted.patch` | `git diff` of all Hyper changes against the base commit (`git-base-commit.txt`); untracked new files are listed in `git-status.txt` |
| `_handoff/fm2-userdata/` | Copy into `%USERPROFILE%\Documents\fm2\`: `settings.ini`, the pipeline manifest, static shaders, profiles. The AMD driver pipeline cache and the 417 MB shader cache were left out; they rebuild on first run |
| `_handoff/agent-memory/` | The previous agent's memory notes: setup, plan, logging rule |
| `_handoff/session-transcript/` | Full transcript of the laptop session (JSONL), to look up exact details |

## Setup on the new machine

1. Extract the package to `C:\Users\<you>\Desktop\reGPU_Hyper` (any path works, but you'll have to reconfigure the build).
2. Copy `_handoff/fm2-userdata/*` to `%USERPROFILE%\Documents\fm2\`. Keep `online_server=off` in `settings.ini`: with it on, the
   game crashes after the title screen.
3. Install the tools:
   - Visual Studio 2022 (Community or Build Tools) with the C++ workload. Its CMake and Ninja are used.
   - LLVM in `C:\Program Files\LLVM`. The compiler is `clang-cl`.
   - Windows 10/11 SDK.
   - Git for Windows: the bench scripts run in Git Bash.
   - Python 3 with the `py` launcher, plus `pip install pillow`.
4. Try the packaged exe first: `Forza-Motorsport-2-ReGPU/build/rel/fm2.exe game`.
5. To rebuild, delete `build/rel/CMakeCache.txt`, then reconfigure from `Forza-Motorsport-2-ReGPU`:
   ```
   tools\vcenv.cmd cmake -G Ninja -B build\rel -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
   tools\vcenv.cmd cmake --build build\rel --target fm2
   ```
   - `tools/vcenv.cmd` looks for VS 2022 in either install location.
   - If it can't find `py`, prepend `%LOCALAPPDATA%\Programs\Python\Launcher` to PATH.
   - `thirdparty/` is complete: FidelityFX, DLSS, ffmpeg, SDL3, XenonRecomp, and meshoptimizer v0.22 (the user approved it).
     The `.git` folders of the DLSS and FidelityFX SDKs were dropped to save space.
6. **The HD 630 is not an AMD GPU.** Watch for:
   - shader or driver problems;
   - the zero-copy path: the laptop used an `ID3D12Device3` fallback.

   Check the log and `fm2.log` on first boot.

## Benchmarking

Run from Git Bash in `Forza-Motorsport-2-ReGPU/`:
```
export BENCH_RACENOW="drive=0" BENCH_PERF=1
sh tools/hyper_bench.sh <label> 30 FM2_UPSCALER=off REGPU_HYPER_ANISO=2 [REGPU_HYPER_*=...] [FM2_DUMP_AT=25]
```
- **Run:** one parked race (Nissan Speedway, booted straight in with `--racenow`, no driving), measured for 30 s.
  - Each run appends a line to `out/bench.txt`.
  - `FM2_DUMP_AT=25` saves `out/bench_<label>.png` for picture checks.
- **Columns:**
  - **race fps:** the number to use.
  - **meter avg:** includes the loading screens, so ignore it. It was wrongly used as fps until 7 Nov.
  - **GPU ms/frame:** the most reliable. Driving runs have about ±3.5 fps of noise, which is why runs are parked.
- **Failures:** the script retries crashes and stalls (180 s timeout, 3 attempts) and logs failures to `out/bench_failures.txt`.
  There is a rare known startup crash in ReGPU: a pipeline built from a shader marked Ready with no bytecode, about 1 in 195 runs,
  not yet fixed.
- **Diagnostics:**
  - `FM2_PERF=1`: per-section CPU timings, the "pipeline split" (shows the GPU fence wait), and the resolve census.
  - `FM2_GPU_STATS=1`: vertex and pixel-shader counts.
  - `FM2_GPU_PASSES=1`: GPU time per pass.
  - `REGPU_MEASURE_*` ceiling switches: see the diagnostics table in `HYPER-CHANGES.md`.

## Where the work stands

**Kept, in presets:**

| Cut | Presets | Notes |
|---|---|---|
| Whole-frame render scale | Mild 50 / Low 35 / Extreme 25 | |
| Cheap one-pass upscale | Low, Extreme | |
| Mesh decimation | Extreme 25% | |
| Anisotropy cap | Mild 4×, Low/Extreme 2× | |
| Helper-surface scale (reflections, blur, bloom) | Mild 75% / Low and Extreme 50% | +11 to +20% race fps at native res |
| 32-bit 10:10:10:2 resolves | Every preset | Lossless, about −0.5 ms |

**Measured on the laptop at 720p native:** about 19–21 ms of GPU per frame, so it's **GPU-bound**. The renderer's real CPU work is
about 8 ms; the other ~7.5 ms of its time is waiting for the GPU. The main view shades each pixel about 8.8 times, but it isn't
opaque overdraw: a depth prepass was built and lost about 1.3 ms.

**First jobs on the Intel box:**

1. **Baseline.** Hyper off, then `REGPU_HYPER=mild|low|extreme`, at 720p native. Use the `FM2_PERF` pipeline split to see whether
   the i3 or the HD 630 is the limit.
2. **Re-run every [RETEST ON INTEL] row** in `HYPER-CHANGES.md`:
   - helper scale at 25%;
   - bilinear filtering;
   - mip drop;
   - no-discard (v2 key fix);
   - 10:10:10:2 resolves;
   - decimation at native resolution (the HD 630 is weak at vertex work);
   - half-rate helper passes;
   - 8-bit colour surfaces;
   - parallel draw workers (`FM2_DRAW_WORKERS`);
   - the prepass, only if the GPU turns out pixel-starved.
3. **If CPU-bound:** start with per-draw constant setup, the largest CPU cost at 3.5 ms per frame on the laptop.
4. **Keep climbing.** Log every result, then stack the winners into presets down to the potato tier.

**Caveats:**
- `settings.ini` currently has `anisotropy=0`. The anisotropy cap only matters when the player's setting is higher (the default is 16).
- Benchmarks pass `FM2_UPSCALER=off` to get native resolution.

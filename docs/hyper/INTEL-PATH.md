# Intel path: what this thread did, for consolidation with the other machine

**Branch point:** this thread starts at the contents of `reGPU_Hyper_transfer.zip` + `reGPU_Hyper_update_2026-11-07.zip`
(unpacked on 2026-11-08). Everything below is a change made after that point, on this machine only. Anything the
other machine did after the same zips is the other branch.

## Every change since the zips (not only Hyper cuts)
- `src/gpu/renderer.cpp` `PrecompileAll`: skip manifest keys whose shader ucode is missing (boot hang fix).
- `%USERPROFILE%\Documents\fm2\pipelines.manifest` renamed `.laptop` (laptop manifest listed 1,239 missing ucode files).
- `out/ppc` regenerated, `build/tools` deleted and rebuilt (`tools/intel_rebuild.ps1`); `src/kernel/stubs.cpp` touched by the regen (backed up first).
- Hyper code: the kept switches below, plus `hlsl.cpp`/`hlsl.h` preamble and emitter changes, `CMakeLists.txt` adds meshoptimizer `vcacheoptimizer.cpp` and `overdrawoptimizer.cpp`.
- `tools/hyper_bench.sh` prints `median N fps`; new tools `tools/intel_*.ps1`, `imgdiff.py`, `median_fps.ps1`.
- Machine setup only (no repo effect): VC++ redist, Defender exclusion for `build/`.

Machine: i3-7100T + Intel HD 630, 720p native, parked `--racenow` Nissan Speedway race, median fps from 10 s in.
Started from the laptop handoff (`reGPU_Hyper_transfer.zip` + `update_2026-11-07`). All code is ReGPU-side
(`src/gpu/`, `src/settings.cpp`, `CMakeLists.txt`); no FM2 game code. Full detail per run: `HYPER-CHANGES.md`.
Kept sources snapshot: `experiments/intel-keep/`. Bench tools: `Forza-Motorsport-2-ReGPU/tools/intel_*.ps1`.

## Focus of this thread
The HD 630 is shader-ALU bound (not bandwidth). This thread therefore went after **shader arithmetic and
per-draw shader specialisation**, then helper passes. Things the AMD laptop found useless often pay here.

## Kept (stack order, cumulative median fps; native 15.2)
| # | Switch | What it does / where | Intel effect | Origin |
|---|---|---|---|---|
| 0 | laptop potato stack: `FM2_UPSCALER=off ANISO=0 HELPERS=25 RESOLVE1010=1 DECIMATE=10` | existing laptop cuts | 18.5 fps | laptop |
| 1 | `REGPU_HYPER_NODISCARD=1` (`hyper_nodiscard`) | renderer.cpp `NoDiscardShader`, key flag bit 25 | +15% → 21.25 | laptop patch, first kept on Intel |
| 2 | `REGPU_HYPER_SHADER_OPT=1` | renderer.cpp `ShaderOptLevel`/`OptFlags`: fxc /O1 instead of skip-opt; `CacheSeed` changes | → 22.15 | **Intel** |
| 3 | `REGPU_HYPER_VCACHE=2` | decimation `Worker`: meshopt vertex-cache + overdraw reorder | → 23.15 | **Intel** |
| 4 | `REGPU_HYPER_PLAIN_MUL=5` | hlsl.cpp: plain `a*b` in VS; PS uses taint analysis (`MulP/DotP` only where a value may be inf/NaN) | → 25.85 (VS), 27.5 (PS smart) | **Intel** |
| 5 | `REGPU_HYPER_FETCHSPEC=1` | renderer.cpp `FetchModeBits`, `FetchSpecShader`, `g_fetchSalt` (key flag bit 29): per-draw PS variant with texture sampler/clamp/cube modes baked as constants | **→ 33.35 fps, 30.25 ms GPU** | **Intel** |

Settings keys for 2–5 are not added yet (phase 7).

## Rejected on Intel (patch kept in `experiments/`)
bilinear filter, no depth alpha test, workers=3 (crash), looser decimation, fast fetch, heavy-shader mip bias, mip drop,
half-precision regs (`intel_half_regs.diff`, driver ignores min16), depth prepass (`intel_prepass_merged.diff`, worse),
weld, 8-bit colour, full clears, alpha-func variants, sloppy decimation (smears textures).

## Ceilings on the phase-3 stack (33.6 fps / 30.85 ms)
blended draws 9.5 ms, cars 5.6 ms, helper passes 4.6 ms, CPU-only ceiling ~57 fps.

## Phase 5 (closed, nothing kept)
Half-rate helpers (`REGPU_HYPER_HALFRATE`): +7 to +9.5% but breaks the car reflection and shadow in all three tries; rejected, patch `intel_halfrate.diff` (v2/v3 steps described in HYPER-CHANGES.md).

## In progress (old note)
- Phase 5 half-rate helpers (`REGPU_HYPER_HALFRATE`, `hyper_halfrate`; `experiments/intel_halfrate.diff`):
  +7–9% but broke car reflection/shadow; third attempt keeps the resolve size-variant swap, skips only the pixel copy.

## Scope split with the AMD thread
The AMD agent saw this roadmap (phases 1-7) and works only outside it. This thread stays inside it:
- shader ALU and fetch specialisation;
- helper passes;
- cheaper blended shaders;
- CPU per-draw work;
- Intel preset and auto-detect.

The two climbs don't overlap on performance work. At merge, check whether their gains stack: both threads cut the same GPU frame.

## Merge plan (this tree is the merge target)
When the AMD laptop's research arrives:
1. Diff its changes against the same zip branch point.
2. Sort each item: in both / AMD-only / Intel-only.
3. Port every AMD-only cut into this tree behind its own switch.
4. A/B each one on Intel (two parked pairs plus a picture check), keep or reject it, and log it here as AMD-originated.

## Notes for merging
- Key flag bits used by this thread: 25 (no-discard), 29 (fetch-spec salt). Check for clashes.
- `intel_ab.ps1` fix: `-Base/-Test` split only on commas followed by `NAME=`.
- Shader-hang fix in `PrecompileAll` (skip manifest keys whose ucode is missing) — needed on any fresh machine.

## Merge of the laptop (AMD) thread — 2026-11-09
Source: `Downloads\reGPU_Hyper_handoff-to-intel_2026-11-09.zip`, extracted to `Desktop\laptop-handoff` (not over this tree).
Pre-merge snapshot: `experiments/intel-premerge/`. Merged snapshot: `experiments/intel-keep/`.
- `renderer.cpp`, `main.cpp`: 3-way `git merge-file` (Intel / fork base rebuilt by reverse-applying `laptop-since-fork.patch` / laptop), **no conflicts**.
- `hyper.h`, `hyper.cpp`: hand merge. Intel's no-discard kept, laptop's Potato preset, `HyperLeanFetch`, `HyperLeanVFetch`, `HyperCullPixels`, `HyperCullAll` added (marked `[laptop]`).
- `tools/hyper_bench.sh`: laptop's loading-screen rerun added, Intel's median kept. `tools/fm2ctl.py`: laptop's. `fps_meter.*`, `autodrive.*`: identical on both sides.
- Pipeline-key flag bits: no clash (laptop's lean fetch aliases the shader hash; Intel uses bits 25 and 29).
- Overlaps resolved by choice, both left in code behind their switches: laptop `LEAN_FETCH` (incl. `fastMul`) vs Intel `FETCHSPEC` + `PLAIN_MUL=5` (Intel's kept in the stack); laptop `FREEZE` vs Intel half-rate helpers (rejected here, laptop's retested).
- Merged build on the Intel stack: 37.3 fps median, 26.4 ms GPU (unchanged).

### Laptop cuts retested on Intel (batch m2)
| Laptop cut | Origin | Intel result | Verdict |
|---|---|---|---|
| Draw culling by pixel count (`CULL_PIXELS=64 CULL_ALL=1`) | laptop | median 37.25 → 37.9 (+1.7%), GPU −0.55 ms, picture 1.42% | candidate → kept in combo |
| Cull in helper and shadow passes too (`+CULL_PASSES=1`) | laptop | 37.6 → 38.75 (+3.1%), −0.8 ms, picture 2.11% | candidate → kept in combo |
| Lean vertex fetch (`LEAN_VFETCH=1`) | laptop | 37.65 → 37.55, GPU same | rejected on Intel: no gain |
| Half-precision pixel temps (`REGPU_MEASURE_HALF=1`) | laptop | 37.45 → 37.6, GPU same | rejected on Intel: no gain (driver ignores min16, as Intel's own half-regs test) |
| `A2M_TEST=1` | laptop | 37.75 → 44.3 (+17.6%), −4.8 ms | **rejected: removes content** (crowd, canopy fabric, chain-link fence vanish; 13% of pixels) |
| Frozen helper passes (`FREEZE=120 FREEZE_REFRESH=3`) | laptop | 37.55 → 39.45 (+5.1%), −1.0 ms, picture identical | candidate → kept in combo |
| **Combo: cull + cull passes + freeze** | laptop | **37.55 → 40.25 (+7.2%), GPU 26.75 → 25.9 ms**, picture 2.26% (tiny culled draws) | **KEPT, in the Intel stack** |
Not tested here: laptop `LEAN_FETCH` (and its `fastMul`), superseded by Intel `FETCHSPEC` + `PLAIN_MUL=5`; potato preset (phase 7 builds the Intel preset on it).
| Lean LOD (`REGPU_HYPER_LEAN_LOD=1`), hlsl.cpp `TranslateShader` | Intel | 39.4 → 40.8 (+3.6%), −1.1 ms, lossless | **KEPT** |

## Phases 4-7 closed (10 Nov)
- Phase 4 kept: swizzle selects (+12.8%), lean LOD (+3.6%). Rejected: bool specialisation, cube-map bias (no gain).
- Phase 6 closed by measurement: CPU-only ceiling 76 fps vs ~41 fps GPU-bound: no payoff now.
- Phase 7: `hyper=auto` + Potato with an Intel set and the laptop's set for other vendors; every switch has a `hyper_*` settings key. **Stock 14.9 → `REGPU_HYPER=auto` 41.3 fps median (+177%)** on the HD 630.
## Climb 2, waves 1 and 2a (Intel, HD 630, 720p native)
| Cut | Switch | Result | Picture | Verdict |
|---|---|---|---|---|
| B1+B2 tail baked per draw | REGPU_HYPER_TAILSPEC=1 | 44.4 -> 47.4 fps, GPU 22.7 -> 21.2 ms (3 pairs) | lossless (timer noise) | KEPT |
| C5 cull threshold 256 px | REGPU_HYPER_CULL_PIXELS=256 | 42.2 -> 44.4 fps (+5.3%) | subtle | KEPT |
| C5 cull 128 | =128 | +2.5% | clean | superseded by 256 |
| C5 cull 384 | =384 | +2.2% | car rear badge lost | REJECTED |
| A2 bias baked / plain Sample | REGPU_HYPER_BIASSPEC=1 | no gain | - | REJECTED |
| B3 flow prefer / avoid | REGPU_HYPER_SHADER_FLOW=1/2 | none / picture broken | - | REJECTED |
| E6 freeze refresh 4/6 | FREEZE_REFRESH | +0.5-1% (noise) | clean | not kept |
| E6 frozen shadows | FREEZE_SHADOWS=1 | none | 28% wrong | REJECTED |
| C1 null pixel shader on depth-only draws | REGPU_HYPER_NULLPS=1 | no gain (44.4 vs 44.2) | clean | REJECTED |
Potato on Intel now defaults TAILSPEC=1 and CULL_PIXELS=256. Stack: 47.4 fps.
| E1 resolve by copy | - | closed by analysis: nearly every resolve converts format or scales colour | - | NOT BUILT |
| E2 dead resolves skipped | REGPU_HYPER_DEAD_RESOLVE=60/8 | no gain (47.4 vs 47.4) | 6-8% wrong | REJECTED, code reverted, patch in experiments/w2_deadresolve_patch.py |
| B4 predicated ops flattened | REGPU_HYPER_PRED_FLATTEN=1 | no gain (47.4 vs 47.1) | clean | REJECTED, reverted |
| C3 interpolator trimming | - | closed by analysis: fxc already drops unread inputs; outputs must match layout | - | NOT BUILT |
| Tuning: helpers 15%, decimate 5, freeze refresh 5 | HELPERS/DECIMATE/FREEZE_REFRESH | all within noise (GPU 21.2-21.5 both sides; fps base noisy 43-46) | clean | not kept |
| B10 dxc (SM 6.0, -O3) instead of fxc | REGPU_HYPER_DXC=1 | GPU 21 -> 140 ms, 6 fps | 74% wrong | REJECTED, reverted; patch experiments/w3_dxc_patch.py. Likely register array spilled to scratch on Intel plus a semantic difference; B9 (16-bit via dxc) closed with it |
| Baseline drift check | - | clean build 45.7 fps, pre-cleanup snapshot 43.7 fps, same GPU 21-22 ms: the machine slowed (CPU side), the cleanup did not cost anything | - | NOTE |

## Track G (user OK 2026-11-10): FSR 1 scene scale and output size (native base 45.6 fps, GPU 21.7 ms)
| Test | Switches | GPU ms | fps | Verdict |
|---|---|---|---|---|
| sc90 720p out | FM2_UPSCALER=fsr1, FM2_FSR_SCALE=90 | 23.5 | 43.5 | slower: FSR pass costs more than it saves |
| sc80 720p out | FSR_SCALE=80 | 21.2 | 48.1 | +5%, slightly soft |
| sc70 720p out | FSR_SCALE=70 | 19.1 | 53.1 | +16%, screenshot hit loading screen, unjudged |
| hd900: 720p -> 1600x900 | FM2_RESOLUTION=1600x900, FSR_SCALE=80 | 28.7 | 35.6 | -22% |
| hd720: 720p -> 1080p | FM2_RESOLUTION=1920x1080, FSR_SCALE=67 | 32.3 | 31.1 | -32% |
| hd576: 576p -> 1080p | FSR_SCALE=53 at 1080p | 27.6 | 36.7 | -20% |
Notes: REGPU_HYPER_SCENE only lowers below the settings file render_scale (was 50): use FM2_FSR_SCALE. The pixel cull threshold is in host pixels, so lower scales cull more (crowd lost at 50%): scale it by area before keeping any scale. Nothing adopted yet.
Decision (user, 2026-11-10): stay native. No resolution lever adopted; Track G closed.
| B5 smart multiply v2 | ceiling REGPU_HYPER_PLAIN_MUL=1 (all plain) | 44.2 vs 45.0: no headroom | - | CLOSED by ceiling |
| B6 zero constants baked per draw | REGPU_HYPER_ZEROSPEC=1 | 41.9 vs 43.3, GPU equal | clean | REJECTED, reverted; patch experiments/w4_zerospec_patch.py |
| C2 position-only VS for depth-only | - | closed: frozen shadow maps saved only 0.25 ms, C1 nothing | - | NOT BUILT |
| D1/D2/D3 draw order, pre-pass | - | closed by the 7 Nov Intel pre-pass test (-3.4%): opaque overdraw is only ~10% of main-view PS runs | - | NOT BUILT |
| A6 static texture table | - | closed: L effort; fetch spec and swizzle bake already removed the per-fetch work that measured; remaining bindless index read is one cbuffer load | - | NOT BUILT |
Phases 8-11 (climb 2 waves 0-3) closed 2026-11-10. Kept: TAILSPEC (+6.5%), CULL_PIXELS=256 (+5.3%).

## Flicker study (2026-11-10): 60 consecutive frames per config, car parked; tools/flicker_capture.ps1, flicker.py
Excess flicker = pixels with 3+ A-B-A flips that never flip in stock. Stack 0.53-0.64% of the screen.
| Config | Excess flicker | Finding |
|---|---|---|
| stack minus CULL_PIXELS | 0.05% (noise) | culling is the whole stack's flicker: a culled draw is redrawn every 4th frame to re-measure it, so it blinks |
| stack plus FREEZE_SHADOWS | 13.8% | the car's shadow area flickers: never offer |
| every other leave-one-out / extra | same as stack | no flicker of their own |
| **Fix: REGPU_HYPER_CULL_PROBE=1** | **0.05%** | re-measure draw writes no colour and no depth; fps 44.9 vs 44.9 (3 pairs): **KEPT**, in stack and Intel potato defaults; patch experiments/flk_cullprobe_patch.py |

## Climbs from review round 2 (2026-11-11)
| Climb | Cause | Fix (switch) | Flicker | Speed (3 pairs) | Status |
|---|---|---|---|---|---|
| Frozen shadow maps | FM2 resolves a 256x256 shadow map and a 320x184 bloom level to one address; frozen, the bloom level stayed in front and the shadow read it | REGPU_HYPER_RESOLVE_BY_SIZE=1: a fetch uses the same-address variant of its own size | 16.4% -> 0.067% (floor) | frozen shadows still no gain (43.6 vs 43.6); size fix alone neutral | fixed; awaiting user (keep available or drop) |
| Culling 256 residual flicker | objects at the threshold flip between culled and drawn | REGPU_HYPER_CULL_HYST=150/200: culled objects return only above threshold x percent | 0.085% -> 0.062% / 0.056% | neutral (+0.9% / +0.4%, noise) | awaiting user review |
Patch: experiments/climb_shadow_cull_patch.py. Test tooling added (not Hyper): async sweep PNG writes, FM2_DUMP_AFTER_RACE capture trigger, FM2_AUTODRIVE_STRAIGHT; tools/motion_capture.sh, clean_ab.sh, clip_mp4.py.
Round 3 (user): CULL_HYST=200 and RESOLVE_BY_SIZE=1 KEPT (stack + Intel Potato defaults); FREEZE_SHADOWS fixed and kept available, off by default.

## Conclusion benchmark (2026-11-11): every preset at native 720p, no FSR, 3 interleaved runs, median of medians
| Preset | fps | GPU ms | vs stock |
|---|---|---|---|
| Stock (off) | 14.9 | 63.1 | — |
| Mild | 17.9 | 52.0 | +20% |
| Low | 18.9 | 50.8 | +27% |
| Extreme | 19.7 | 49.2 | +32% |
| Potato | 44.3 | 20.6 | +197% (3.0x) |
| Auto (picked Potato) | 43.6 | 21.0 | +193% |
Mild/Low/Extreme forced to native (REGPU_HYPER_SCENE=100, no upscale) carry only their non-resolution cuts, not the Intel shader cuts. Page: https://claude.ai/artifact/M2y6mQyC5ce1kZA6jZieS4 . This closes the Intel climb.

## Laptop package 2 (Turbo knowledge) redone on the HD 630 (2026-11-11)
Baseline fix: settings.ini has anisotropy=0; every run now forces FM2_ANISOTROPY=16 (true native 14.9 fps, same as before: anisotropy is worth only 1-3% here). Earlier "stock" figures and the round-2 texture-filtering clips had no anisotropic filtering.
Full write-up: intel-research/INTEL-RESEARCH-HD630.md; four-pager https://claude.ai/artifact/NLj1xBXpAbbESnCEhgBKzJ ; benches tools/iq_bench.sh, iq_bench2.sh, iq_stats.py, iq_diffs.py; visuals intel-research/make_visuals.py.
Intel Turbo (chosen, v3) = NODISCARD SHADER_OPT PLAIN_MUL=5 FETCHSPEC SWIZZLE_SEL LEAN_LOD TAILSPEC RESOLVE1010 RESOLVE_BY_SIZE LEAN_FETCH=3 LEAN_VFETCH: 14.75 -> 34.2 fps (+132.7%), diff 0.99 (noise 0.62).
Potato 46.6 (+212.8%). Laptop Turbo here 23.5 (+57.7%). Skip-0-px culling costs -7.4% alone on Intel: not upstream here.

## 14 Nov: cull 256 -> 128 (round-4 review: distant cars vanished)
Cause: at mid distance each piece of a car (body, windows, wheels) is under 256 px even when fully visible, so pieces were culled one by one; the 200% comeback margin kept them gone up to 512 px.
Potato driving fps (3 rounds, medians): cull 256 49.3 / 48.0, cull 128 47.3 / 48.7 (inside noise), MAXIDX 1024 46.0, MAXIDX 4096 46.7, cull off 43.3.
Parked flicker cull 128 vs stock: 0.072% (floor 0.064%). Visual: cars whole at 128.
Rejected and removed (no fix, no gain): REGPU_HYPER_CULL_MAXIDX (never cull draws over n indices), REGPU_HYPER_CULL_JUMP (come back on growth, 125/150%), HYST 120. All still broke cars at 256.
Kept: Intel Potato default REGPU_HYPER_CULL_PIXELS 256 -> 128 (hyper.cpp). Clips: out/motion/r5*; review page round 5.
## 14 Nov: Potato + FSR 3 flickered on every edge (6.3% parked) -> RESOLVE1010 off under temporal upscaling
Bisect (parked flicker vs stock, floor 0.064%): Potato+FSR3 6.35%; without culling 6.33%, without freeze 6.35%, without fetch/tail/swizzle specialisation 6.40%; without RESOLVE1010 0.011%.
Cause: RESOLVE1010 keeps the scene resolve in R10G10B10A2, so FSR 3 is created without HDR (needs R16G16B16A16_FLOAT) and its accumulation fails: edges flip every frame.
Fix (generic, renderer.cpp resolve format): no 10:10:10 resolve while a temporal upscaler runs (g_temporal). 720p and FSR 1 unchanged.
Also: Potato 720p "excess" 0.26% is a timing artefact (Potato reaches the capture frame 4 s into the race with AI cars still moving; the stock reference is at ~19 s).

## 14 Nov: CORRECTION, Potato + FSR 3 speed
Every Potato + FSR 3 measurement before 14 Nov (31-33 fps parked/driving, FSR-STUDY and the FSR showcase page) ran FSR 3 without HDR because RESOLVE1010 kept the scene in R10G10B10A2: edges flickered (6.3% parked). Fix: no 10:10:10 resolve while a temporal upscaler is set (TemporalUpscalerSet(), by setting). Correct HDR numbers, median of 3, parked / driving / 1% low: Potato + FSR 3 19.3 / 19.3 / ~5.5; full stack (+ frozen shadows) + FSR 3 18.7 / 19.3 / ~5.3; Potato + FSR 1 36.7 / 38.7 / ~9; Potato 720p 47.3 / 48.0. Parked flicker of the fixed full stack + FSR 3: 0.012%. One run hung on the loading screen (169 draws) and is excluded.

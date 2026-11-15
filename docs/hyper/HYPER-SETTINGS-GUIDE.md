# Hyper settings guide

Every Hyper switch in this tree: what it does, what it buys, what the player sees, and whether it flickers. Written as
the knowledge base for a future settings screen. All switches are generic ReGPU code (no game knowledge); numbers come
from FM2 on the Intel machine (i3-7100T, HD 630, 720p native, parked race on Nissan Speedway) unless marked otherwise.

## How this was measured
- **Speed:** median fps from 10 s into a parked race, A/B pairs interleaved (`tools/intel_run.ps1`). Gains are
  measured on top of the stack before them, so they don't add up exactly.
- **Picture:** a screenshot pair (switch off / on) diffed per pixel (`tools/imgdiff.py`), then looked at by eye.
  Run-to-run noise alone gives about 1.5% of pixels over 8/255 (race timer, crowd animation).
- **Flicker:** 60 consecutive frames per configuration with the car parked (`tools/flicker_capture.ps1`), each pixel
  checked for A-B-A flips: a change that snaps back on the next frame, 3 or more times in 60 frames
  (`tools/flicker.py`). **Excess flicker** is the share of the screen that flips like that with the switch but never
  in stock. The full stack is measured with each switch removed in turn, and with each extra switch added.
  Noise floor: 0.05%. Heat maps: `Forza-Motorsport-2-ReGPU/out/flkmap/`.
- **Driving:** the same capture with the auto-driver on. The A-B-A test does not work in motion: every run takes a different line, and high-detail textures shimmer frame to frame, so 25% (stock) to 40% (stack) of the screen counts as flipping. In motion the comparison is by eye: side-by-side GIFs in `flicker-review/`.

## Summary for the interface

| Group | Switches | Advice |
|---|---|---|
| **Free speed (lossless, no flicker)** | NODISCARD, RESOLVE1010, SHADER_OPT, VCACHE, PLAIN_MUL=5, FETCHSPEC, SWIZZLE_SEL, LEAN_LOD, TAILSPEC | Always on in a performance mode; no visible change. Can be a single "Shader optimisations" toggle. Changing any of them recompiles every shader once (long first load). |
| **Picture trades (visible, no flicker)** | HELPERS, ANISO, DECIMATE, FREEZE + FREEZE_REFRESH | Each lowers quality in one area the player can name. Expose as sliders with a one-line "what you'll notice". |
| **Culling (fixed flicker)** | CULL_PIXELS, CULL_ALL, CULL_PASSES + **CULL_PROBE** | Small far objects vanish. Flickered before the probe fix; **CULL_PROBE must always be on with culling.** |
| **Resolution** | FM2_UPSCALER, FM2_FSR_SCALE, REGPU_HYPER_SCENE, REGPU_HYPER_UPSCALE | Softer picture; on this GPU only 70% or lower pays. The user chose native for the Intel preset. |
| **Never offer** | FREEZE_SHADOWS, A2M_TEST | Heavy flicker or removed content. |
| **Superseded / no gain on Intel** | LEAN_FETCH, LEAN_VFETCH, CULL_RETEST | Keep off; hide from the interface. |

## Switch by switch

Each entry: what it does · speed · picture · flicker · what the player should expect when turning it on.

### Lossless shader and pipeline cuts

**REGPU_HYPER_NODISCARD=1** (settings key `hyper_nodiscard`)
- Does: draws whose alpha test is off use a pixel shader compiled without its `discard`, so the GPU keeps early depth testing.
- Speed: +15% on Intel (the first big win).
- Picture: identical. Flicker: none (removing it from the stack changes nothing).
- Expect: nothing visible. Changing it recompiles shaders.

**REGPU_HYPER_RESOLVE1010=1** (`hyper_resolve1010`)
- Does: resolves into the guest's 10:10:10:2 format stay 32-bit on the host instead of 64-bit float: half the bandwidth.
- Speed: part of the laptop base set; small on Intel (shader-bound, not bandwidth-bound).
- Picture: identical. Flicker: none.
- Expect: nothing visible.

**REGPU_HYPER_SHADER_OPT=1**
- Does: compiles translated shaders with fxc optimisation level 1 instead of none.
- Speed: +4% on Intel.
- Picture: identical. Flicker: none.
- Expect: nothing visible in play. **First launch after turning it on compiles every shader again, and optimised compiles are slower** (seconds each for the biggest shaders): a long first load, then normal.

**REGPU_HYPER_VCACHE=2**
- Does: reorders each mesh's triangles for the vertex cache and for less overdraw (same triangles, new order), on a worker thread.
- Speed: +4.5% on Intel.
- Picture: identical. Flicker: none.
- Expect: nothing visible.

**REGPU_HYPER_PLAIN_MUL=5**
- Does: Xenos multiplication treats 0 × anything as 0; the translator keeps that rule only where an operand may hold an infinity or NaN, and uses plain multiplies elsewhere (vertex shaders all plain).
- Speed: +12% (vertex) and +6% (pixel) on Intel.
- Picture: identical in testing. Flicker: none.
- Expect: nothing visible. A game that relies on 0 × infinity in a way the analysis misses would show black or white specks; none seen in FM2. Recompiles shaders.

**REGPU_HYPER_FETCHSPEC=1**
- Does: per draw, compiles a pixel-shader variant with each texture's sampler, clamp and cube mode baked in.
- Speed: +21% on Intel (the biggest single cut).
- Picture: identical. Flicker: none.
- Expect: nothing visible once warm. New variants compile in the background the first time a draw is seen: the first race on a track can stutter briefly, later ones are smooth.

**REGPU_HYPER_SWIZZLE_SEL=1**
- Does: the texture-fetch swizzle as selects instead of an indexed register.
- Speed: +12.8% on Intel.
- Picture: identical. Flicker: none (0.06% without it vs 0.05% floor).
- Expect: nothing visible. **Turning it off is costly**: every shader recompiles and, with SHADER_OPT on, the first load ran past 3 minutes.

**REGPU_HYPER_LEAN_LOD=1**
- Does: drops the forced-LOD test from every texture fetch when the forced-LOD diagnostic is unset.
- Speed: +3.6%.
- Picture: identical. Flicker: none.
- Expect: nothing visible.

**REGPU_HYPER_TAILSPEC=1**
- Does: per draw, bakes the pixel shader's export tail (alpha test function, colour exponent bias, generated parameter, 7e3 ceiling) as literals.
- Speed: +6.5%.
- Picture: identical. Flicker: none.
- Expect: nothing visible; more per-draw variants compile on first sight (as FETCHSPEC).

### Picture trades

**REGPU_HYPER_HELPERS=<25-100>** (`hyper_helpers`)
- Does: offscreen helper surfaces narrower than half the screen (reflection views, blur and bloom levels) drawn at this percent of their size per axis. The screen itself keeps its resolution.
- Speed: part of the laptop base set (25 on Intel); 15 was within noise.
- Picture: car reflections and bloom get softer and blockier the lower it goes.
- Flicker: none.
- Expect: blurrier reflections in car paint and windows, softer glow around bright areas.

**REGPU_HYPER_ANISO=<0|2|4|8|16>** (`hyper_aniso`)
- Does: caps anisotropic filtering.
- Speed: part of the laptop base set (0 on Intel).
- Picture: the road surface, kerbs and markings blur sooner at a distance and at grazing angles.
- Flicker: none.
- Expect: a smeared road texture a few car lengths ahead.

**REGPU_HYPER_DECIMATE=<10-90>** (`hyper_decimate`)
- Does: big meshes drawn with fewer triangles (percent kept), simplified on a worker thread, borders locked so pieces still meet. Tuning: DECIMATE_MIN (smallest mesh, 300 indices), DECIMATE_ERROR (largest change, 0.2).
- Speed: part of the laptop base set (10 on Intel); 5 was within noise.
- Picture: coarser silhouettes on big objects (stands, buildings, terrain).
- Flicker: none when parked. Each mesh swaps to its simplified version once, when the worker finishes it: a one-time pop the first time it's seen (driving results below).
- Expect: slightly angular scenery; a brief pop on first sight.

**REGPU_HYPER_FREEZE=<frames>** with **REGPU_HYPER_FREEZE_REFRESH=<n>**
- Does: a 3D helper view (reflections, environment, lighting views) is drawn normally for FREEZE frames, then only every REFRESH-th frame; in between, the textures it fed keep their last content. Odd REFRESH keeps double-buffered views alternating.
- Speed: part of the laptop cull + freeze combo (+7.2% together); refresh 4, 5 or 6 instead of 3 gained nothing measurable.
- Picture: reflections and environment lighting update at a third of the frame rate.
- Flicker: none when parked (removing FREEZE changes nothing). Driving results below.
- Expect: reflections that lag slightly behind the car's movement.

### Culling

**REGPU_HYPER_CULL_PIXELS=<n>** (laptop), with **CULL_ALL=1** and **CULL_PASSES=1**
- Does: an occlusion query measures how many pixels each draw covers; a draw that covered n pixels or fewer is skipped. CULL_ALL makes every depth-tested draw eligible (not only opaque ones); CULL_PASSES extends it to helper and shadow passes.
- Speed: 64 pixels was part of the +7.2% combo; 256 adds +5.3%; 384 adds +2.2% but loses the car's rear badge.
- Picture: tiny far objects disappear (distant crowd pieces, small signs). At 256 that's subtle; at 384 small car details go.
- **Flicker: yes, without the fix.** A culled draw is drawn again every 4th frame (CULL_RETEST) to re-measure it, so it blinked in for one frame out of four: distant stands, the end of the track, far building edges, the car's badge and lettering. Measured 0.53–0.64% of the screen; without culling 0.05%.
- **Fix: REGPU_HYPER_CULL_PROBE=1** (kept): the re-measure draw writes no colour and no depth, so the query still counts its pixels but nothing reaches the screen. Flicker back to 0.05%, speed unchanged (44.9 vs 44.9 fps, three pairs).
- Expect: with the probe, small distant things are simply absent, steadily. The interface should never let culling run without the probe.

**REGPU_HYPER_CULL_RETEST=<n>**: how often a culled draw is re-measured (default 4). With the probe on, it only affects how fast something that grew becomes visible again (n frames at worst). No flicker of its own.

### Resolution (Track G; the Intel preset stays native)

**FM2_UPSCALER=fsr1** with **FM2_FSR_SCALE=<50-100>**
- Does: draws the scene at that percent of the output per axis, FSR 1 upscales it.
- Speed on Intel (native 45.6 fps): 90% → 43.5 (slower: FSR's own cost), 80% → 48.1, 70% → 53.1.
- Picture: softer, with FSR's edge sharpening; text and the HUD stay sharp.
- Output above 720p (1600×900, 1080p) costs 20–32% on this GPU, even from a native 720p scene.
- Note: `REGPU_HYPER_SCENE` only lowers below the settings file's `render_scale`. Culling's pixel threshold is in drawn pixels, so a lower scale culls more (the crowd went at 50%): scale the threshold with the area before combining them.

**REGPU_HYPER_UPSCALE=1**: the final upscale as one bilinear pass instead of FSR 1: blurrier, cheaper. No flicker.

### Never offer

**REGPU_HYPER_FREEZE_SHADOWS=1**: freezes depth-only helper passes (shadow maps) too. **13.8% of the screen flickers**: the whole shadow area under and around the car. No speed gain. Rejected.

**REGPU_HYPER_A2M_TEST=1** (laptop): alpha-to-coverage draws use a plain alpha test. +17.6% on Intel but **removes content**: the crowd, canopy fabric and chain-link fences vanish. Rejected.

### Superseded or no gain on Intel

**REGPU_HYPER_LEAN_FETCH=<0-3>**, **REGPU_HYPER_LEAN_VFETCH=1** (laptop): the laptop's fetch path, superseded on Intel by FETCHSPEC and PLAIN_MUL; no gain here, no flicker. The Intel preset sets both to 0.

## Rejected experiments (no switch left in the code)
Bias baking, compiler flow flags, null pixel shader, dead resolves, flattened branches, DXC, zero-constant baking,
half-rate helpers and the rest: see `HYPER-CHANGES.md` and `INTEL-PATH.md`. Their patches are in `experiments/`.

## Open items
- Motion: the stack shimmers more than stock on walls and the road while driving (by eye and by the A-B-A count: about 34-40% vs 25% of the screen). Most likely ANISO=0 (no anisotropic filtering) and quarter-size helpers; not yet pinned to one switch. A motion-compensated measure, or a fixed camera path, is needed to attribute it.
- Decimation pop and frozen reflections in motion: not measurable with the parked method; check by eye in `flicker-review/driving_stock_vs_stack.gif`.
- Menus, replays, other tracks and weather: not covered yet.

## Review round 1 (user, 2026-11-10)
The user's verdict on the first review page: the clips were not trustworthy enough. Several stacked many switches,
and the driving clips came from different camera positions. Decisions so far, all provisional until clean clips exist:
| Setting | Decision | Follow-up |
|---|---|---|
| CULL_PROBE | Keep, but players may turn it off | Still some visible flicker by eye: another research pass |
| CULL_PIXELS | A slider: off / 64 / 256, **default 64** | Prove 64 doesn't flicker against stock; climb 256 until it doesn't flicker |
| ANISO 0 | Needs a clean A/B clip | Becomes a climb: keep the gain, remove the shimmer |
| HELPERS 25, FREEZE, DECIMATE | Need clean A/B clips | Hard to see in stacked clips |
| FREEZE_SHADOWS | Climb it: find a way to keep the gain without the flicker | Was "never offer" |
| Shader cuts | One "Shader optimisations" toggle, each one also editable on its own | — |

**Method from now on:** every clip compares stock against stock plus ONE switch, from the same camera at the same
moment. A "climb" means taking a cut that gains speed but shows a side effect, and chasing the version that keeps the
gain without the side effect.

## Review round 2 (user, 2026-11-11): clean clips, stock against stock plus one setting
| Setting | User verdict | Measured parked flicker (floor 0.08%) |
|---|---|---|
| Culling 64 + probe | Clean, identical to stock: **default** | 0.095% |
| Culling 256 + probe | A minimum of flicker, mostly parked; tolerable for Potato | 0.085% |
| ANISO 0 and 2 | Identical to stock, no issue seen | 0.086% / 0.083% |
| HELPERS 25 and 50 | Fine, no flicker or shimmer | 0.065% / 0.080% |
| FREEZE (120, every 3rd) | Fine | 0.075% |
| DECIMATE 10 and 25 | Fine | 0.112% / 0.095% |
| FREEZE_SHADOWS | Climb it: intense flicker parked, less often while moving | 16.4% |
Result: the picture-trade settings are cleared for the interface as they stand. Open climbs: frozen shadow maps, and the residual culling flicker at 256.

## Review round 3 (user, 2026-11-11)
| Setting | User verdict | Applied |
|---|---|---|
| FREEZE_SHADOWS + RESOLVE_BY_SIZE | Fixed, no visible difference: keep it available | RESOLVE_BY_SIZE on in the Intel Potato defaults (lossless); FREEZE_SHADOWS stays an opt-in setting (no speed gain measured) |
| CULL_HYST | 200% is clean; 150% also perfect; 256 without a margin still flickers | CULL_HYST=200 in the stack and Intel Potato defaults |
For the interface: culling always carries CULL_PROBE and CULL_HYST=200; the slider is off / 64 / 256, with 256 in Potato.

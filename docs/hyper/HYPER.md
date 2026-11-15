# Hyper: low spec, high performance mode for ReGPU

Code name for a mode that lets games running on ReGPU run on much weaker hardware. Everything is done at the
ReGPU level, the layer between the game and the GPU, so it works on any game without touching game code.
It is prototyped in the Forza Motorsport 2 port (whose renderer is ReGPU's origin) and meant to move into ReGPU
itself once the ReGPU copies are unified.

## Rules (agreed 5 Nov 2026)

1. **Degrade, never remove.** Every pass, draw and effect still runs. Only resolution, precision, sampling
   quality and (Extreme preset only) refresh rate go down. The game looks blurrier and mushier, never incomplete.
2. **Strictly generic.** ReGPU decides what to degrade from what it sees in the GPU commands (render-target sizes
   and formats, depth-only passes, full-screen quads, texture usage, shader code). No per-game hints or profiles.
3. **Fixed presets** (Off, Mild, Low, Extreme), each a fixed bundle of cuts. Every cut also has its own
   `REGPU_HYPER_*` switch for A/B tests.
4. **GPU and CPU both in scope.** Target: much weaker hardware than the dev laptop (older iGPUs, Steam Deck on
   battery).
5. **ReGPU-ready.** Game-neutral files and `REGPU_` names.

## Limits per cut

| Cut | Allowed |
|---|---|
| Main 3D image | Down to 25% render scale (upscaled) |
| Secondary passes (shadows, reflections, post-processing) | Half resolution or lower |
| Textures | Half size (drop the top mip level, or decode at half size) |
| Sampling | Bilinear, no anisotropic filtering, smaller mip levels |
| Shaders | Lower precision and cheaper sampling, same job |
| Refresh rate | Shadows and reflections every other frame, **Extreme preset only** |
| CPU side | Cheaper decode, upload and per-draw work |

## Plan

| Phase | Work |
|---|---|
| 0. Measure | `--racenow`, auto-drive, FPS meter, `tools/hyper_bench.sh` (done). Label each GPU pass by kind. |
| 1. Framework | `REGPU_HYPER=<preset>`, the list of cuts, presets as bundles. |
| 2. Cuts | 1 secondary passes at half size; 2 main scene to 25%; 3 cheaper sampling; 4 textures at half size; 5 shader precision; 6 refresh rate (Extreme); 7 CPU side. |
| 3. Presets | Benchmark and screenshot each preset; check every effect is still there. |
| 4. Upstream | Package for ReGPU with Scott. |

## Caching

Most cuts change settings, not assets, so nothing new is cached. Textures shrink at decode time (no disk cache).
Shader-changing cuts recompile once per preset; the shader cache is keyed on the shader code, so normal and Hyper
versions live side by side.

## Baseline (dev laptop: Ryzen 3 5300U, Vega iGPU, 720p, FSR 1 at 50%, uncapped)

About 77-82 fps, GPU-bound: ~14.8 ms GPU time per frame. Main scene pass 4.8 ms; two full-screen passes 1.1 ms
each; two 163-draw passes ~0.9 ms each; the rest in many small passes. Renderer CPU ~11.9 ms per frame.

## Findings (5 Nov 2026, dev laptop)

| Experiment | Result | Kept? |
|---|---|---|
| Render scale 50% to 25% (cut 2) | about +15% fps | Yes: `REGPU_HYPER` presets |
| Half-precision vertex outputs | no change in GPU time; visible banding | Removed |
| Optimised shader compile | no change | Removed |
| Isolation, parked on the grid: no game pixels at all | +6% fps | Measurement only |
| Isolation: half of all geometry | +10% fps | Measurement only |
| Isolation: no pixels and half the geometry | +18% fps | Measurement only |

Conclusion: on this machine the GPU side has a ceiling of about +18% even with impossible cuts, so no GPU-side
Hyper cut can be a massive win. The frame is limited mostly by the renderer's CPU work: translating ~1,600 Xenos
draws per frame into D3D12 (pipeline lookup ~3-4 ms, constants ~3 ms, textures ~1.5 ms, submit ~1 ms per frame).
That per-draw CPU cost is where a big multiplier could come from, and weak hardware has weak CPUs too.

Measurement tools kept (off by default): `FM2_GPU_STATS`/`REGPU_GPU_STATS` (vertices, triangles and pixels per
frame), phase-labelled `FM2_GPU_PASSES`, `REGPU_MEASURE_NO_PIXELS`, `REGPU_MEASURE_HALF_VERTS`,
`tools/hyper_bench.sh` (`BENCH_RACENOW="drive=0"` for a parked, repeatable view).

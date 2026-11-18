# Hyper change log

Every change made to the FM2 tree (`Forza-Motorsport-2-ReGPU`) for ReGPU_Hyper. Nothing is committed yet.

Flags: **[PROVEN]** = at least 3% fps or 0.5 ms GPU on the Vega dev laptop; **[RETEST ON INTEL]** = smaller here, redo on the i3-7100 / HD 630.

**Benchmark fix (7 Nov):** until 7 Nov the "avg fps" column of `tools/hyper_bench.sh` was the fps meter's average since boot, which includes the loading screens (~80 shown when the race ran at ~52). GPU ms columns were right, and every flag above was decided on them. The script now prints `race` fps (meter readings after the race starts) and `perf` fps. Corrected race fps from the saved logs: helper scale 50% 45.0/40.4 to 50.2/48.6 (+11-20%).

## Rules

1. **No measured gain, no code.** An optimisation that doesn't improve performance is removed, not left in.
2. **Everything is gated.** With Hyper off and no switch set, the game behaves exactly as before.
3. Every entry records its gate and what it measured.

## Kept: Hyper (performance)

| Change | Files | Gate | Measured (dev laptop) |
|---|---|---|---|
| Presets framework | `src/gpu/hyper.{h,cpp}`, `src/sysinfo.cpp` (log line) | `REGPU_HYPER=mild\|low\|extreme` (or `FM2_HYPER`) | n/a |
| Cut 2: whole-frame render scale down to 25%, FSR 1 forced | `src/settings.cpp` | `REGPU_HYPER` (Mild 50, Low 35, Extreme 25) or `REGPU_HYPER_SCENE=<25-100>` | Extreme: ~93 fps vs ~76-82 (+15%) |
| Cheap final upscale: one bilinear pass with the display gamma, in place of FSR 1's EASU and RCAS plus the gamma pass (found with RGP: those three full-frame passes cost ~1.8 ms a frame) | `src/gpu/fsr1.hlsl` (`HyperUpscaleMain`), `CMakeLists.txt` (entry point), `src/gpu/renderer.cpp` (`Fsr1Present`, `GammaPoints`, `GradeFrontBuffer`) | `REGPU_HYPER_UPSCALE=0\|1`; on in Low and Extreme | Parked, two pairs: 74.9 to 84.9 and 78.4 to 88.5 fps (+13%); GPU ~1.5 ms less a frame; slightly softer picture, same brightness |
| Settings for every cut, so each can scale with the hardware: `HyperSettings` / `HyperConfigure` (game-neutral; in ReGPU this belongs in `regpu::Config`), filled by FM2 from `settings.ini` keys `hyper=`, `hyper_scene=`, `hyper_upscale=`. Order: environment, then settings, then the preset | `src/gpu/hyper.{h,cpp}`, `src/settings.cpp` | the keys (absent: unchanged) | Checked: `hyper=low` plus `hyper_upscale=0` gives Low at 35% with FSR 1 |
| Mesh decimation: big triangle-list meshes drawn from simplified index buffers (same vertices, fewer triangles). Positions read from guest memory through the vertex shader's first fetch (`TranslatedShader::positionFetch`, recorded at translation); meshoptimizer `meshopt_simplify` with locked borders on a worker thread; cached by index address, size and content; the original indices until a mesh is ready. Game-neutral | `thirdparty/meshoptimizer` (v0.22, MIT, `simplifier.cpp` + `allocator.cpp` in `CMakeLists.txt`), `src/gpu/hlsl.{h,cpp}`, `src/gpu/renderer.cpp` (`hyperdec`), `src/gpu/hyper.{h,cpp}`, `src/settings.cpp` (`hyper_decimate=`). Patch: `experiments/decimate_patch.py` | `REGPU_HYPER_DECIMATE=<10-90>` (% kept); 25 in Extreme only. Tuning: `REGPU_HYPER_DECIMATE_MIN` (300 indices), `REGPU_HYPER_DECIMATE_ERROR` (0.2) | Extreme, 3 pairs: 97.7 to 101.4 fps and 98.0 to 101.9 (+4%); 1% lows mixed. Native 720p: no fps gain (63.6 vs 65.3 at 50%; 64.3 at 25%) although triangles -40%, vertex shader runs -25%, GPU -1 ms: this Vega is pixel-bound at 720p. Picture indistinguishable at Extreme. **Rerun on vertex-weak GPUs (Intel iGPU)** |
| **[PROVEN]** Anisotropy cap: the wrapping sampler's anisotropic level capped by the preset | `src/gpu/renderer.cpp` (root signature samplers), `src/gpu/hyper.{h,cpp}`, `src/settings.cpp` (`hyper_aniso=`). Patch: `experiments/aniso_patch.py` | `REGPU_HYPER_ANISO=<0\|2\|4\|8\|16>`; Mild 4, Low/Extreme 2 | Parked GPU ms by level: 0: 20.88, 2: 20.9, 4: 21.2, 8: 22.1, 16: 22.8. Cap 2 vs default 16: GPU 22.57 to 20.94 ms (-7%), 72.7 to 74.9 fps. Distant ground slightly blurrier |
| **[PROVEN]** Helper-surface scale: render targets narrower than half the screen (reflection views, blur, bloom) drawn smaller; the screen stays native | `src/gpu/renderer.cpp` (`PassScale`), `src/gpu/hyper.{h,cpp}`, `src/settings.cpp` (`hyper_helpers=`). Patch: `experiments/helperscale_patch.py` | `REGPU_HYPER_HELPERS=<25-100>`; Mild 75, Low/Extreme 50 | 50% vs 100%, 3 pairs: GPU 21.02/22.89 to 18.74/19.61 ms (-2.3 to -3.3 ms); fps +1-3%. Car reflections slightly softer. 25% vs 50%: GPU -1.2 ms (inside noise), reflections blurrier: **[RETEST ON INTEL]** as the potato level |
| **[PROVEN, borderline; RETEST ON INTEL]** Lossless: resolves whose guest destination is the 32-bit k_2_10_10_10 are stored as `R10G10B10A2_UNORM` instead of RGBA16F (the two full-screen HDR copies each frame): half the copy and read bandwidth; the Xbox 360 stores the same bits | `src/gpu/renderer.cpp` (`colorBlit1010`, `Resolve` format choice, `EnsureResolved` UAV flag), `src/gpu/hyper.{h,cpp}`, `src/settings.cpp` (`hyper_resolve1010=`). Patch: `experiments/resolve1010_patch.py` | `REGPU_HYPER_RESOLVE1010=0\|1`; on in every preset (not with MSAA) | 4 pairs, GPU always lower: -2.74, -0.35, -0.73, -0.28 ms (typical ~-0.5); race fps 47.3/51.3/50.7/51.3 to 52.7/53.3/53.3/50.0. Picture identical. Bandwidth cut: expect more on DDR4 iGPUs |
| Presets combined (6 Nov) | | `REGPU_HYPER=low` / `extreme` | Parked: Low 93.0 fps (+21%), Extreme 96.3 fps (+25%) vs ~77 |

## Kept: tooling (off unless asked for)

| Change | Files | Gate | Purpose |
|---|---|---|---|
| FPS meter in the overlay panel, game-neutral | `src/gpu/fps_meter.{h,cpp}`, `src/gpu/renderer.cpp` | `REGPU_FPS_OVERLAY=1` (or `FM2_FPS_OVERLAY`); reset with F10 or `REGPU_FPS_RESET` / `FM2_FPS_RESET` | Current, average and 1% low fps |
| Auto-drive (MVP, steers from a frame readback) | `src/hooks/autodrive.{h,cpp}`, `src/gpu/renderer.cpp`, `src/kernel/imports_xam.cpp` | `FM2_AUTODRIVE=1` | Hands-off races (oval only) |
| `--racenow[=payload]`: boot straight into a race | `src/main.cpp`, `src/kernel/zip_overlay.cpp` | the command-line flag | Debugging; no menus, grid or countdown |
| Tour step `SWITCH:<name>` | `src/kernel/imports_xam.cpp` | only inside `FM2_TOUR` routes | Turns a live switch on at a point in a route |
| Benchmark script | `tools/hyper_bench.sh` | run by hand | One race, uncapped; appends to `out/bench.txt` |

## Kept: diagnostics (off unless asked for; remove or move to ReGPU's debug tooling before upstreaming)

| Change | Files | Gate |
|---|---|---|
| Pipeline statistics per frame (vertices, triangles, pixel shader runs) | `src/gpu/renderer.cpp` | `FM2_GPU_STATS=1` / `REGPU_GPU_STATS=1` |
| Pass labels with phase and surface width; resolve split into blit, tail and clears; scene/post totals | `src/gpu/renderer.cpp` | `FM2_GPU_PASSES=1` |
| Isolation switches at draw submission (break the picture on purpose) | `src/gpu/renderer.cpp` | `REGPU_MEASURE_NO_PIXELS`, `REGPU_MEASURE_HALF_VERTS`, `REGPU_MEASURE_EMPTY_DRAWS` |

| Alpha test stripped from every pixel shader (ceiling) | `src/gpu/hlsl.cpp` | `REGPU_MEASURE_NO_ALPHATEST=1` |
| Skip every draw into surfaces of given widths (pass cost ceiling) | `src/gpu/renderer.cpp` (`DrawImpl`) | `REGPU_MEASURE_SKIP_PITCH=560,280,320` |
| Bilinear wrapping sampler (no mip blend, no aniso) | `src/gpu/renderer.cpp` (samplers) | `REGPU_MEASURE_BILINEAR=1` |
| Resolve draw count after clears | `src/gpu/renderer.cpp` (`Resolve`, pass mark "rsv draws after clear") | `FM2_GPU_PASSES=1` |
| Pipeline section split: frame start (GPU fence wait), surfaces, pipeline lookup | `src/gpu/renderer.cpp` (`g_pipeSplit`) | `FM2_PERF=1` |

| Indexed draw census: draws and indices per frame by primitive type and size (>= 300 indices) | `src/gpu/renderer.cpp` (`DrawImpl`, after `hyperdec::Apply`) | `FM2_PERF=1` (every 600 frames) |
| PS texture fetch census (main-view indexed colour draws by fetch count) and the fetch ceiling switch | `src/gpu/hlsl.{h,cpp}` (`TranslatedShader::textureFetches`), `src/gpu/renderer.cpp` | `FM2_PERF=1`; `REGPU_MEASURE_TFETCH_MAX=<n>` |
| Blend census: blended main-view draws per frame by blend control, depth control and colour mask | `src/gpu/renderer.cpp` (`DrawImpl`, before `Section(3)`) | `FM2_PERF=1` |
| Heavy depth-writing blended PS census (FM2: the car shaders) with fetch count and HLSL size; dump one shader | `src/gpu/renderer.cpp` (blend census block) | `FM2_PERF=1`; `REGPU_MEASURE_DUMP_PS=<hash>` writes `out/ps_<hash>.hlsl` |
| Resolve census: resolves per frame by size, rectangle, host and guest format (`format` printed as host DXGI*100 + guest format) | `src/gpu/renderer.cpp` (`BlitResolve`, `g_censusDestFormat`) | `FM2_PERF=1` (every 600 frames) |
| Skip the copy of resolves at least N game pixels wide (ceiling) | `src/gpu/renderer.cpp` (`BlitResolve`) | `REGPU_MEASURE_SKIP_RESOLVE=<width>` |

| Skip blended (1) or opaque (2) draws (overdraw split, ceiling) | `src/gpu/renderer.cpp` (`DrawImpl`) | `REGPU_MEASURE_SKIP_BLENDED=1\|2\|3\|4` (3: true transparents only; 4: blended draws that write depth, i.e. FM2's cars) |

## Parked experiments: every one, to redo on other hardware

Removed from the code because they gave nothing on **this** machine (Ryzen 3 5300U, Vega iGPU). Another GPU or CPU
can be limited by something else, so each is kept recreatable: run them all again on the Intel iGPU box. The patch
scripts are in `experiments/` (Python; they apply to the FM2 tree as of 6 Nov 2026, run as
`py -3 experiments/<file> Forza-Motorsport-2-ReGPU`); without a script, the description says what to change.

| Experiment | What it stresses less | Result here | Redo with | Date |
|---|---|---|---|---|
| Half-precision vertex outputs (`min16float4` interpolants) | interpolator / vertex output bandwidth | GPU 15.50 vs 15.26 ms (none); visible road banding | In the generated vertex/pixel shader HLSL, declare the interpolants `min16float4` | 5 Nov |
| Optimised shader compile (`D3DCOMPILE_OPTIMIZATION_LEVEL3`) | shader ALU | GPU 15.31 vs 15.26 ms (none) | Shader compile flags in `renderer.cpp` | 5 Nov |
| Whole-surface clears after resolves | memory compression / clears | 80.5 vs 81.7 fps (none) | Extend each resolve's clear to the whole surface | 6 Nov |
| Parallel draw translation (`FM2_DRAW_WORKERS=3`, existing switch) | CPU (command processor) | 67.2 vs 69-71 fps (worse, 4 cores here) | Just the switch; try on a CPU with more cores | 6 Nov |
| Gamma points from the constant buffer in the Hyper upscale | shader ALU | GPU 13.95/14.23 vs 14.07/13.95 ms (none) | `experiments/hyper_gamma_patch.py` | 6 Nov |
| 8-bit colour surfaces (every surface 8-bit, ceiling test) | colour bandwidth | GPU 15.23-15.31 vs 15.11-15.17 ms (none) | Host colour surfaces made `R8G8B8A8_UNORM` instead of 16-bit float | 6 Nov |
| Mip drop (`REGPU_HYPER_MIPDROP=1-3`): largest mip levels left out at upload | texture memory and bandwidth (VRAM-starved GPUs!) | native 70.6 vs 70.4 / 69.1 fps (none); blurrier | `experiments/mipdrop_patch.py` (revert: `mipdrop_revert.py`) | 6 Nov |
| Half-rate helper passes (`REGPU_HYPER_HALFRATE`): flat helper passes (blur, bloom; narrower than half the screen, no depth) drawn every other frame, their saved images replayed in between | GPU, post-processing | native 67.05 vs 66.0 fps (none); Extreme 95.45 vs 99.7 (noise). The first version (+7.5%) also skipped 3D reflection passes and broke them | `experiments/halfrate_patch.py`, then `replay_patch.py`, then `pass_patch.py`; final rules (rect-only snapshots, passes that clear depth never skipped, a pass ends on surface or frame change) are described here and must be redone by hand | 6 Nov |
| **[RETEST ON INTEL]** No-discard pixel shaders (`REGPU_HYPER_NODISCARD`): draws without the alpha test use a PS variant with the alpha switch removed, so early depth is kept | early-Z / depth bandwidth | First run (7 Nov) was **invalid**: the pipeline key names shaders by guest hash, so the cached pipeline with the discard was reused. Redone with its own key (flags bit 25): PS runs 8.60M to 8.30M (-3.5%), GPU 18.75/18.80 vs 18.70/19.15 ms (none) | `experiments/nodiscard_patch.py`, then `experiments/nodiscard_v2_renderer.diff` (the key fix) | 7 Nov |
| **[RETEST ON INTEL]** `FM2_NO_DEPTH_ALPHATEST` (existing switch) | early-Z in depth passes | none | just the switch | 7 Nov |
| **[RETEST ON INTEL]** Bilinear wrapping sampler (`REGPU_MEASURE_BILINEAR=1`), on top of aniso 2 | texture bandwidth / filtering | GPU 19.46/18.86 vs 18.75/19.29 ms (none) | the switch | 7 Nov |
| **[RETEST ON INTEL]** Helper scale 25% (vs 50%) | offscreen fill + bandwidth | GPU 21.70/19.45 vs 20.27/18.49 ms (-1.2, noise) | `REGPU_HYPER_HELPERS=25` | 7 Nov |
| **[REJECTED here; retest on Intel only if its GPU is pixel-starved]** Depth prepass (`REGPU_HYPER_PREPASS`): the command recorder shadows graphics state and holds a pass's commands; each opaque depth-writing draw (stencil allowed when it never rejects: FM2 tags 1367 draws a frame with stencil ALWAYS/REPLACE) also gets a depth-only copy (no PS, no colour) in a side stream emitted before the held commands at the next hazard; the draw itself tests <= without depth write, with its no-discard PS variant | opaque overdraw | 844 draws a frame prepassed, 7 flushes; picture correct. Whole frame PS runs 8.60M to 8.07M (-6%), GPU +1 to +2 ms. Opaque-only main view: PS 5.03M to 4.51M (-10%), GPU 10.15 to 11.41 ms (worse). FM2's opaque scene has little overdraw; the 8.8x comes from screen passes, blended and alpha-to-mask draws. Costs ~0.5 ms CPU too (bad for the i3) | `experiments/prepass/` (diffs against the pre-prepass sources, and the hyper/settings files with the setting) | 7 Nov |
| **[RETEST ON INTEL]** Looser decimation limits on the native potato stack (`REGPU_HYPER_DECIMATE_ERROR=0.6`, `REGPU_HYPER_DECIMATE_MIN=100`, vs 0.2 / 300) | vertex work | triangles 342k to 339k (-1%); GPU 17.89/17.96 vs 17.79/17.65 ms (noise). The limits are not what holds decimation back: 179 of 237 meshes come back "kept" (the simplifier cannot reach the target with locked borders), and cars are many small bordered pieces. Next lever would be unlocked borders (risk: cracks at seams) | the switches | 7 Nov |
| **[REJECTED]** Decimation with position welding (`REGPU_HYPER_DECIMATE_WELD=1`: `meshopt_generateShadowIndexBuffer` on positions, then simplify) | vertex work | Worse: triangles 342k to 400k, meshes simplified 237 to 123; GPU 17.53/17.60 vs 20.65/17.74 ms. Merging by position makes the simplifier give up on more meshes | `experiments/renderer.cpp.pre-weld` + add `indexgenerator.cpp` to CMake and the weld block (described here) | 7 Nov |
| **[REJECTED: breaks the rules]** "PS2 cars": border-ignoring sloppy decimation (`meshopt_simplifySloppy`) with `REGPU_HYPER_DECIMATE_MIN=3` | vertex work | Triangles 464k to 244-268k, GPU ~19.2-19.4 ms (single runs, aniso 2). The car body barely changes shape (its panels are already simple), but the sky dome disappears (black sky), the rear-window reflection goes black and the HUD gauges get holes: effects removed. Cars are hundreds of tiny pieces (12-30 triangles each, every edge a border), so per-draw decimation cannot make them low-poly; that would need the game's own LOD, a per-game hint | `experiments/decimate_sloppy.diff`; picture `Forza-Motorsport-2-ReGPU/out/sloppy_compare.png` | 7 Nov |
| **[RETEST ON INTEL]** Heavy-shader mip bias (`REGPU_HYPER_HEAVY_LOD=2`): pixel shaders with at least 5 texture fetches (`REGPU_HYPER_HEAVY_FETCHES`) get +2 LOD bias on every fetch (generic; in FM2 these are mostly the car shaders, 5-9 fetches, 2-3x the HLSL of an average shader) | texture bandwidth of the heaviest shaders | GPU 18.84/19.01 vs 18.24/18.76 ms (~-0.4 ms); car reflections slightly softer, road texture visibly changed (its shader has 5+ fetches too). Quarter-size textures barely help: these shaders are ALU-bound here, not bandwidth-bound | `experiments/heavy_lod_block.cpp` (insert before the first `cs.cacheKey = Fnv(...)` in `GetShader`) | 7 Nov |
| **[REJECTED]** Lossless fast fetch (`REGPU_HYPER_FASTFETCH=1`): a 2D fetch with no special case (mirror, border, single-axis clamp, texel offset, denormalised coordinates, sub-image scale, explicit LOD; v2 also handles the filter override inline) samples directly instead of running the whole `TFetchRaw` helper | ALU of the generic fetch helper | v1: GPU 18.69/18.67 vs 18.18/18.98 ms; v2: 18.82/19.20 vs 18.73/18.65 ms (~-0.3 ms, noise). The helper's checks are not the cost | `experiments/fastfetch_block.cpp` (insert before the first `cs.cacheKey = Fnv(...)` in `GetShader`) | 7 Nov |
| **[NO EFFECT in FM2; keep for other games]** Flat reflections (`REGPU_HYPER_FLATREFLECT=1`): every cube-map fetch samples its smallest mip (the cube's average colour) | reflection sampling | GPU 18.47/19.40 vs 18.43/19.58 ms, picture unchanged: FM2 does not use cube maps for its reflections (they come from the 280-wide offscreen passes, sampled as 2D). Generic and cheap; may matter in games that do use cube maps | `experiments/flatreflect_block.cpp` (insert before the first `cs.cacheKey = Fnv(...)` in `GetShader`) | 7 Nov |
| **[REJECTED as built; idea parked]** Unlit mode (`REGPU_HYPER_UNLIT=<n>`): pixel shaders with at least n texture fetches output their FIRST texture sample's colour (alpha kept), so the driver drops the lighting math | heavy pixel shader ALU | n=5: GPU 19.20/20.49 vs 18.60/18.68 ms (~-1.2 ms); n=3: 19.85 ms (none). **Picture broken**: the first fetch is rarely the base colour (road white, magenta areas, car shows a grey dirt/occlusion map). A real unlit mode needs dataflow analysis of the microcode to find the base-colour fetch and the vertex colour; possible in the translator but a project of its own | `experiments/unlit_block.cpp`; picture `Forza-Motorsport-2-ReGPU/out/unlit_compare.png` | 7 Nov |
| **[PROVEN, borderline; potato tier; RETEST ON INTEL]** Frozen 3D helper passes (`REGPU_HYPER_FREEZE=<frames>`, in the build, off unless set): an offscreen colour surface narrower than half the screen that draws with depth testing (a 3D helper view) is drawn and resolved for that many frames, then its draws and resolves are skipped; the textures it fed keep their last content. Depth-only passes (shadow maps) are excluded: v1 froze them too and car shadows stuck in place | reflection / lighting views | v2, parked, 2 pairs: GPU 19.03/18.65 vs 18.05/17.92 ms (~-0.85 ms, -5%), race fps 54.0/54.0 to 56.0/56.7. Driving (1 run each, different moments): 19.21 vs 17.76 ms. In FM2 it freezes the 280-wide car reflection/lighting view; car paint and windows keep the start-line reflection while driving (plausible, not live). v1 (shadow maps frozen too): 18.82 vs 17.30/17.11 ms but wrong shadows | in the build: `g_freeze`, `FreezeAfter`, `FrozenSurface` in `renderer.cpp` (`Resolve` and `DrawImpl`); diff `experiments/freeze_helpers.diff`; pictures `Forza-Motorsport-2-ReGPU/out/freeze_compare.png` (v1), `freeze2_compare.png` (v2, parked and driving) | 7 Nov |
| **[PROVEN, the best native cut since the helper scale]** Draw culling by pixel count (`REGPU_HYPER_CULL_PIXELS=<n>`, in the build, off unless set): every main-view depth-tested colour draw gets an occlusion query; pixels are summed per mesh (shaders + index buffer + counts) over the frame; a mesh that produced n pixels or fewer is skipped, except every `REGPU_HYPER_CULL_RETEST` (4)-th frame when it is drawn and measured again. n=0 culls only hidden draws (occlusion culling), larger n also tiny ones. `REGPU_HYPER_CULL_ALL=1` makes every depth-tested draw eligible (default: opaque depth-writing only). Generic | pixel + vertex + submit work of hidden / tiny draws | Final (all draws eligible), 2 pairs vs 19.07/19.10 ms, race fps 53.3/52.7: n=0: 17.45/17.12 ms (-1.8 ms), 58.7/59.3 fps (+11%); **n=64: 16.86/16.10 ms (-2.6 ms, -14%), 61.3/62.7 fps (+17%)**; 37-40% of draws skipped. Driving n=64: 19.11 vs 16.93 ms, 50.0 vs 60.0 fps (+20%). Pictures correct parked and driving. History: per-query resolves cost +23 ms (fixed: one batched resolve per frame); resolving in the next frame read stale counts and culled the sky, glass, road and HUD parts (fixed: resolve before the frame is submitted); keying by draw order was fragile (fixed: sum per mesh) | in the build: `drawcull` namespace, the query block before the draw call in `DrawImpl`, the resolve before `Close()` in the swap; diff `experiments/batch6_cull_freeze_a2m.diff`; pictures `out/cull7_compare.png` (final), `cull3..6_compare.png` (the broken stages) | 7 Nov |
| **[PROVEN, borderline; lossless look]** Alpha-to-coverage as alpha test (`REGPU_HYPER_A2M_TEST=1`, in the build, off unless set): alpha-to-mask draws (crowds, fences, foliage) drop alpha-to-coverage and use alpha >= 0.5 (what one-sample coverage keeps anyway) | coverage-mask cost | 2 pairs: 19.25/18.79 vs 18.56/18.53 ms (~-0.5 ms); crowd and fences identical. Intel GPUs are often weak at coverage masks: retest there | in the build (`MakePipelineKey`, `DrawImpl` pinfo); diff above | 7 Nov |
| Frozen helpers refreshed every n-th frame (`REGPU_HYPER_FREEZE_REFRESH=3`, with `REGPU_HYPER_FREEZE=120`) | reflection views | 2 pairs: 19.25/18.79 vs 18.30/18.47 ms (~-0.6 ms); reflections update at a third of the rate (live-ish) | in the build | 7 Nov |
| **[REJECTED as built]** Half-rate shadows: `REGPU_HYPER_FREEZE_SHADOWS=1` (depth-only helper passes frozen too, refreshed every 3rd frame) | shadow map passes | 2 pairs: 18.20/17.73 ms (~-1.05 ms) but the large track-side shadow band disappears (the cascades seem to alternate frames, so a fixed refresh lands on the wrong ones) | in the build (off) | 7 Nov |
| Not built this batch: half-size post-processing (#4: the full-width post passes write the final image, so shrinking them is a resolution cut; needs a pass-by-pass analysis) and constant-upload dedup (#6: GPU-bound laptop cannot show a CPU gain; the float banks are written straight into the ring with a byte swap, so dedup means comparing ~4 KB of guest registers per draw). Both left for the Intel box if it turns out CPU-bound | | | | 7 Nov |

## Measurements (ceilings: what a whole class of cut could give at most)

| Switch (breaks the picture) | Native 720p here | Meaning |
|---|---|---|
| `REGPU_MEASURE_HALF_VERTS=1` (half the triangles of every draw) | 87.5 vs 62.0 fps (+41%), 2 runs each | Misleading as a geometry ceiling: the holes also cut pixel shader runs 29% (10.9M to 7.8M a frame); that, not vertex work, is the gain. Real decimation keeps the pixels |
| GPU freed (`REGPU_MEASURE_EMPTY_DRAWS=1`) | ~122 fps | The CPU ceiling with this renderer |
| `REGPU_MEASURE_NO_ALPHATEST=1` | 75.8 to 81.2 fps, GPU 21.17 to 20.55 ms | Alpha-tested draws carry the cost; a lossless no-discard variant for the rest gained nothing |
| `REGPU_MEASURE_SKIP_PITCH=560` / `560,280,320` | GPU 21.42 to 18.36 / 15.36 ms; 75.0 to 81.4 / 87.8 fps | Offscreen helper passes cost up to 6 ms; the helper scale recovers ~half |
| Pass breakdown (`FM2_GPU_PASSES=1`) | ~19.9 ms: main scene 9.0 ms (1437 draws), 560-wide post 2.15 ms, full-width resolves ~1.2 ms each, all clears ~0.4 ms | Main scene shading is the biggest remaining target |
| CPU split (`FM2_PERF=1`, 7 Nov) | renderer 15.7 ms/frame, of which 7.5 ms is the frame-start GPU fence wait; real CPU ~8 ms (constants 3.5, submit 1.3, textures 1.24, indices 0.45, shaders 0.4, pipeline lookup 0.24) | This box is GPU-bound. On an i3-7100 the CPU part may grow ~1.5x (~12 ms), so constants are the first CPU cut to retest there |
| `REGPU_MEASURE_SKIP_RESOLVE=1000` (full-screen resolve copies left out) | GPU 19.29/19.12 to 17.90/17.67 ms; race fps 48.8/49.2 to 53.1/53.7 (+9%) | Per frame: 3 full screens resolved (two HDR copies as guest 10:10:10:2, one 8888), in 208- and 512-row bands. Led to the 10:10:10:2 cut |
| Overdraw (7 Nov; offscreen passes skipped with `REGPU_MEASURE_SKIP_PITCH`, `FM2_GPU_STATS=1`) | Whole frame 9.34 PS runs per output pixel; main view (1280-wide surface, which the pass label printed as w640) 8.77; opaque draws only 5.45 (GPU 9.6 ms); blended only 3.42 (6.4 ms). Without the alpha test: 8.03 (-8%) | Discard/early-Z is not the cause. Opaque overdraw ~3-4x after the full-screen post quads: a depth prepass is the main remaining lever (est. -1.5 to -2.5 ms net); blended overdraw (smoke, glass, particles) could only shrink with smaller transparent targets |
| Main-view colour draws by prepass eligibility (7 Nov, prepass build) | per frame: 844 opaque depth-writing (stencil ALWAYS/REPLACE 1367 of the depth-writing ones), blended 603, alpha-to-mask 260, alpha test 109, no depth write or other stencil 148 | Alpha-to-mask (foliage, crowds, fences) is a bigger opaque-ish class than the alpha test |
| Native potato vs native untouched (7 Nov, 2 pairs, 720p native, settings anisotropic 16x): `REGPU_HYPER_ANISO=0 REGPU_HYPER_HELPERS=25 REGPU_HYPER_RESOLVE1010=1 REGPU_HYPER_DECIMATE=10` | GPU 23.28/23.82 to 17.69/17.58 ms (-25%); race fps 41.3/42.7 to 56.0/56.7 (+34%); triangles 464k to 310k drawn; PS runs 10.25M to 8.16M (-20%) | The most aggressive native-resolution stack of kept switches. Reflections and far ground visibly softer; HUD and screen unchanged. Picture: `Forza-Motorsport-2-ReGPU/out/native_vs_potato.png` |
| Pixel-side ceilings (7 Nov, 2 runs each vs the native best: aniso 2, helpers 50, 10:10:10:2; base GPU 18.90/18.26 ms, race fps 50.7/52.7, 8.61M PS runs) | Blended draws skipped (`REGPU_MEASURE_SKIP_BLENDED=1`): GPU 12.99/12.86 ms (-5.7 ms), race fps 75.3/76.0 (+46%), PS runs 5.58M. Half-shading proxy (`REGPU_HYPER_SCENE=71` + cheap upscale; the run actually used the settings' 50% scale, so it is a quarter-shading proxy): GPU 13.98/13.74 ms (-4.7 ms), PS runs 2.22M. Only the first texture fetch of each PS kept (`REGPU_MEASURE_TFETCH_MAX=1`): 16.03/16.11 ms (-2.5 ms); first two kept (`=2`): 16.24/16.95 ms (-2.0 ms) | Transparent effects are the largest pixel-side lever left (smaller targets could recover a good share of 5.7 ms). Checkerboard's real ceiling is below the quarter-shading proxy, roughly -2.5 to -3 ms. Texture taps beyond the second cost ~2 ms in all; trimming heavy shaders realistically recovers a fraction. Fetch census, main-view indexed colour draws per frame by PS texture fetches: 0:49 1:480 2:347 3:246 4:98 5:21 6:13 7:13 8+:39 |
| Transparent-effects check (7 Nov), before building a half-size transparent layer | Blend census (main view, per frame): 530 alpha-blended draws that test AND write depth with stencil (blend 07060706, depth 67), ~100 alpha-blended screen overlays without depth, ~11-15 true transparent effects (depth test, no write), ~5 premultiplied overlays. Skipping only the true transparents (`REGPU_MEASURE_SKIP_BLENDED=3`): GPU 18.63/18.30 vs 18.34/18.46 ms (none), PS runs -2%. Skipping the 530 depth-writing blended draws (`=4`): GPU 15.12 ms (-3.3 ms) and **every car disappears**: FM2 draws its cars with alpha blending on | The half-size transparent layer is **not built**: nothing to gain in this scene (maybe in smoke-heavy driving; retest there). The earlier 5.7 ms "blended" ceiling was mostly the cars. **Car shading costs ~3.3 ms of ~18.5**: the next pixel target (their shaders: sample count, reflection work). Picture: `Forza-Motorsport-2-ReGPU/out/blended_solid.png` |
| `REGPU_MEASURE_FAST_TFETCH=1` (every 2D fetch a plain sample: no mirror, border, axis clamp, texel offset or filter override; can break textures) | GPU 19.50/19.50 to 17.49/17.91 ms (-1.6 to -2.0 ms), race fps 50.7/52.7 to 54.0/55.3; picture near identical in the parked scene | Not the helper's overhead (the lossless fast fetch gained nothing): the gain is from what it drops, most likely texel-offset taps (soft-shadow / blur filters collapsing onto one texel). Candidate cut: fewer offset taps in heavy filters |
| `REGPU_MEASURE_NO_TEXEL_OFFSET=1` (every fetch's texel offset zeroed: multi-tap filters collapse onto one texel) | GPU 18.64/18.75 vs 18.43/18.93 ms (none) | Offset taps are not where the stripped-fetch ceiling's 2 ms comes from. Remaining suspect: the stripped fetch ignored the per-instruction filter override, sampling some "linear" fetches with the fetch constant's point filter (a quality cut, not overhead). Not pursued further on this machine |
| `REGPU_MEASURE_SKIP_PITCH=280` with helpers at 50% (7 Nov, 2 pairs) | GPU 19.68/18.89 to 18.55/17.98 ms (~-1.0 ms), race fps 52.0/53.3 to 55.3/56.7 (+6%) | The 280-wide passes are not just reflections: without them the cars render black (they feed the car lighting/paint). Freezing them (render once, reuse) could recover up to ~1 ms with static car lighting. Picture: `Forza-Motorsport-2-ReGPU/out/reflskip_compare.png` |

## Not features (don't upstream)

| Change | Why |
|---|---|
| `src/kernel/stubs.cpp` | Regenerated by `tools/recomp.sh` |
| `tools/vcenv.cmd` | Local path fallback to VS 2022 Community |
| `thirdparty/XenonRecomp` | The repo's own patch applied (`patches/xenonrecomp-fm2.patch`) |

## Lean texture fetch (8 Nov 2026), [PROVEN], lossless

**Lead:** `REGPU_MEASURE_FAST_TFETCH` had shown ~2 ms. Split with the new `REGPU_MEASURE_TFETCH_SKIP=<mask>` (1 mirror, 2 border,
4 axis clamp, 8 resolve scale, 16 filter override, 32 forced-LOD branch, 64 exponent+swizzle). Single bits were lost in thermal
noise. All bits (127) vs base, 3 interleaved pairs: GPU 18.08 vs 19.8–20.4 ms, race fps 55.6 vs 51.8 (+7%). That is the ceiling,
and it breaks some textures.

**Built: `REGPU_HYPER_LEAN_FETCH=1`.** Per draw, after the textures are set up, if every 2D slot of the pixel shader has no
mirror/border/axis-clamp bits, no exponent adjust, an identity or absent swizzle and resolve scale 1, the draw uses a variant of
the pixel shader (alias hash `LeanFetchAlias`, kept out of the manifest) whose fetch helper drops those checks (mask 111; the
filter override stays). Main view only (`edramMode 4 && WidePass()`), only after `GameSettled()`.
- Pitfalls: queued at background priority, the variants never compiled (0 lean draws). At normal priority from the first frame,
  the compile burst stalled the loading screen forever. Main view + settled fixed both.
- Census: 52% of textured main-view draws run lean. Blockers per slot: mode bits 1.52M, swizzle 0.89M, exponent 56k, resolve
  scale 27k (per 2.2M draws).
- Result, 3 interleaved pairs (aniso 2, helpers 50, 1010): GPU 20.93/19.66/19.76 → 19.43/19.37/19.18 ms (**−0.8 ms**), race fps
  47.3/50.7/48.7 → 51.3/52.0/52.0 (**+6%**). Picture identical (`out/leanfetch_compare.png`, 1% pixels differ: timer, crowd).
- **Room left (~1.2 ms to the ceiling):** bake each slot's swizzle and mode bits into the variant key, so the other 48% of
  draws get a specialised fetch too; and bake the filter override.

### Lean fetch v2: baked texture state (`REGPU_HYPER_LEAN_FETCH=2`), [PROVEN], lossless

Each draw's used slots (tinfo.x bits 8-23, the whole tinfo.w, and whether the resolve scale applies) are hashed into a variant
(`BakedFetchAlias`, at most 24 per shader) whose helper reads `static const` arrays instead of the constants, so the compiler folds
every check. 100% of textured main-view draws run baked (97% from the first seconds), only 85 variants. Picture identical
(`out/bakedfetch_compare.png`).
3 interleaved rounds (aniso 2, helpers 50, 1010), GPU ms / race fps:
| | 1 | 2 | 3 | mean |
|---|---|---|---|---|
| base | 20.81 / 47.3 | 19.33 / 52.7 | 19.58 / 50.0 | 19.91 / 50.0 |
| v1 (plain draws only) | 20.26 / 50.0 | 19.11 / 50.7 | 19.12 / 51.3 | 19.50 / 50.7 |
| v2 (baked) | 18.62 / 53.3 | 18.26 / 52.7 | 20.16 / 49.3 | 19.01 / 51.8 |
v2: **−0.9 ms, +4% fps** (round 3 looks like a thermal dip). v2 replaces v1. Still ~1 ms short of the skip-127 ceiling: the remaining
cost is in parts kept on purpose (filter override, texel offsets, sampler selection by runtime mode). Retest on Intel.

## Next-jump ceilings (8 Nov 2026), on top of lean fetch v2

| Switch | 2 valid pairs, GPU ms | Picture | Verdict |
|---|---|---|---|
| `REGPU_MEASURE_OPAQUE_ZWRITE=1` (blended depth-writing draws, i.e. cars, drawn opaque) | 18.02 → 18.40 (**+0.4, worse**) | – | **Rejected**: blending isn't what costs on the cars |
| `REGPU_MEASURE_FAST_MUL=1` (Mul9 → plain multiply everywhere) | 18.02 → 17.0 (−1.0) | **HUD race timer digits garbled** (the HUD relies on 0×anything = 0) | Ceiling only |

Bench: `tools/hyper_bench.sh` now reruns once a run stuck on the loading screen (race fps > 250; one base run started 22 s late
and never left the loading screen, no baked variants involved: the known start-up flake).

## Lean fetch v3: fast math on 3D draws (`REGPU_HYPER_LEAN_FETCH=3`), [PROVEN]

The baked-fetch variant also replaces Mul9 with a plain multiply, except for screen passes (`g_screenPassDraw`: rect lists and
draws of ≤6 vertices, which covers HUD quads). Main view only, after `GameSettled()`.
3 interleaved pairs vs v2 (aniso 2, helpers 50, 1010): GPU 20.36/19.49/20.44 → 18.90/17.90/18.54 ms (**−1.65 ms**), race fps
51.3/50.0/50.0 → 54.0/53.3/54.7 (**+7%**). Won every pair. Picture: 0.8% pixels differ (timer, crowd); timer digits clean
(`out/fastmul3d_timer.png`, `out/fastmul3d_compare.png`).
Against the base without lean fetch (~19.9–20.1 ms, ~50 fps): **about −1.6 ms and +8%** for lean fetch v3 in all.
Risk: only this scene was checked. A 3D shader that switches a term off by multiplying an infinity or a NaN by 0 would show
it (sparkles, black or white pixels). Check other tracks, the menus' 3D car and photo mode before a preset uses it.

### v3 picture check on other tracks (8 Nov)
Parked, aniso 2, helpers 50, 1010; v2 vs v3 with a screenshot at 25 s (`BENCH_RACENOW="drive=0,race=N"`, N = 3, 7, 11, 16:
tracks 22 (city street circuit), 16 (Road Atlanta-style, low sun), 35 (Mugello), 8 (speedway infield)).
Pixels differing: 2.1%, 0.5%, 0.5%, 0.9%. All of it is timing: the shots land at different race times (AI cars further
down the road, scrolling banner, timer). **No sparkles, black/white pixels or wrong shading** on car paint, glass,
reflections, sky, crowd, road or HUD (`Forza-Motorsport-2-ReGPU/out/scene{3,7,11,16}_compare.png`).
One crash (sc16-v2, v2 not v3; the retry ran fine). fps in these runs is not comparable (no BENCH_PERF, one run each).
Still unchecked: the menus' 3D car, photo mode, replays, night/rain if any.

## [RETEST ON INTEL] Culling in helper and shadow passes (`REGPU_HYPER_CULL_PASSES=1`, 8 Nov)
Draw culling (see above) extended from the main view to every colour pass and to depth-only passes (shadow maps); the
mesh key also takes the surface info and pass type, so one mesh drawn in several passes is judged per pass. In the build,
off unless set (`cullPasses` in DrawImpl).
3 interleaved pairs on top of aniso 2, helpers 50, 1010, lean fetch 3, cull 64 + CULL_ALL: GPU 17.08/17.92/16.92 →
17.00/16.78/17.19 ms (**~−0.3 ms**, lost 1 pair), race fps 53.3/54.7/56.0 → 55.3/55.3/54.0 (+1%). Skipped draws 39.7% → 44.7%.
Picture identical (1.1% pixels differ: timer, crowd; `Forza-Motorsport-2-ReGPU/out/cullpass_compare.png`).
Below the bar here: the helper passes are already at 50% size, so their draws are cheap. Shadow maps may matter more on
the HD 630 (vertex-bound), hence the retest.
Lean fetch v3 on top of cull 64 + CULL_ALL (3 interleaved pairs, same session): GPU 19.29/18.36/18.44 → 17.39/17.81/16.88 ms
(**−1.3 ms**, won all 3), race fps 50.0/53.3/52.7 → 51.3/53.3/56.0 (+4%). The two stack; the laptop ran ~1 ms slower
than earlier in the day (thermal), so compare only within a session.

## [REJECTED here; RETEST ON INTEL] Half-precision pixel shaders (`REGPU_MEASURE_HALF=1`, 8 Nov)
Measurement switch in the build: pixel shader temporaries (r0..r63) are declared `min16float4` (FXC, ps_5_0 min
precision), so a driver with fast fp16 may run the maths at half precision. Texture coordinates live in the same
registers, so it is a ceiling, not a product.
Laptop (AMD Radeon integrated), 3 interleaved pairs after a warm-up run, on top of aniso 2, helpers 50, 1010, lean fetch 3,
cull 64 + CULL_ALL: GPU 16.16/16.83/16.67 → 17.37/16.69/18.00 ms (**~+0.8 ms, worse**), race fps 55.3/52.7/55.3 →
56.0/56.0/50.0. Picture identical at this resolution (0.9% pixels differ: timer, crowd; `Forza-Motorsport-2-ReGPU/out/half_compare.png`).
The AMD driver either ignores min precision on DXBC or pays conversions around it. Intel Gen9 drivers are known to
honour DXBC min precision (2x fp16 rate on the HD 630), so this one deserves a single Intel pair before it is dropped.
Run: `REGPU_MEASURE_HALF=1` added to the potato settings, against the same without it.

## [PROVEN] Potato preset (`REGPU_HYPER=potato`, settings `hyper=potato`, 9 Nov)
A fifth preset in `src/gpu/hyper.{h,cpp}`: native resolution (no scene scale, no cheap upscale) with the strongest
kept native cuts: aniso 0, helpers 25%, 10:10:10:2 resolves, decimate 10%, lean fetch 3, draw culling 64 pixels with
every depth-tested draw eligible. New accessors `HyperLeanFetch`, `HyperCullPixels`, `HyperCullAll` (renderer.cpp now
reads them instead of the raw environment; each still overridable: `REGPU_HYPER_CULL_ALL` now takes 0|1).
Left out: freeze helpers (borderline, static reflections), cull passes and half precision (retest on Intel).
2 interleaved pairs vs stock (720p native, parked): GPU 22.64/23.94 → 17.26/15.73 ms (**−29%**), race fps 37.3/41.3 →
53.3/58.7 (**+42%**). Log confirms every cut on; 99.9% of textured draws baked, ~40% of draws culled.
Picture: reflections softer, road texture less filtered at a distance; everything present
(`Forza-Motorsport-2-ReGPU/out/potato_preset_compare.png`).
Intel run: `REGPU_HYPER=potato` alone.

## Climb 2, step 0–1 (9 Nov): checks and where the potato frame goes
- Fast math (lean fetch 3) on the menu's 3D car (car select): clean; the baked counter rose on that screen
  (`Forza-Motorsport-2-ReGPU/out/menu_3.png`). Photo mode and replays still unchecked.
- Start-up line fixed: with no scene scale it says the upscale is "as set (Hyper adds none)", not "FSR 1".
- `tools/fm2ctl.py`: `FM2CTL_EXE=<full path>` starts another build (e.g. build\rel\fm2.exe) with `game`.
- Potato pass breakdown (`FM2_GPU_PASSES=1`, ~15.8 ms): main view 5.6–6.4 ms (1437 draws, logged as w640), post passes
  ~3.3 ms (w560 19 draws 1.7, w1280 39 draws 1.6), resolves ~1.8 ms, a full-screen blit 0.94 ms, w280 helpers ~1 ms.
  Still 8.8 pixel shader runs per output pixel. MSAA is 1x.
- **[REJECTED before building] Depth pre-pass.** Ceiling `REGPU_MEASURE_FLAT_MAIN=1` (measurement, in the build: baked
  main-view pixel shaders return flat grey): main view 5.68 → 4.44 ms. All main-view pixel shading costs ~1.2 ms, so a
  pre-pass could save less than that. (Pair 1 of that run read 24 ms: shader compile burst, discarded.)
- Main view split, 2 runs each: no rasterising (`REGPU_MEASURE_NO_PIXELS`) 2.8/4.4 ms, draws with no vertices
  (`REGPU_MEASURE_EMPTY_DRAWS`) 0.9 ms; whole frame 17.2/17.4 → 13.8/14.1 → 9.1/10.4 ms. **Vertex work is the biggest
  piece left: ~2–3.5 ms in the main view, ~7 ms of the whole frame with per-draw cost.**
- Why: the vertex arena turns itself off on this laptop ("guest memory can't be write-watched"), so indices are read
  in place and indexed draws are expanded (directIndex: one vertex shader run per triangle corner, no post-transform
  reuse). Stats agree: 502k vertex shader runs for 213k triangles (2.4 per triangle). 1324 indexed list draws a
  frame, 407k indices.
- **Next lead:** give the GPU real index buffers (byte-swapped copies in the index arena, cached) even when vertices are
  read in place, so the hardware reuses shaded vertices. Ceiling first: force the arena path on with hashing.

## [REJECTED] Vertex-reuse lead (9 Nov)
Ceiling switch in the build: `REGPU_MEASURE_FORCE_ARENA=1` keeps the vertex arena (real index buffers) on although
guest memory can't be write-watched here, so every buffer is hashed each frame.
- With decimation on (potato), the arena run lost decimation (374k triangles vs 213k), so it was no comparison.
- With `REGPU_HYPER_DECIMATE=0` on both sides, 2 pairs: vertex shader runs 633–635k for 374–376k triangles **in both
  modes** (1.69 per triangle). In-place reads already get the same vertex reuse; the hypothesis was wrong. The 2.4 per
  triangle seen on potato comes from decimation's simplified meshes, which reuse vertices less (but draw fewer).
- Arena forced: main view 6.06/6.09 → 9.62/7.83 ms and CPU renderer 14 → 21–42 ms (hashing). Worse on both.
Vertex work stays the biggest piece (~2–3.5 ms in the main view), but not through lost reuse. Remaining vertex levers:
the vertex shader's own cost (manual fetch and byte swap per attribute) and decimation that keeps reuse.

## [PARKED, below the bar] Cheaper vertex fetch (9 Nov)
Vertex fetch (`VFetchK` → `VFetch` → `LoadGuest`/`Swap`/`Unpack` in hlsl.cpp) decides everything per vertex at run time:
format switch, stride and offset from `vfl`, guest memory or in place (`vf.z`), endian mode (`vf.y`), and always
4 loads even for 1-dword formats.
Ceiling switch in the build: `REGPU_MEASURE_VFETCH_FIXED=1` (vertex shaders always read in place with the 32-bit swap).
2 pairs on potato: GPU 18.22/17.05 → 17.09/15.94 ms (−1.1 ms), main view 6.63/5.64 → 4.91/5.05 ms. **But the picture is
black** (80% pixels differ, `Forza-Motorsport-2-ReGPU/out/vfetchfix_compare.png`): most buffers are not that case, so
nearly all geometry went off screen. So −1.1 ms is an upper bound that also includes the lost rasterising: the fetch
logic itself costs less. A correct version (vfl/vf values baked per vertex-shader variant, like lean fetch for
textures) would likely give 0.3–0.6 ms. Parked; on Intel's weak vertex units it could be worth more.

## Climb 2, step 3: post passes and copies (9 Nov, measurements only)
Potato, `FM2_GPU_PASSES=1`, outside the main view (~9 ms of ~15):
- `w560`, 19 draws after a resolve's clear: **1.66 ms** at helpers 25%, 2.27 ms at helpers 100% (1 run). Helper scaling
  reaches it, yet a quarter of the pixels still costs 1.66 ms: the cost is not pixel count. Either a few very heavy
  draws (many taps or many vertices) or the segment also holds draws to another surface (the label is the surface at the
  segment's start). Needs a frame capture (F12 / `fm2ctl.py capture`) to name its draws and shaders.
- `w1280`, 39 draws (post composite + HUD): 1.3–1.6 ms. Full-screen; not scalable without lowering resolution.
- Resolve blit `w1280`: 0.92–0.96 ms. Resolves per frame (census): 6 at 1280×720 (formats 2407 and 2806), the rest
  small (shadow 256² depth, reflection 256²/512², bloom chain 32²–128², 320×180 ×3). The working colour target is
  RGBA16F (8 bytes per pixel); 8-bit surfaces were already measured small here (retest on Intel, shared DDR4).
- CPU, for the i3: renderer 12 ms/frame of which **constants 4.6 ms**, submit 2.0, textures 1.4, pipeline 1.2.
Next: capture one potato frame and name the 19 draws of the w560 segment; if one or two shaders dominate, a generic
"heavy screen pass" degrade (fewer taps or half-rate) is the candidate.

## Climb 2: the w560 segment named (9 Nov, frame capture)
Trace (`fm2ctl.py capture` on a potato race; `FM2CTL_EXE`, `FM2_RACENOW=1`): the segment is a 512×512 surface (pitch 560)
filled by one rect-list draw (ps `6bd46051fe3f74a0`, 48 vertices = 16 rects) and then a mip chain built by ps
`97ddb2985f0d80eb`, one draw + one resolve per level, 512 → 1 (9 resolves, the small ones 8×8). An environment /
reflection map rebuilt every frame.
Live flat-colour test (`ps HASH tint`, pass time with `FM2_GPU_PASSES`): base 1.33 ms; 6bd4 flat 1.32; 97dd flat 1.27;
back 1.28. **Neither shader is the cost.** What is left in the segment is the chain's 9 resolves (barrier + blit each,
the GPU drains between them) and the other draws sharing the segment. Lever, if any: refresh this chain every 2nd–3rd
frame (reflection map at lower rate, like the frozen helpers); ceiling ≤ ~1.3 ms, likely ~0.6. Not built.

## [PROVEN] Climb 3 step 1: culled draws skip their CPU setup (9 Nov)
Draw culling now decides right after `SelectSurfaces` in `DrawImpl`, before the pipeline lookup, constants and textures
(it used to decide at the draw call, after all of that). Recheck frames still do the full path; the query block reuses
the early key. The mesh key now uses the guest index address (`dmaBase`, count, vertices) instead of the host index
buffer, which decimation and the arena swap later. A/B switch: `REGPU_MEASURE_LATE_CULL=1` (old behaviour). Source
before: `experiments/renderer.cpp.pre-earlycull`.
3 interleaved pairs on potato: race fps 59.3/58.0/60.0 → 62.7/64.0/67.3 (**+9.5%**, won all 3); renderer CPU
13.58/12.79/12.83 → 12.55/11.36/11.13 ms (**−1.4 ms**); GPU 16.30/15.88/15.39 → 16.29/14.72/14.46 ms. Constants
4.12 → 3.46 ms, pipeline 1.28 → 1.07. Cull rate unchanged (~35%). Picture identical (0.8%: timer;
`Forza-Motorsport-2-ReGPU/out/earlycull_compare.png`). In potato automatically (it is how culling works now).

## [REJECTED] Climb 3 step 2: vertex-cache order for decimated meshes (9 Nov)
`meshopt_optimizeVertexCache` on each simplified index buffer (decimation worker; needed `vcacheoptimizer.cpp` in the
build). A/B `REGPU_MEASURE_NO_VCACHE_ORDER=1`. 3 interleaved pairs on potato (with early culling): GPU 15.90/14.87/14.32 →
14.73/14.89/14.67 ms (noise), race fps 62.0/62.0/66.0 → 62.0/60.7/65.3; vertex shader runs 513–514k → 512k; main view
4.76–5.31 vs 4.97–5.08 ms. Triangle order is not why decimated meshes reuse vertices less (meshopt_simplify's output is
already cache-friendly). Reverted (code and CMake); this entry is the record.

## [BELOW THE BAR on potato] Climb 3 step 3: alpha test into potato (9 Nov)
`REGPU_HYPER_A2M_TEST=1` on top of potato (with early culling), 3 interleaved pairs: GPU 14.20/15.08/14.51 →
13.89/14.15/14.58 ms (−0.4 ms, 2 of 3 pairs), race fps 66.7/65.3/66.7 → 63.3/66.7/64.0 (noise). It was −0.5 ms on the
7 Nov stack; culling already skips many of the affected draws. Not added to the preset; switch unchanged.

## [PROVEN] Climb 3 step 4: lean vertex fetch (`REGPU_HYPER_LEAN_VFETCH`, in potato, 9 Nov)
Each vertex shader's fetch layout (`vs.info.vertexFetchLayout`: constant slot, format, sign/normalise, stride, offset,
exponent, swizzle) is fixed per shader, yet `VFetchK`/`VMerge` read it from the `vfl` constants per vertex, so every
vertex ran the whole format switch and 4 loads. Now baked in as `static const uint4 kVfl[]` at translation
(renderer.cpp, next to the other HLSL transforms); the compiler folds the switch and drops unused loads. Lossless. Cost:
patched copies of a shader no longer share one compile. Accessor `HyperLeanVFetch()` (hyper.{h,cpp}); on in potato.
3 interleaved pairs on potato: GPU 15.49/16.15/15.73 → 14.76/15.34/15.05 ms (**−0.74 ms**, won all 3), race fps
64.7/56.7/61.3 → 64.0/62.0/62.7 (+3.3%), renderer CPU −0.47 ms; main view 5.31/6.09/5.28 → 5.16/4.93/5.40 ms.
Picture identical (0.7%: timer; `Forza-Motorsport-2-ReGPU/out/leanvfetch_compare.png`).

## [REJECTED] Climb 3 step 5: the resolve chain (9 Nov)
Batching the reflection mip chain's barriers is not possible losslessly: each level's draw reads the level resolved just
before it. Tested the other suspect instead: every resolve transitions the whole 1280×720 RGBA16F colour surface to
shader-readable and back, which on AMD can decompress it (DCC). `REGPU_MEASURE_COLOR_UAV=1` (measurement, in the build:
colour surfaces also allow unordered access, which turns DCC off). 3 pairs on potato: GPU 15.55/15.45/15.07 →
17.10/16.11/16.23 ms (**+1 ms, worse**: compression helps the main view more than the transitions cost); the w560
segment 1.34/1.31/1.33 vs 1.38/1.33/1.35 ms (unchanged). Decompression is not the chain's cost. Closed.

## Climb 3 closed (9 Nov): potato vs stock after it
2 pairs (potato runs also had `FM2_GPU_PASSES=1`, which costs a little): race fps 40.0/46.7 → 60.7/60.0 (**+39%**),
GPU 24.03/22.69 → 15.50/16.48 ms (−32%), renderer CPU 18.8/17.3 → 12.0/12.7 ms. Stock ran slower this session
(laptop warmer), so compare within a session. Kept from climb 3: early cull decision (+9.5% fps), lean vertex fetch
(−0.74 ms). Rejected: vertex-cache order, alpha test in potato (below bar), resolve-chain DCC test.
Potato frame now: main view 5.35 ms, reflection map chain 1.46, post composite 1.39, resolve blit 0.92, 218-draw
segments after the main resolves 0.83; CPU constants 3.85, submit 2.02, pipeline 1.34, textures 1.33 ms.

## CPU tries on the laptop side (9 Nov; the user asked this thread to try CPU work too)
Potato's CPU split: constants 3.85 ms = index list 0.19, constant banks 0.44, vertex fetch 0.47 (bools/loops 0.15,
uploads 0.68), the rest 0.51 (MSAA scale 0.16, **index buffer 1.11**).
- **[KEPT: decimation stays in potato]** Decimation off (`REGPU_HYPER_DECIMATE=0`) to save its per-draw lookup
  (index buffer 1.11 → 0.71 ms CPU): 3 pairs, race fps 64.0/66.0/63.3 (on) vs 62.7/60.0/60.7 (off), GPU 14.53/14.76/15.57 vs
  15.79/15.70/14.42 ms. Decimation still wins (+5% fps).
- **[BELOW THE BAR, kept: lossless]** Hash each index buffer once per frame: `GetIndexBufferLocked` records the hash it
  checked (`g_lastIndexHash`), `hyperdec::Apply` reuses it instead of hashing the same indices again. A/B
  `REGPU_MEASURE_DOUBLE_HASH=1`. 3 pairs: index buffer 1.10/1.03/1.01 → 0.97/1.03/1.02 ms; fps and GPU noise. The hash
  was never the cost: ~1300 lookups a frame at <1 µs each (mutex, hash maps, cache misses).
Conclusion: the per-draw CPU time is spread thin over many sub-millisecond pieces; no single piece outside the general
per-draw path is worth a dedicated cut. The remaining CPU win is that general rewrite (Intel thread's item 6).

## Deck benchmark (9 Nov): a base mistake caught
`tools/deck_bench.sh` (every speed-up alone vs native, stacks, presets; 3 rounds, native twice per round) first ran with
this machine's settings file at **anisotropic 0×** (ReGPU's default is 16×). Against that base the anisotropy cap does
nothing (it only lowers the level) and read −4 to −7% (heat noise), and every other gain was measured against a native
with filtering already off. First-pass medians (labels `dk-*`, base 45.7 fps / 22.1 ms GPU, kept for the record):
cull +11.1%, helpers 50/25 +9.5%, lean fetch 3 +6.7%, freeze +3.6%, A2M +2.3%, 1010 +2.3%, lean vfetch +0.8%,
decimate 10 −0.8%, potato +47.4%, everything +47.4%, mild +31.4%, low +43.0%, extreme +56.2%.
Re-run with `FM2_ANISOTROPY=16` on every run (labels `dq-*`, Turbo part `tq-*`); the deck uses those.

## 9 Nov: deck re-measure vs native 720p, 16x aniso base (labels dq-*, tq-*)
3 rounds, parked, 30 s, medians; base native 42.0 fps / GPU 23.97 / CPU 18.3 (tq run: 42.7 / 23.91 / 18.20). Pixel diff vs dq-native-2 (noise floor native2: 0.48 mean, 1.1% px>8).
| Variant | fps | vs native | GPU Δ ms | diff | Class |
|---|---|---|---|---|---|
| help25 | 50.7 | +21.7% | −3.76 | 3.74 | Hyper |
| help50 (2 runs) | 48.4 | +16.1% | −2.62 | 2.63 | Hyper |
| cull 64 + ALL | 48.0 | +15.2% | −2.57 | 0.41 | Hyper |
| lean3 | 46.0 | +10.4% | −1.87 | 0.48 | Turbo (near) |
| aniso2 / aniso0 | 46.0 / 44.7 | +10.4% / +7.3% | −1.05 / −1.42 | 1.78 / 3.01 | Hyper |
| lean2 (tq) | 45.3 | +6.1% | −1.56 | 0.45 | Turbo |
| cull0 + ALL (tq) | 44.7 | +4.7% | −0.85 | 0.48 | Turbo (1-frame-late reveal) |
| r1010 | 43.3 | +4.0% | −0.20 | 0.96 | Turbo (near) |
| a2m | 43.3 | +4.0% | +0.02 | 1.13 | Turbo at MSAA 1x; not proven alone |
| freeze 120/3 | 43.3 | +4.0% | −0.89 | 0.45 | Hyper |
| dec10 | 42.7 | +2.5% | +0.22 | 1.18 | Hyper; not proven alone |
| leanv | 42.0 | +0.8% | +0.18 | 0.49 | Turbo; not proven alone |
| potato / everything | 66.0 / 67.3 | +58.5% / +61.6% | −8.97 / −9.50 | 5.5 / 5.9 | Hyper |
| mild / low / extreme | 60.0 / 68.0 / 71.3 | +44.1% / +63.3% / +71.2% | — | 9.1 / 11.7 / 13.6 | lower res |
| **strict** (tq) | 50.0 | +17.1% | −3.93 | — | Turbo |
| **turbo** = strict + lean3 (tq) | 52.0 | **+21.8%** | −4.12 (CPU −3.5) | 1.59 | Turbo |
Deck: https://claude.ai/artifact/GTR3GcgsyBj3FGDESzVbZX version 9 (slides q-*). Screenshots out/bench_dq-*-2.png, tq-*-2.png.

## 15 Nov 2026: Intel's benchmarks redone on the laptop (branch hyper-intel, 3af446d)

Intel package merged (`_intel-package/`, only copy besides `P:\Hyper-package.zip`, verified identical with `cmp`).
Ran `sh tools/amd_redo_intel.sh` (about 4 h): iq_bench, iq_bench2, ax (native / laptop potato / Intel potato),
preset_bench, FSR at 1600x900. Intel set forced on AMD by env: `. tools/amd_intel_set.sh` then `$IP`.
Full tables, both machines side by side, failures and recreate commands: `Forza-Motorsport-2-ReGPU/docs/hyper/INTEL-VS-AMD.md`.
Medians: `out/amd_{iq,iq2,ax}_stats.json`; diffs `out/amd_iq_diffs.json`.

Headlines (laptop, median fps, native 42.8 to 43.2):
- [PROVEN] Intel potato set on the laptop: 77.3 fps (+79.1%) vs laptop potato 71.5 (+65.7%). GPU 12.0 vs 13.3 ms.
- [PROVEN] Intel Turbo v1 on the laptop: 55.9 (+29.7%, diff 0.90) vs laptop Turbo 52.6 (+22.0%, diff 1.67).
- [PROVEN] Cull 256 px + probe + hysteresis: +17.2% (laptop 64 px cull +12.3%, 0 px cull +8.4%).
- [PROVEN] plainmul +8.8%, fetchspec +7.4% on the laptop (Intel +13.4%, +14.8%).
- [NOISE on laptop] nodiscard, swizzle, shaderopt, leanlod, tailspec, bysize, leanv, a2m, r1010 (all under 3%).
- [INCONCLUSIVE] Potato + FSR 3 to 900p: two states, about 34 fps or 60 to 69 fps, same switches. Cause unknown.
- [INVALID] labels `ay-*` (empty FSR variables).

### 15 Nov 2026 (later): Potato default on AMD, FSR 3 explained
- [PROVEN] `src/gpu/hyper.cpp`: Potato's shader-cut set (was Intel-only, vendor 0x8086) is now the default on AMD (0x1002) too.
  `REGPU_HYPER_POTATO_SET=laptop` restores the older laptop set. Bench `pg-*`, 3 interleaved rounds, medians:
  native 42.2 fps, old set 71.3 (+66.4%, GPU 13.74 ms), new default 78.1 (+82.3%, GPU 12.01 ms), env-forced 78.2.
  Recreate: `sh tools/hyper_bench.sh pg-potato-N 30 FM2_UPSCALER=off FM2_FSR_SCALE=100 FM2_ANISOTROPY=16 REGPU_HYPER=potato` (add `REGPU_HYPER_POTATO_SET=laptop` for the old set).
- [EXPLAINED] FSR 3 "fast state" (60 to 69 fps) = runs stuck on the loading screen (28 to 169 draws/frame), not the race.
  `tools/hyper_bench.sh` now reruns a run whose last logged frame has under 400 draws. Clean re-measure `bb0-/bb1-pfsr3-1..3`:
  Potato + FSR 3 to 900p is 33 to 35 fps parked and driving.
- [OPEN] With FSR 3 the race start gets stuck on the loading screen in roughly a third of launches (6 of about 20 today).
- [INVALID] labels `ba0-*`, `ba1-*` (Intel set not loaded; stock + FSR 3).
- Tools added: `tools/fx_capture.sh` (60 parked + 180 driving frames per effect), `tools/fx_compare.py` (videos, heat maps, flicker scores).

### 15 Nov 2026 (evening): per-effect flicker compare (laptop)
Captures: `sh tools/fx_capture.sh both` (each of 22 effects alone + native + native2; 60 parked frames, 180 driving frames at
a 12 fps cap) -> `out/fx/`. Compare: `py tools/fx_compare.py ../flicker-compare` -> `reGPU_Hyper/flicker-compare/index.html`
(native | effect videos parked and driving, parked heat maps, `scores.json`).
- Parked noise floor (native2 vs native): 366 extra flickering px (crowd animation). Native itself: 358 events/frame.
- [FLICKER, parked] cull64 (`CULL_PIXELS=64 CULL_ALL=1`): 967 extra flickering px, 427 events/frame. The only effect clearly above noise.
- [NO EXTRA parked flicker] every other effect: 264 to 486 extra px (a2m 486, nodiscard 429 at the top; cull0 296 and cull256fix 278 below noise).
- [INCONCLUSIVE, driving] events/frame x0.87 to x1.10 of native (native2 x1.02); dec10 x1.10, lean3 x1.08, lean2 x1.07 highest.
  The driving runs are not frame-identical (aligned native2 vs native differs by 16 of 255 on average), so the score cannot
  separate flicker from different motion. Use the videos.
- [FAILURE] race start stuck on the loading screen in 10 of 24 driving launches at the 12 fps cap (and in FSR 3 launches): a
  general intermittent launch failure, not FSR-specific. fx_capture.sh retries after 100 s.
- [FAILURE] raw frame dumps (3.7 MB each, 20 GB) filled C: (88 MB free) and truncated 104 frames of drive/native2; frames
  recompressed losslessly (7 GB) and that run recaptured.

## 16 Nov 2026 (laptop): Potato car-highlight blink isolated and fixed (culling x quarter-size helpers)

**Symptom (user):** with Potato, a white stripe on the right side of the player's car flickers, easiest to see parked.
None of the 22 single effects showed it (user reviewed all in the "Hyper Flicker Review" artifact: all "none").

**Method:** stacks instead of single effects. `FX_STACKS=1 FX_DIR=out/fxs sh tools/fx_capture.sh static [regex]`
(60 parked frames each, native 720p, 16x anisotropy forced, upscaler off). New detector `tools/blink.py <capture dirs>`:
pixels with luminance std > 4 over the 60 frames where stock (`out/fx/static/native`) has std < 1, counted in the box
around the car (rows 360-670, columns 430-850), plus the dominant period. `tools/flicker.py` only counts one-frame
blips and missed this (the blink is 6 frames on, 6 off).

| Stack | Blinking px on the car | Period (frames) |
|---|---|---|
| Full Potato (`potato`) | 2,184 | 12 |
| Potato, `CULL_PIXELS=-1` (`p-nocull`) | 0 | - |
| Potato, `HELPERS=100` (`p-help100`) | 3 | - |
| Potato, `NODISCARD=0` (`p-discard`) | 597 | 12 |
| Potato, `RESOLVE1010=0 RESOLVE_BY_SIZE=0` (`p-noresolve`) | 599 | 12 |
| Potato, `FREEZE=0` (`p-nofreeze`) | 2,188 | 15 |
| Potato, `DECIMATE=0` / shader cuts off / `VCACHE=0` / `ANISO=16` | 2,185 / 2,184 / 2,184 / 2,173 | 12 |
| Culling alone, Potato's settings (`c128`) | 121 | 4 |
| Geometry group (`g-geom`), geometry + shader (`g-geom-shader`) | 131 / 135 | 4 |
| **Culling + `HELPERS=25` only (`c128-help25`)** | **1,937** | 4 |
| Stock second run (`out/fx/static/native2`), single `freeze`, `help25` | 0 | - |
| Single `cull256fix` / `cull64` (15 Nov captures) | 324 / 79 | 4 |

**Result:** the clash is draw culling (`CULL_PIXELS=128 CULL_ALL=1 CULL_PASSES=1 CULL_PROBE=1 CULL_HYST=200`) with
helper surfaces at 25%. User confirmed on the running game (two-part stack, parked) that this is the flicker.

**Cause (hypothesis, consistent with the code, not instrumented):** with `CULL_PASSES` the cull also acts on draws
into helper surfaces; at 25% size they cover 1/16 of the pixels and fall under the 128 px threshold. The probe
re-measures a culled draw without depth writes, so it can count more pixels than the real draw: probe says big, the
draw comes back, the real draw measures tiny, it is culled again.

**Fix [PROVEN]:** `REGPU_HYPER_CULL_PIN` (default on, `0` = off), `renderer.cpp` cull decision: a draw that comes back
from being culled twice within 64 frames is never culled again. Log line "hyper cull: ... N draws pinned" (15 parked).
- Blink score: `c128-help25-pin` 0, `potato-pin` 0, `potato-pin0` (fix off, same build) 600, period 12.
- Speed, 3 interleaved parked rounds, Potato, median fps: fix on 74.0 / 75.7 / 75.2, fix off 73.8 / 75.3 / 74.0
  (labels `pin-on-*`, `pin-off-*`). No cost. GPU 12.3 to 12.8 ms both.
- Not checked: driving with the fix; the 121 px culling-alone blink with the fix (expected fixed by the same pin).
- Not tried: scaling the pixel threshold by the helper surface size (would keep those draws from being culled at all).

**Failed / spoiled along the way:** stopping the first background capture left its child script running, so two capture
scripts killed each other's game; `p-nofreeze`, `p-help100`, `p-nodec`, `p-aniso16` were recaptured. Driving captures
exist only for `potato` and `p-nocull` (`out/fxs/drive`). The planned stack review artifact was not published: the
lead came straight from the captures. `tools/fxs_artifact.py` (scores + videos for it) is written but never run.

**Confirmed by eye (user, 16 Nov):** full Potato with the fix, parked, running game: the stripe no longer flickers.

**Driving check of the cull pin (16 Nov, later):** `FX_STACKS=1 FX_DIR=out/fxs sh tools/fx_capture.sh drive 'potato-pin'`
(12 fps cap, 180 frames; fix on = `potato-pin`, fix off = `potato-pin0`; 5 launches stuck on the loading screen and
were retried). Scored on the 2,184 pixels that blinked parked, share of their variation at the 12-frame period:
stock 0.012 / 0.010 (two runs), Potato without culling 0.012, Potato before the fix 0.020, fix off 0.039, **fix on
0.013**. Jumps over 20 levels per pixel: fix off 18.3, fix on 10.4 (stock 19.6 / 16.6: the highlight moves on its own
while driving, so this score is weak). Reading: with the fix the 12-frame rhythm is back at the stock level; the
driving score is supportive, not conclusive. Clip for the eye: `reGPU_Hyper\drive_car_fix_off_vs_on.mp4` (car area, 2x).

## 16 Nov 2026 (laptop): Hyper Settings window (prototype)

`tools/hyper_menu.cpp` -> `build\rel\hyper_menu.exe` (CMake target `hyper_menu`, next to fm2.exe). A plain Win32 form;
no game code and no settings.ini edits: it starts `fm2.exe game` (or `game --racenow drive=0|1,fps=0`) from the repo
root with every choice as an environment variable. `REGPU_HYPER=off` always and every cut explicit, so the form is
exactly what runs (the "Passed to the game" box shows the line, for copying into this log).
- Presets: Off, Mild, Low, Extreme, Potato (the shader-cut set) fill the controls; editing by hand clears the preset.
- Custom presets: name + "Save custom"; stored as sections of `Documents\fm2\hyper_menu.ini`; "Delete custom". The
  last state is kept in section `last` and restored on open.
- Groups: Surfaces (helper size, anisotropy, 10:10:10:2, resolve by size, freeze + refresh), Geometry (cull threshold,
  blended draws, every pass, probe, hysteresis, pin, decimation, vertex-cache reorder), Shaders (9 checkboxes + lean
  fetch), Rendering and upscaling (Normal / FSR 1 / FSR 3, output resolution, render scale with the computed render
  resolution, sharpening, one-pass bilinear, FSR 3 version), Display and launch (fullscreen, vsync, frame cap, fps
  overlay, Self-Driving, Start game, Start in race).
- Checked: builds; opens with Potato; "Start in race" starts the game with the expected command line.
- Not checked: "Start game" (boot from the start), saving/deleting a custom preset, the FSR modes from the form,
  Self-Driving from a normal boot (auto-drive only works on the Nissan Speedway oval).
- Known limits: FSR 1 render scale is clamped by the game to 50-100%; Mild/Low/Extreme's scene scale (50/35/25%) is
  shown as FSR 1 at 50% for that reason; no "Auto" preset.

## 16 Nov 2026 (laptop): race-start hang (stuck on the loading screen) explained and fixed in the race-now shortcut

**Symptom:** a `--racenow` launch sometimes stays on the loading screen for ever (the "FSR 3 fast state", the stuck
driving captures, and a launch from the Hyper Settings window). Not Hyper: seen with Hyper off, Low and Potato.
**Findings:** thread dumps of a stuck game (`build\stackdump.exe <pid>`, `out/stuck_stacks.txt`, `stuck_stacks2.txt`):
no deadlock, every thread in a bounded wait, the loading screen keeps drawing. Log: "racenow: launching", the nine
loader threads are created, then no track file is ever read (good runs log "wheel: the game says not driving" next).
The main game thread waits on a full work queue (`sub_8245CED8`) that nothing drains; which thread should is not found.
**Experiment:** `sh tools/stuck_test.sh 10 nowait=FM2_RACENOW_WAIT=0 wait30=FM2_RACENOW_WAIT=30` (12 fps cap, upscaler
off, drive=1, interleaved; good = "wheel: the game says" within 40 s of "racenow: launching"). Results in
`out/stuck_test.txt`, logs `out/stuck_<arm>-<round>.err`.
- No wait (as before): **8 of 10 stuck**.
- `FM2_RACENOW_WAIT=30` (loading screen held 30 updates, about 0.55 s, between setting the race up and launching): **0 of 10 stuck**.
**Fix [PROVEN]:** the wait is now the default (30 updates; `FM2_RACENOW_WAIT=0` = old behaviour), in the race-now boot
flow (`src/kernel/zip_overlay.cpp`, our debug shortcut, not game code). The shortcut launched the race 0.3 s after the
profile loaded, before some start-up work had finished. What exactly was unfinished is not identified.
**Not checked:** whether a normal boot through the menus can stick (not expected: a player takes far longer).

## 16 Nov 2026 (laptop): Hyper Settings restyled, hover cards, Turbo and Lossless presets

`tools/hyper_menu.cpp` rewritten as a custom-drawn window (GDI+ shapes, GDI ClearType text, per-monitor DPI via
`tools/hyper_menu.manifest`; links gdiplus, dwmapi). Graphite with an amber accent; dark title bar and dark dropdown
menus. Same settings, same ini keys and same environment as before; all controls kept (toggles, segmented choices,
dropdowns). Four columns of cards (Rendering; Surfaces + Display; Geometry; Shaders), each cut card shows how many of its
settings are on. Sub-settings are indented and greyed when the setting they need is off (render scale, sharpening and
the one-pass upscale need an upscaler; FSR 3 version needs FSR 3; cull options need culling; triangle reorder needs mesh
simplification, since it runs inside the simplifier).
- **Hover cards:** every setting and button. What it does, what you'd see, and a table of its measured gain alone vs stock
  on the Vega laptop and the HD 630 (INTEL-VS-AMD.md section 1, HYPER-SETTINGS-GUIDE.md), the current choice highlighted,
  green / red / grey for gain / loss / noise or not measured, plus notes on how it was measured.
- **Summary strip:** measured whole-configuration numbers when the settings match a measured one (stock, Potato at 720p,
  Potato + FSR 1 or FSR 3 to 900p); otherwise "not measured" or "custom". Right side: the resolution drawn and shown.
- Header: preset dropdown, name box, Save, Delete (two clicks). Footer: Copy launch line (env + command to the
  clipboard), Self-driving, Start game, Start in race. Status messages in the footer.
- **Presets (built in, in this order):** Off, **Lossless** (new: native, only the cuts measured with no visible change:
  10:10:10:2, resolve by size, no discard, shader opt, plain multiply, fetch spec, tail spec, swizzle selects, lean LOD;
  triangle reorder left out because it needs mesh simplification), Mild, Low, Extreme, **Turbo** (the user's custom
  preset made built-in: Potato's cuts with culling of fully hidden draws only, plus lean fetch v3 and lean vertex fetch),
  Potato. The custom `[Turbo]` section was removed from `Documents\fm2\hyper_menu.ini` (backup:
  `hyper_menu.ini.bak_before_test`).
- Checked: builds; screenshots at 100% DPI; hover cards with tables; Copy (clipboard holds the line), Save and Delete
  (ini sections appear and go); Turbo's saved settings are recognised as the built-in Turbo.
- Not checked: the dropdown menus by click (they are standard popup menus), Start buttons from the new window, other
  DPI scales. Lossless and Turbo are not benchmarked as wholes.

**Lossless and Turbo benchmarked (16 Nov, later):** `sh tools/preset_bench2.sh` (the window's presets as it starts them:
REGPU_HYPER=off and every cut explicit; one 60 s warm-up each, then 3 interleaved rounds, parked, 30 s, 720p native,
16x anisotropy forced, upscaler off; labels `pbw-*`, `pb-*` in `out/bench.txt`). No failures, no stuck launches.
| Preset | Median fps, rounds 1/2/3 | Median | vs stock | GPU ms | 1% low |
|---|---|---|---|---|---|
| Stock | 43.6 / 43.9 / 43.6 | 43.6 | | 22.5-23.3 | 17-19 |
| Lossless | 52.9 / 53.0 / 53.2 | 53.0 | **+21.6%** | 18.8-19.0 | 18-19 |
| Turbo | 59.6 / 66.6 / 65.8 | 65.8 | **+50.9%** | 12.7-14.0 | 10-18 |
| Potato | 80.6 / 79.6 / 79.3 | 79.6 | **+82.6%** | 11.7-12.1 | 18-21 |
- Lossless: a fifth of the frame for free, very steady across rounds.
- Turbo is 17% slower than Potato and less steady (first round 59.6, 1% lows down to 10). **By design (user):** Turbo
  cuts less. It culls only fully hidden draws (0 px), not small ones; culling past 0 px is worth a lot of speed but
  costs the experience (small distant things vanish), so Turbo keeps them. (An earlier guess here, that the gap was
  CPU-side, is withdrawn.)
- The window's summary now shows these numbers when Lossless or Turbo is selected (HD 630: not measured).

**Hyper Settings froze (user, 16 Nov, fixed):** the window stopped repainting for a while, then recovered. Cause: my
late-mouse-leave handling re-armed mouse tracking whenever the cursor was over the window, including its title bar;
over the title bar that posts another leave at once, an endless loop. Repro (`scratchpad/freeze.ps1`: cursor on the
title bar plus one WM_MOUSELEAVE): 1,609 ms of CPU in 3 s before, 16 ms after. Fix: re-arm only when the cursor is in
the client area. Hover cards checked again after the fix. Paint cost measured at the same time: 30 ms per full repaint.

**Hyper Settings layout (user, 16 Nov):** the measured-summary strip is removed (the user found it not useful; the
whole-preset numbers stay in this log). "Start game" is now the large yellow button on the far right; "Start in race" is
renamed **Test Race** and sits to its left. The resolution line ("Draws … and shows …") moved into the Rendering card's
subtitle. The window is 70 px shorter.

**Hyper Settings layout pass (user asked for alignment and comfort, 16 Nov):**
- Columns rebalanced (Rendering + Display, Surfaces + Experimental, Geometry, Shaders) and each column's last card now
  runs to one shared bottom edge, so the grid ends in a straight line instead of four ragged heights (284 / 510 / 356 /
  428 px before).
- New **Experimental** card ("In no preset; for testing"): lean vertex fetch, alpha-to-mask as alpha test, lean texture
  fetch. The Shaders card now holds only the seven lossless cuts, so its "Lossless" subtitle is true (alpha-to-mask
  removes content). Lossless shows "7 of 7 on".
- Spacing on one scale: 24 px window margin, header-to-cards and cards-to-footer gaps; 16 px between cards; 12 px card
  bottom padding. Header controls, card edges and footer buttons share the same left and right edges.
- Window 1384 x 757 at 100%.
- Header and footer slimmed (user): both 56 px instead of 78 / 76. The title is 21 px and the subtitle sits on the same
  line; header controls 32 px, footer buttons 34 px. Window 1384 x 715 at 100%.
- Presets trimmed (user): **Mild, Low and Extreme removed**; the window offers Off, Lossless, Turbo, Potato and the
  custom ones. **Turbo no longer turns on the Experimental options** (lean texture fetch v3, lean vertex fetch): it is
  now Potato's cuts with culling of fully hidden draws only. The Turbo benchmark above (65.8 fps) was with those two
  on, so it no longer describes this Turbo.
- **Potato culls draws of 256 px or fewer (user, 16 Nov), was 128.** Changed in both places Potato is defined: the
  window's preset (`tools/hyper_menu.cpp`) and the engine's Potato defaults (`src/gpu/hyper.cpp`, so
  `REGPU_HYPER=potato` and `auto` match). Measured alone, 256 px with probe + hysteresis was +17.2% (laptop) and +6.7%
  (HD 630). The Potato numbers above (75-80 fps) were at 128 px; Potato at 256 px is not benchmarked yet.
- **Potato redraws frozen helper views every 4 frames (user, 16 Nov), was 3.** Both places: `tools/hyper_menu.cpp`
  (Potato preset; Turbo keeps 3) and `src/gpu/hyper.cpp` (engine Potato defaults). Caution from the code and the guide:
  an odd refresh keeps double-buffered views alternating; with an even one, one buffer of such a view may be redrawn
  every time and the other never. On the HD 630, 4 gained nothing measurable over 3. Not yet checked by eye or
  benchmarked.
  **Not built yet:** at 21:16 the Visual Studio 2022 folder was emptied while an installer (msiexec) ran, so
  `tools\vcenv.cmd` finds no compiler or CMake. `fm2.exe` and `hyper_menu.exe` are from 21:02 (cull 256 px included,
  refresh 4 not). Rebuild once Visual Studio is back.
- Hyper Settings: lean texture fetch choices renamed **lv1 / lv2 / lv3** (levels) instead of v1 / v2 / v3 (user),
  in the buttons and the hover card. Same values (`REGPU_HYPER_LEAN_FETCH=1|2|3`). Not built yet (Visual Studio gone).
- **Build environment replaced (16 Nov):** Visual Studio 2022 Community and the Windows SDK disappeared at 21:16. At the
  user's request a lean install: Visual Studio **Build Tools** 2022 with only `VC.Tools.x86.x64`, `Windows11SDK.26100`
  and `VC.CMake.Project` (1.7 GB, `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`). `tools\vcenv.cmd`
  already prefers Build Tools; `build\rel` was regenerated with `cmake -S . -B build\rel` (it held the old CMake and
  Ninja paths). Only the changed files recompiled. Built at 21:26: `fm2.exe` (Potato redraw every 4 frames) and
  `hyper_menu.exe` (lv1 / lv2 / lv3). A cancelled full Community install left a 49 MB unregistered folder in
  `C:\Program Files\Microsoft Visual Studio\2022\Community` and a paused entry in the Visual Studio Installer.

## 16 Nov 2026 (laptop): the game's own AI drives the player's car (benchmark driver prototype)

**Found:** Career's "Hire Driver" (`Flows/CareerCircuitRace.xml`) hands the player's car to the game's driver AI with
`Lobby.activePlayer.aiDriverId = id`, `userCar:setAIDriverId(id)`, `GameLib:RuntimeReInitCareerHireFireDriver()`;
drivers come from the `AIPlayers` table. The race scripts (`Countdown.lua`, `RaceCamera.lua`) already handle it.
**Built (race-now shortcut only, `src/kernel/zip_overlay.cpp`, `src/main.cpp`):** payload `ai=1` (`FM2_RACENOW_AI=1`).
The boot flow picks the most skilled free driver (`SELECT Id FROM AIPlayers WHERE isUsed = 0 ORDER BY UISkill DESC`);
at the race's pre-start point (the car exists there; setting it earlier failed: "attempt to index userCar") it calls
`setAIDriverId` and the hired-driver re-init. Both report ok in the log. Use with `drive=0`.
**Result:** the AI races the car in Arcade too, on its own: 83 mph out of turn 1, 146 mph on the back straight, a real
line through the banking, 4th to 2nd in 54 s (`out/aidrive_sheet.png`, frames in `out/frames`).
**Consistency** (`sh tools/ai_consistency.sh`; Potato, 720p native, 40 s from the race start, interleaved; labels
`aic-*`): game AI 88.8 / 89.8 / 87.6 fps (spread 2.2, 2.5%); our vision bot 82.8 / 81.9 / 80.8 (spread 2.0, 2.4%).
Same run-to-run consistency, so the AI driver is usable for player benchmarks. Its fps is higher than the bot's
because it drives a different line and faster (different scenery); compare runs with the same driver only.
- **Hyper Settings: Driver choice (16 Nov).** The footer's Self-driving switch became **Driver: You / Bot / AI**
  (ini key `autodrive`, values 0 / 1 / ai, so old saves still load). Test Race starts `--racenow drive=0,ai=1,fps=0`
  for AI and `drive=1` for Bot. Hover card explains each and shows the consistency numbers above. Checked: picking AI
  and pressing Test Race started fm2.exe with `drive=0,ai=1,fps=0`. Not checked: the AI on other tracks (Test Race
  always runs Nissan Speedway).

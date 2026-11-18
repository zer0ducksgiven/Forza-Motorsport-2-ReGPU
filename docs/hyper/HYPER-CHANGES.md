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

## Not features (don't upstream)

| Change | Why |
|---|---|
| `src/kernel/stubs.cpp` | Regenerated by `tools/recomp.sh` |
| `tools/vcenv.cmd` | Local path fallback to VS 2022 Community |
| `thirdparty/XenonRecomp` | The repo's own patch applied (`patches/xenonrecomp-fm2.patch`) |

## Intel box: native potato vs native (8 Nov 2026)

Machine: Core i3-7100T (2C/4T, 35 W), Intel HD 630 (driver 31.0.101.2135), 16 GB. Packaged laptop `fm2.exe`, 720p native, parked
Nissan Speedway, `tools/hyper_bench.sh` 30 s, `BENCH_PERF=1`. Native: `FM2_UPSCALER=off REGPU_HYPER_ANISO=2`. Potato:
`FM2_UPSCALER=off REGPU_HYPER_ANISO=0 REGPU_HYPER_HELPERS=25 REGPU_HYPER_RESOLVE1010=1 REGPU_HYPER_DECIMATE=10`.

| Run | race fps | GPU ms/frame |
|---|---|---|
| native-2 / native-3 | 16.0 / 16.0 | 68.67 / 68.48 |
| potato-1 / potato-2 / potato-3 | 19.3 / 19.3 / 18.7 | 54.76 / 56.02 / 54.14 |

**Result: +19% race fps (16.0 → 19.1), GPU −20% (68.6 → 55.0 ms).** Laptop: +34%. Still deeply GPU-bound (~50 ms/frame waiting on
the GPU). `intel-native-1` is void: `--racenow` stuck on a 99-draw scene (262 fps); the bench script does not detect this yet.

## Intel batch 1: switch-only retests and ceilings (8 Nov 2026)

On top of the native potato stack (6 baseline runs: 19.1 fps, GPU 54.1 ms). 2 parked runs each, `tools/intel_batch1.ps1`.
Race fps on this box moves in ~0.67 fps steps, so **GPU ms is the deciding column** here.

| Experiment | race fps | GPU ms | vs base GPU | Flag |
|---|---|---|---|---|
| Bilinear sampler (`REGPU_MEASURE_BILINEAR=1`) | 19.7 | 55.0 | +0.9 | none |
| `FM2_NO_DEPTH_ALPHATEST=1` | 19.7 | 55.1 | +1.0 | none |
| Looser decimation (`_ERROR=0.6`, `_MIN=100`) | 19.4 | 53.7 | -0.4 | none |
| `FM2_DRAW_WORKERS=3` | n/a | n/a | n/a | **[REJECTED ON INTEL]** every run crashed or stuck (2 cores) |
| Ceiling: GPU freed (`EMPTY_DRAWS`) | **54.0** | 5.6 | | **CPU ceiling is 54 fps: 60 fps needs CPU cuts too** |
| Ceiling: cars skipped (`SKIP_BLENDED=4`) | 22.7 | 43.2 | -10.9 | cars cost ~11 ms (laptop 3.3) |
| Ceiling: all blended skipped (`SKIP_BLENDED=1`) | 26.0 | 39.6 | -14.5 | |
| Ceiling: helper passes skipped (`SKIP_PITCH=560,280,320`) | 21.7 | 47.5 | -6.6 | even at helpers 25% |
| Ceiling: stripped texture fetch (`FAST_TFETCH`) | 19.4 | 47.6 | **-6.5** | laptop -2: fetch ALU is an HD 630 bottleneck |
| Ceiling: no alpha test (`NO_ALPHATEST`) | 20.0 | 47.7 | **-6.4** | laptop -0.6 |
| Ceiling: full-screen resolves skipped | 19.4 | 54.1 | 0 | resolves are free here after 10:10:10:2 |

Takeaways: the HD 630 is shader-ALU bound in the texture-fetch helper and the alpha-test path, far more than Vega. Top leads for the
rebuild: lossless fast fetch and no-discard (both "no gain" on the laptop), then the car shaders. CPU: renderer ~49 ms/frame of
which most is GPU wait; the 54 fps empty-draw ceiling means per-draw CPU work must drop ~10% for 60.

## Intel batch 2: patch experiments (8 Nov 2026, rebuilt exe)

Rebuilt `fm2.exe` on this box (LLVM 23, MSVC 14.44 libs) matches the packaged one: potato 18.7 fps / GPU 52.8 ms (`i2-newexe-1/2`).
Patch tooling: `tools/intel_ab.ps1` (incremental build + base/test pairs on the Intel stack), `tools/imgdiff.py` (picture check).

| Experiment | base fps / GPU ms | test fps / GPU ms | Change | Flag |
|---|---|---|---|---|
| **No-discard pixel shaders** (`REGPU_HYPER_NODISCARD=1`; `nodiscard_patch.py` for hyper/settings + `nodiscard_v2_renderer.diff -p0` on renderer.cpp) | 18.7/51.11, 18.7/52.80 | 20.7/46.18, 20.7/46.10 | **+10.7% fps, -5.8 ms GPU** | **[PROVEN ON INTEL] KEPT** (laptop: none). Picture identical apart from motion (`out/nodiscard_intel_compare.png`) |
| Lossless fast fetch v2 (`REGPU_HYPER_FASTFETCH=1`; insert `experiments/fastfetch_block.cpp` in `GetShader` before the first `cs.cacheKey = Fnv(`) | 22.0/46.91, 22.7/46.73 (warm) | 20.7/49.16, 22.0/47.67 (warm) | **-4.5% fps, +1.6 ms GPU** | **[REJECTED ON INTEL]** reverted. The early-out branch makes every fetch bigger; first pair void (40 shaders compiled mid-race). The 6.5 ms fetch ceiling needs per-pipeline specialisation instead (roadmap phase 3) |
| Heavy-shader mip bias (`REGPU_HYPER_HEAVY_LOD=2`; insert `experiments/heavy_lod_block.cpp` in `GetShader`) | 22.0/46.66, 22.7/46.76 | 20.7/46.86, 22.0/47.25 | +0.3 ms GPU, fps noise | **[REJECTED ON INTEL]** reverted. Smaller mips on the car shaders change nothing: they are ALU-bound here, not bandwidth-bound |
| Mip drop (`REGPU_HYPER_MIPDROP=2`; `experiments/mipdrop_patch.py`) | median 21.8/21.7/21.9/21.8 | median 21.9/22.0/21.8/22.1 | +0.5%, GPU same | **[REJECTED ON INTEL]** reverted (restore from `experiments/intel-keep/`). Race fps showed +6% but averages only 3 readings |

**Metric change (8 Nov):** race fps averages only ~3 fps-meter readings per 30 s run, so on this ~20 fps box it moves in 0.67 fps
steps. `hyper_bench.sh` now also prints `median N fps`: the median of the per-second perf readings from 10 s after the race start
(~20 readings). Decide on median fps + GPU ms. Recomputed for earlier runs with `tools/median_fps.ps1`: native 15.2, laptop potato
stack 18.5, **no-discard 18.4 to 21.25 (+15.5%)**.
| **Optimised shader compile** (`REGPU_HYPER_SHADER_OPT=1`, new switch in `renderer.cpp`: `ShaderOptLevel()`, `OptFlags()`, level folded into `CacheSeed`; default -1 keeps `D3DCOMPILE_SKIP_OPTIMIZATION`) | median 21.8/21.9, GPU 46.98/46.92 | median 22.1/22.2, GPU 45.52/45.72 | **+1.4% fps, -1.3 ms GPU** | **[PROVEN ON INTEL] KEPT** (laptop: none at O3). Needs one warm boot per level (new bytecode, then driver pipelines: first measured run 17.2 fps). Level 3 vs 1: GPU 45.6/45.5 vs 43.1/45.8, median 22.3/22.3 vs 21.8/22.3: no better, keep 1. Settings key to add with the Intel preset |
| Half-precision pixel registers (`REGPU_HYPER_HALF_REGS=1`: `min16float4 r0..r63` in pixel shaders, new; patch `experiments/intel_half_regs.diff`) | median 21.9/22.0/22.0/21.8, GPU 45.8/47.6/47.5/45.8 | median 21.6/21.9/21.6 (+1 warming run 17.2), GPU 47.5/46.6/47.1 | none | **[REJECTED ON INTEL]** reverted. No change at all: the HD 630 DX12 driver most likely ignores DXBC min-precision hints. A real 16-bit path would need DXIL (dxc, SM 6.2 `-enable-16bit-types`), noted for phase 4 |
| Depth prepass on the Intel stack (`REGPU_HYPER_PREPASS=1`; hand-merged onto no-discard v2: `experiments/intel_prepass_merged.diff` against `experiments/intel-keep/`) | median 22.4/22.3, GPU 47.26/47.34 | median 21.6/21.6, GPU 48.68/48.84 | **-3.4% fps, +1.5 ms GPU** | **[REJECTED ON INTEL]** reverted. 844 draws/frame prepassed, 7 flushes, picture correct. Doubling the opaque draws costs more than the hidden shading saves, even here |
| Half-rate helper passes | | | | **[DEFERRED to roadmap phase 5]** the laptop patches no longer apply (the first needs a removed measurement block); to be rebuilt by hand with the helper-pass work |
| Ceiling: half the triangles of every draw (`REGPU_MEASURE_HALF_VERTS=1`) | median 22.3/22.1, GPU 47.07/47.71 | median 30.3/30.2, GPU 34.44/34.41 | **+36% fps, -13.0 ms GPU** | ceiling. PS runs 7.68M to 5.69M (-26%), VS runs 848k to 432k: GPU time tracks pixel-shader runs; vertex work is ~1 ms at most |
| Welded decimation, sloppy decimation | | | | **[SKIPPED ON INTEL: measured]** they cut vertex work, which the half-triangles ceiling puts at ~1 ms; real decimation keeps the pixels. Sloppy also removes effects (rule 2) |
| 8-bit colour surfaces, whole-surface clears | | | | **[SKIPPED ON INTEL: measured]** bandwidth cuts; mip drop and heavy-shader mip bias showed bandwidth is not the limit on this box |

**Phase 1 closed (8 Nov).** Kept on Intel: no-discard (+15.5%), fxc optimisation level 1 (+1.4%, -1.3 ms). Climb: native 15.2 to
22.15 fps median. The HD 630 is bound by pixel-shader ALU: GPU time follows pixel-shader runs (7.7M/frame, 8.3 per screen pixel),
not bandwidth or vertices. Next levers: fewer pixel-shader instructions per run (alpha-test and fetch specialisation, cheaper car
shaders) and fewer runs (blended overdraw).

## Intel batch 3: the four skipped experiments, run anyway (8 Nov 2026)

On the Intel stack (no-discard, fxc level 1). `tools/intel_batch3.ps1`; one build with all four behind switches.

| Experiment | base median / GPU | test median / GPU | Change | Picture | Flag |
|---|---|---|---|---|---|
| **Sloppy decimation** (`REGPU_HYPER_DECIMATE_SLOPPY=1`, error unbounded) | 22.1/47.73, 22.4/46.99 | 26.4/40.00, 26.5/39.91 | **+19% fps, -7.4 ms GPU** | 13.3% of pixels differ: pit building striped, roof torn, tachometer arc reshaped | **breaks rule 2 as is**; salvage under test (HUD excluded, error bound) |
| Welded decimation (`REGPU_HYPER_DECIMATE_WELD=1`, rebuilt from the ledger) | 22.4/46.71, 22.0/47.66 | 22.1/47.67, 21.7/48.41 | -1.4%, +0.9 ms | 2.9% | **[REJECTED ON INTEL]** |
| 8-bit colour surfaces (`REGPU_MEASURE_COLOR8=1`, ceiling) | 22.3/45.61, 22.3/45.60 | 22.7/46.30, 22.8/46.32 | +2% fps, +0.7 ms GPU | 2.6% (HDR clipped) | ceiling: colour bandwidth is not the limit |
| Whole-surface clears (`REGPU_HYPER_FULL_CLEAR=1`) | 22.3/47.10, 22.3/47.31 | 22.1/47.83, 22.1/47.62 | -0.9% | identical | **[REJECTED ON INTEL]** |

**Correction to the phase 1 close:** sloppy cut pixel-shader runs only 3.6% (7.68M to 7.40M) but triangles drawn 20% (311k to
248k) and GPU time 7.4 ms. Geometry does cost real time on the HD 630 (many small triangles); the half-triangles ceiling's
13 ms was not mostly pixels. Geometry reduction is a lever here after all.
| Sloppy, HUD excluded (draws without depth test) + error bound (`REGPU_HYPER_DECIMATE_SLOPPY_ERROR`), base median 22.3 / GPU 47.1 | | 1.0: 26.5/40.00; 0.05: 25.3/41.72; 0.02: 24.4/43.23; 0.01: 23.9/43.98 | +19% / +13% / +9% / +7% | 13.3% / 15.2% / 10.7% / 9.2% | **[PENDING USER DECISION]** every bound shifts textures on buildings (clustered vertices take another vertex's UVs/normals); tachometer arc still altered (drawn with depth test on, so the HUD exclusion misses it). At tight bounds triangles drawn exceed the base (395k vs 311k) yet GPU is 3 ms lower: part of the gain is cheaper shading of smeared attributes. Images `out/sloppy_<bound>_compare.png` |

## Sloppy investigation (8 Nov 2026)

| Experiment | base median / GPU | test median / GPU | Change | Picture | Flag |
|---|---|---|---|---|---|
| Vertex-cache reorder (`REGPU_HYPER_VCACHE=1`: `meshopt_optimizeVertexCache` on every decimation candidate, cut or not; new) | 22.4/47.12, 22.1/47.54 | 22.3/47.25, 22.5/46.80 | none | identical | **[NO GAIN ON INTEL]** |
| **Overdraw reorder** (`REGPU_HYPER_VCACHE=2`: vertex cache then `meshopt_optimizeOverdraw`, threshold 1.05; new, lossless) | 22.1/47.65, 22.4/47.12 | 23.1/45.41, 23.4/45.21 | **+4.5% fps, -2.1 ms GPU** | identical (1.2%, motion) | **[PROVEN ON INTEL] KEPT** (needs `vcacheoptimizer.cpp`, `overdrawoptimizer.cpp` in CMake). Front-to-back triangle order lets early-Z (on since no-discard) reject hidden pixels |
| Sloppy 0.02 on top of overdraw reorder | 23.1/45.48, 23.4/45.21 | 24.0/44.00, 24.2/43.82 | +3.7%, -1.4 ms | 10.0% | what is left at a recognisable bound |
| Sloppy unbounded on top of overdraw reorder | 23.4/45.09, 23.4/45.09 | 26.3/40.25, 26.4/40.01 | +12.6%, -5.0 ms | 13.5% | the rest is real triangle removal (~20%), the part that wrecks buildings |

Finding: of sloppy's original 7.4 ms, ~2 ms was triangle order (now kept losslessly as `REGPU_HYPER_VCACHE=2`). The remainder is
genuine geometry removal; vertex clustering keeps one vertex's UVs per cluster, so texture smear comes with every cut. Salvaging more
needs screen-size-aware LOD (cut only meshes that are small on screen), which ReGPU cannot do generically without the view matrix.

## Phase 2 opening (8 Nov 2026): alpha-test ceiling and the Xenos multiply rule

| Experiment | base median / GPU | test median / GPU | Change | Picture | Flag |
|---|---|---|---|---|---|
| Ceiling: no alpha test (`REGPU_MEASURE_NO_ALPHATEST=1`) on the current stack | 23.1/42.58, 22.6/45.75 | 24.8/40.67, 24.9/40.54 | +9%, -3.6 ms | 5.4% (breaks cut-outs) | phase 2 budget is ~3.6 ms now (was 6.4 before no-discard) |
| **Plain multiply, both stages** (`REGPU_HYPER_PLAIN_MUL=2`: `Mul9` becomes `a * b`, new) | 23.2/43.95, 23.4/45.30 | 30.8/32.83, 30.8/32.58 | **+32% fps, -12 ms GPU** | scene identical; **HUD timer text broken** (stray diagonal, outlined digits), thin line under the spoiler | not keepable as is: glyph quads rely on 0 * inf = 0 |
| Plain multiply, pixel shaders (`=1`), first pair | 23.4/43.73, 23.2/45.26 | 21.9/44.48 (warming), 27.1/37.40 | unclear | 1.5% | re-run |
| Plain multiply, NaN-safe (`=3`: `m = a * b; m == m ? m : 0`, both stages) | 23.2/43.57, 23.4/43.72 | 19.9/47.25 (warming), 23.6/43.39 | none | exact | **[NO GAIN]** the per-component select itself is the cost |
| Plain multiply, pixel shaders only (`=1`, warm re-run) | 23.3/43.82, 23.4/43.61 | 27.3/37.38, 27.1/37.49 | +16.6%, -6.3 ms | HUD timer broken, spoiler line (same as `=2`) | the breakage is in the pixel stage; to be made safe |
| **Plain multiply, vertex shaders only** (`=4`) | 23.2/43.77, 22.9/44.06 | 25.8/38.94, 25.9/38.96 | **+12% fps, -5.0 ms GPU** | identical (1.21%, motion; timer and spoiler checked zoomed) | **[PROVEN ON INTEL] KEPT** in the Intel stack. Caveat: only the parked race scene checked; menus and other tracks to verify before it goes into a preset |
| **Smart multiply** (`REGPU_HYPER_PLAIN_MUL=5`, new in `hlsl.cpp`: vertex shaders plain as `=4`; pixel shaders track per register whether it may hold an infinity or NaN, from exp2 / raw log2 / raw rcp / raw rsqrt and anything computed from them, sticky; only multiplies and dots with a tainted operand keep `Mul9`/`Dot9`, the rest use `MulP`/`DotP`) | vs `=4`: 25.8/38.97, 25.8/40.38 | 27.6/37.85, 27.4/35.47 | **+6.6% fps, ~-3 ms GPU** | identical (1.17%; timer, Lap, spoiler, LAPS panel and tachometer checked zoomed) | **[PROVEN ON INTEL] KEPT**, replaces `=4`. Recovers about half of the pixel-only plain multiply's +16.6% without its HUD breakage. Changing the preamble recompiles every shader once (~22 min cold here) |
| Alpha-test variants (`REGPU_HYPER_ALPHAFUNC=1`: alpha-tested draws use a PS with the run-time switch replaced by their one comparison; flags bits 26-28; new) | 27.8/37.50, 27.8/37.52 | 27.7/37.49, 27.8/37.59 | none | identical | **[NO GAIN ON INTEL]** reverted. The uniform switch is cheap; the cost of the alpha test is the discard itself |

**Phase 2 closed (9 Nov).** Kept: plain multiply in vertex shaders, smart multiply in pixel shaders. Climb 22.15 to 27.5 fps median.

## Phase 3: texture fetch specialised per draw (9 Nov 2026)

| Experiment | base median / GPU | test median / GPU | Change | Picture | Flag |
|---|---|---|---|---|---|
| Ceiling: stripped texture fetch on the current stack (`REGPU_MEASURE_FAST_TFETCH=1`, warm run) | 27.6/36.37 | 30.9/32.55 | +12%, -3.8 ms | 8.2% (breaks) | phase 3 budget |
| Ceiling: zeroed texel offsets (`REGPU_MEASURE_NO_TEXEL_OFFSET=1`) | 27.8/37.49 | 27.8/37.52 | none | 7.3% | offsets are not the cost |
| **Fetch specialisation** (`REGPU_HYPER_FETCHSPEC=1`, new: `FetchSpecShader` builds a PS variant per draw with each used slot's mode bits (`FetchModeBits`, factored out of the constant upload) and cube-ness as constants (`SpecX`/`SpecCube` switch on the literal slot), so fxc level 1 folds the helper's checks; pipeline key salted with the signature, flags bit 29, kept out of the manifest; the draw uses the generic shader until its variant is ready) | 27.9/37.53, 27.9/37.51 | 33.2/30.50, 33.5/30.01 | **+19.5% fps, -7.3 ms GPU** | identical (1.23%; HUD, car, pit building, tachometer zoomed) | **[PROVEN ON INTEL] KEPT**. Beats the stripped-fetch ceiling: the constants let fxc fold more than the checks. 74 variants compiled in the parked race; first sight of a new mode combination compiles in the background |

### Intel ceilings on the phase-3 stack (batch i8, base 33.6 fps / 30.85 ms GPU)
| Measure | Median fps | GPU ms | Budget |
|---|---|---|---|
| Cars skipped | 39.9 | 25.2 | 5.6 ms (phase 4) |
| All blended skipped | 47.1 | 21.4 | 9.5 ms (phase 4) |
| Helper passes skipped | 39.5 | 26.3 | 4.6 ms (phase 5) |
| Empty draws (CPU ceiling) | 56.9 | 5.4 | CPU caps at ~57 fps (phase 6) |
intel_ab.ps1 now splits -Base/-Test only on commas followed by NAME=, so values with commas work.

### Standing rule (2026-11-09): we optimise ReGPU, not FM2
FM2 is the test bench only. Every kept cut must trigger on generic signals (shader analysis, draw state, surface size or coverage), never on FM2 content: no car detection, no FM2 surface sizes, hashes or constants. FM2-specific values are allowed in measurement switches only. Each kept entry states why it is generic.

## Phase 5: helper passes (Intel)
| Experiment | Base | Test | Result | Verdict |
|---|---|---|---|---|
| Half-rate helpers (`REGPU_HYPER_HALFRATE=1`: draws and resolves into surfaces under half the screen width skipped on odd frames; patch `experiments/intel_halfrate.diff`) | median 33.3/33.3, GPU 31.14/31.05 | median 36.2/36.2, GPU 27.75/27.82 | +8.7% fps, -3.3 ms | **[REJECTED: breaks picture]** reverted. 31.6% of pixels changed: car reflection black, the car shadow turns to garbage, exposure drifts. Skipped frames do not keep last frame's helper images (most likely the title alternates resolve targets, so the skipped one is stale or holds another pass). A fix would cache each helper resolve's output and re-present it on skipped frames. Generic: size rule only |
| Half-rate helpers v2/v3 (clears kept; then size-variant swap kept, only the pixel copy skipped on top of `experiments/intel_halfrate.diff`: in `Resolve`, skip only the `BlitResolve` copy after `EnsureResolved`) | v3 median 31.6/31.6, GPU 31.58/31.34 | v3 median 34.2/35.0, GPU 29.04/28.60 | v2 +7%, v3 +9.5% | **[REJECTED: breaks picture]** reverted. v2 fixed exposure, but the car reflection stays black and the car shadow garbage in v2 and v3; v3 also brought the exposure drift back. Skipping a helper resolve generically is not safe: other passes depend on helper images being refreshed every frame. Phase 5 closed: helpers already at 25% size, so their remaining cost is geometry; no further generic lever found. (Smart App Control went to enforce mid-run and blocked builds; the user turned it off.) |

## Phase 4: cheaper blended shaders (Intel)
| Experiment | Base | Test | Result | Verdict |
|---|---|---|---|---|
| Bool specialisation (`REGPU_HYPER_BOOLSPEC=1`: each `BoolConstant(pb, N)` the pixel shader tests baked as a literal into the fetch-spec variant; patch `experiments/boolspec_patch.py`) | median 32.9/32.8, GPU 30.73/30.57 | median 33.5/33.0, GPU 31.40/30.37 | none | **[REJECTED: no gain]** reverted. Picture identical (1.17%, the usual HUD timer). Static bool branches are uniform and already cheap on the HD 630. Generic: Xenos static branching |
| Swizzle by selects (`REGPU_HYPER_SWIZZLE_SEL=1`: hlsl.cpp `TranslateShader` rewrites the preamble's fetch swizzle loop, `v[sel]` with a dynamic index, into a select chain) | median 32.8/32.7, GPU 29.70/30.74 | median 36.6/37.3, GPU 26.98/26.85 | **+12.8% fps, -3.3 ms GPU** | **[KEPT ON INTEL]** picture identical (1.15%, HUD timer). Lossless: the same channels picked. Intel's compiler spilled the dynamically indexed float4. Generic: shader translator, every game. (A first run was dirty: loading-screen warm-up and a compiling test run.) |

## Laptop (AMD) thread merged into the Intel tree (9 Nov) — retests on Intel, batch m2/m3
Merge details and the per-cut table: `INTEL-PATH.md` (section "Merge of the laptop (AMD) thread"). Kept: draw culling by pixel count with helper/shadow passes plus frozen helper passes, together +7.2% (37.55 → 40.25 median, −0.85 ms GPU). Rejected on Intel: lean vertex fetch, half-precision temps (no gain), A2M test (+17.6% but removes the crowd, canopy fabric and fences). Pictures: `out/m2_*_compare.png`, `out/m3_combo_compare.png`.

## Phase 4 (continued, after the merge)
| Experiment | Base | Test | Result | Verdict |
|---|---|---|---|---|
| Lean LOD (`REGPU_HYPER_LEAN_LOD=1`: hlsl.cpp `TranslateShader` drops the `pforceLod` test from the fetch helper when the `FM2_FORCE_LOD` diagnostic is off) | median 39.3/39.5/38.7/39.7/39.7, GPU 24.6-25.5 | median 39.4/40.8/40.8/41.0/40.7, GPU 23.8-24.6 | **+3.6% fps (last 3 pairs), -1.1 ms GPU** | **[KEPT ON INTEL]** lossless, picture identical (1.30%). Generic: translator |
| Ceilings on the post-merge stack (batch c2, base 41.1 fps / 24.5 ms) | | | blended draws 7.6 ms, cars 4.3 ms, empty draws 76.3 fps (CPU cap up from 57: culling cut CPU work) | phase 6 (CPU) has no payoff while GPU-bound at ~41 fps: closed by measurement. `REGPU_MEASURE_FLAT_MAIN` showed no change: it does not reach the fetch-spec variants (not a real zero) |
| Cube-map bias (`REGPU_HYPER_CUBE_BIAS=2`: cube fetches 2 mips softer) | median 41.0/40.5, GPU 25.42/24.59 | median 39.1/40.8, GPU 23.27/24.49 | none | **[REJECTED: no gain]** reverted (patch: the 10-line block in `TranslateShader`, see this row). Cube maps likely have no mips |

## Phase 7: Intel preset and GPU auto-detect (10 Nov)
- `hyper.cpp` `HyperConfigure`: `hyper=auto` / `REGPU_HYPER=auto` reads the first hardware DXGI adapter; integrated (<= 512 MB dedicated) → Potato, else off. Off stays the default (Hyper off = unchanged).
- Potato on Intel (vendor 0x8086) defaults the Intel thread's measured set: SHADER_OPT=1, VCACHE=2, PLAIN_MUL=5, FETCHSPEC=1, SWIZZLE_SEL=1, LEAN_LOD=1, CULL_PASSES=1, FREEZE=120, FREEZE_REFRESH=3, LEAN_FETCH=0, LEAN_VFETCH=0 (+ the laptop's Potato getters: CULL_PIXELS=64, CULL_ALL, DECIMATE=10, HELPERS=25, ANISO=0; NODISCARD and RESOLVE1010 in every preset). Other vendors keep the laptop's measured Potato. Defaults only: environment and settings win.
- `settings.cpp`: any `hyper_<name>=` key sets `REGPU_HYPER_<NAME>` (every switch has a settings key); Potato turns the upscaler off at native unless asked for.
- Check (batch p7, interleaved): stock 14.9 fps / 54.9 ms; explicit stack 40.6 / 39.5; **potato 41.4 / 41.2; auto 41.4 / 41.2** (GPU 23.6-25.3 ms). Auto log: "auto picked potato (GPU vendor 8086, integrated)". **Stock → one switch: +177% (2.77x).** Stock run 1 failed three times (crash or timeout); only stock run 2 completed.

## Climb 2 — wave 0: measurements (10 Nov, `REGPU_HYPER=auto`, base 42.1 / 42.2 fps, GPU 24.0 ms)
| Measurement | Result | Consequence |
|---|---|---|
| M5 vertex-only (`REGPU_MEASURE_NO_PIXELS=1`) | 78.2 fps, GPU 8.1 ms | **pixel work ≈ 16 ms of 24**: the frame is pixel-bound; vertex + fixed work ≈ 8 ms |
| M9 borderless fullscreen (`FM2_FULLSCREEN=1`) | 42.0 fps, 23.98 ms | no gain: F1 closed |
| M2 per pass (`FM2_GPU_PASSES=1`) | main scene ≈ 8.9 ms (1086 draws, labelled after a 560-wide resolve), 640-wide depth pass 5.3 ms (533 draws), resolve blits 1.47 ms, clears 0.43 ms | E1 (copy instead of blit) target ≈ 1.5 ms |
| M3 per shader (`FM2_GPU_SHADERS=1`) | spread out: top PS 7B62E0F2 1.67 ms (7 draws), 79A94392 1.61 (33), car paint 51932B68 1.35 (64), full-screen 3CDC2B12 1.26 (2), F698E571 0.97 (102) | no single hot shader: translator-wide cuts (tracks A, B) are the right lever |
| M1 pixel runs | 7.51 M per frame, 8.15 per output pixel (stock 10.6) | |

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

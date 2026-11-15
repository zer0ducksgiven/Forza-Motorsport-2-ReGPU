# Climb 2: the experiment catalogue (Intel thread, written 10 Nov 2026)

Start: `REGPU_HYPER=auto` on the i3-7100T / HD 630 at 720p native = **41.3 fps median, 24.5 ms GPU** (stock 14.9).
Target: 60 fps = 16.7 ms GPU, so **about 8 ms more**. The CPU allows 76 fps today.

Rules unchanged: ReGPU only, generic (no FM2 content), degrade never remove, every cut behind a `REGPU_HYPER_*`
switch with a settings key, two parked A/B pairs plus a picture check, no gain = revert and keep the patch, log
every run in `HYPER-CHANGES.md` and `INTEL-PATH.md`. Kept cuts join the Intel potato set.

## What the frame looks like now (measured)

| Fact | Value | Source |
|---|---|---|
| Pixel-shader runs per frame | **7.51 M = 8.15 per output pixel** (stock 10.6) | `gpu stats`, run p7-auto-2 |
| Vertex-shader runs / triangles drawn | 453 k / 172 k (stock 981 k / 465 k) | same |
| GPU time per pixel-shader run | about 3.3 ns | 24.5 ms / 7.51 M |
| Blended draws | 7.6 ms (cars 4.3 ms) | ceiling c2 |
| Floor with no geometry (resolves, clears, copies) | 5.4 ms | ceiling c2, empty draws |
| CPU-only ceiling | 76 fps | ceiling c2 |
| Main colour / depth surfaces | RGBA16F / D32F+S8 (64 + 64 bits per pixel) | renderer.cpp |
| Depth-only draws | still bind a pixel shader and a colour target | renderer.cpp pipeline setup |
| Bench window | windowed (no `FM2_FULLSCREEN`) | hyper_bench.sh |

The headline: **geometry is now small; the frame is 7.5 million pixel-shader runs.** Every experiment below is
judged by what it does to the count of runs, or to the cost of one run.

Columns: **Type** L = lossless, D = degrade. **Effort** S = under an hour, M = a few hours, L = a day or more.
**Odds** = my estimate that it is worth 0.5 ms or more here.

## Wave 0: measurements (no code kept; they aim everything else)

| ID | Measurement | What it decides |
|---|---|---|
| M1 | Pixel-shader runs **per pass** (extend `gpu stats` to the pass split of `FM2_GPU_PASSES`) | Where the 8.15 runs per pixel are: main view, shadow maps, helpers, screen passes. Aims tracks C, D, E |
| M2 | GPU time per pass on today's stack (`FM2_GPU_PASSES=1`) | Splits the 5.4 ms floor and the helper cost |
| M3 | GPU time per shader (`FM2_GPU_SHADERS=1`, and `fm2ctl` `ps HASH tint` for the top ones) | The top 20 pixel and vertex shaders by cost. Aims tracks A and B |
| M4 | Compiled-shader census: instructions, dynamic branches, `Mul9` left, fetches, for the top 20 (`D3DDisassemble`) | Shows which translator cuts are left; rerun after each kept cut |
| M5 | Vertex-only cost (`REGPU_MEASURE_NO_PIXELS`) | Size of everything vertex-side (track C) |
| M6 | Resolution curve: scene scale 100 / 85 / 75 / 50%, measurement only | How pixel-bound the frame is; what track G could give |
| M7 | Zero-weight census: for the top shaders, which multiply-operand constants are exactly 0 at draw time | Go / no-go for B9 |
| M8 | Shared power budget: GPU frequency and package power during a run; rerun with CPU workers capped | Whether CPU load steals the iGPU's power on this 35 W chip |
| M9 | Present path: windowed against borderless fullscreen, and whether Windows composes the frame | Go / no-go for F1 |
| M10 | Driving run, two other tracks, menus: the stack's gain and the number of variants compiled | Protects every result so far; all benches are parked on one track |

## Track A: the cost of one texture fetch (translator, lossless)

Every earlier cut here paid: fetch specialisation +19.5%, swizzle +12.8%, lean LOD +3.6%.

| ID | Experiment | Why it could pay | Effort | Odds |
|---|---|---|---|---|
| A1 | Bake the swizzle word (`tinfo.w`) into the per-draw variant | Ordinary textures skip the swizzle and its branch entirely; resolved ones get a fixed swizzle | S | High |
| A2 | Plain `Sample` when the draw's LOD bias is 0 (baked "bias is zero") | `SampleBias` is a heavier sampler message than `Sample` on Intel | S | Medium-high |
| A3 | Texel offset and size: skip `uv += texel / size` when the offset is a literal zero; pass the reciprocal size instead of dividing | An int-to-float convert and a divide per fetch (`REGPU_MEASURE_NO_TEXEL_OFFSET` is the ceiling) | S | Medium |
| A4 | Bake the sub-image scale test (`tbias.z > 0`) | One dynamic test per fetch | S | Low-medium |
| A5 | Check in the disassembly that sampler choice and cube choice really fold to constants; fix if not | Fetch spec assumes the compiler folds them | S | Medium |
| A6 | Static texture table per draw instead of a bindless index per fetch | Dynamic resource indexing has a cost per sample in Intel's driver | L | Low-medium |
| A7 | Fetch specialisation for vertex-stage fetches and cube fetches (the laptop's runner-up) | They still take the generic path | M | Low |

## Track B: the cost of the arithmetic (translator)

| ID | Experiment | Why it could pay | Type | Effort | Odds |
|---|---|---|---|---|---|
| B1 | Colour-export tail: `oC *= exp2(float(pinfo.w))` runs per pixel in every shader. Bake the scale and the two range clamps into the variant; drop them when unused | An `exp2` and two tests per pixel, in all 7.5 M runs | L | S | **High** |
| B2 | Bake the generated-parameter switch (16 cases on `pinfo.y`) | A 16-way switch per pixel in every shader, almost always "none" | L | S | Medium-high |
| B3 | Compiler flow-control flags: `PREFER_FLOW_CONTROL` against `AVOID_FLOW_CONTROL`, and level 2/3 again on today's leaner shaders | Never tried; the HD 630 runs both sides of many branches anyway | L | S | Medium |
| B4 | Predicated ops as selects (`[flatten]`) for short blocks, `[branch]` for long ones | Predicated Xenos ops become one dynamic branch each | L | M | Medium |
| B5 | Smart multiply v2: taint per component, cleared when a register is fully rewritten with a clean value | Today taint is per register and sticky; M4 counts the `Mul9` left | L | M | Medium |
| B6 | Dead-term elimination from zero weights: a variant keyed on which multiply-operand constants are exactly 0, so the compiler drops the whole term, **its texture fetches included** | Xenos shaders switch features off by a 0 weight (the reason `Mul9` exists). Gated by M7 | L | M-L | Medium, large if it hits |
| B7 | Liveness in the translator: no code for register writes nothing reads | Helps where predicates stop the compiler | L | M | Low-medium |
| B8 | Declare only the registers a shader uses (not 64 zeroed) | Smaller compiler input; may change register allocation | L | S | Low |
| B9 | Real 16-bit floats: compile with dxc to DXIL (`-enable-16bit-types`) | Both threads' half-precision tests were no-ops because old bytecode only carries a hint | D | L | Low-medium |
| B10 | dxc at full optimisation against fxc, same precision | A different optimiser and a different driver front end | L | L (shares B9's work) | Low-medium |
| B11 | Reciprocal and rsqrt forms: drop the clamp or `isinf` fix-up where the operand is provably finite and non-zero | Same analysis as smart multiply | L | M | Low |

## Track C: vertex side and depth-only draws

| ID | Experiment | Why it could pay | Type | Effort | Odds |
|---|---|---|---|---|---|
| C1 | **No pixel shader for depth-only draws** without alpha test (null PS, no colour target) | Shadow maps and depth passes run a pixel shader per pixel for nothing; they may be a large share of the 7.5 M runs (M1 tells) | L | M | **High if M1 confirms** |
| C2 | Position-only vertex shader for those same draws | The compiler drops everything that only feeds the 16 outputs | L | M | Medium |
| C3 | Interpolator trimming: the vertex shader writes only the outputs its pixel shader reads (today always 16 vectors plus one) | 68 floats per vertex through setup and interpolation | L | M | Medium |
| C4 | Bake the index-decode and viewport flags (`vte`, `vinfo.y`) per vertex-shader variant | About 15 dynamic tests per vertex. The laptop's layout baking did nothing here, so modest | L | M | Low-medium |
| C5 | Culling threshold sweep: 64, 128, 256 pixels, and a separate value for helper passes | One number, never tuned on Intel | D | S | Medium |
| C6 | Cull retest interval (`CULL_RETEST` 4 → 8, 16) | Fewer retest frames draw the culled set | D | S | Low-medium |
| C7 | Decimation floor: smaller minimum mesh (300 → 150 indices) | Looser decimation was rejected at 22 fps; cheap to recheck | D | S | Low |

## Track D: overdraw (8.15 runs per output pixel)

| ID | Experiment | Why it could pay | Type | Effort | Odds |
|---|---|---|---|---|---|
| D1 | Front-to-back order for opaque draws inside safe windows (consecutive opaque, depth-tested, depth-writing draws with no blend, stencil or resolve between) | Adds no draws, unlike the pre-pass both threads rejected | L | L | Medium-high if M1 shows main-view overdraw |
| D2 | Cheap first version: only hoist the largest opaque draws (by last frame's pixel count from the culling queries) to the front of their window | Same machinery as D1, a fraction of the sorting | L | M-L | Medium |
| D3 | Selective pre-pass: only for draws with expensive pixel shaders and many pixels, using C1 + C2 | The full pre-pass lost because it doubled every draw | L | M | Medium |
| D4 | Background last: draws at the far plane with no depth write, moved after the opaque set where the state allows | Sky drawn first is shaded then covered | L | part of D1 | Low-medium |
| D5 | Culling by "pixels that survive": extend the query cull to blended draws that end fully covered | Blended draws cost 7.6 ms | D | M | Low-medium |

## Track E: passes, resolves and surfaces (the 5.4 ms floor, helpers)

| ID | Experiment | Why it could pay | Type | Effort | Odds |
|---|---|---|---|---|---|
| E1 | Resolve by GPU copy when size, format and scale match (today always a shader blit) | The laptop's unscheduled runner-up, 0.3–0.5 ms there | L | M | Medium |
| E2 | Dead resolves: skip a resolve whose image nothing samples before it is overwritten (retested every N frames) | Never measured; M2 gives the candidates | L | M | Medium |
| E3 | Fewer mid-frame flushes: one command list submission per frame where nothing reads back | The GPU drains at each flush | L | M | Low-medium |
| E4 | Clear values: create surfaces with the clear colour the title actually uses | Intel's fast clear needs a match; a mismatch is a full draw | L | S | Low-medium |
| E5 | Depth as D24S8, or depth without stencil where the pass has none | Halves depth traffic; Intel keeps stencil in a second surface | L | M | Low-medium |
| E6 | Frozen helpers tuning: refresh 2 / 4 / 6, and `FREEZE_SHADOWS=1` (exists, never tested here) | Frozen helpers gave +5.1% at the laptop's values | D | S | Medium |
| E7 | Scale for big offscreen passes (shadow maps): 50–75% | The helper scale skips surfaces wider than half the screen | D | M | Medium, by M1/M2 |
| E8 | Cheaper path for full-screen passes that read a resolve: check their cost in M3, then apply tracks A and B to them first | Each costs one run per pixel, with many fetches | L | by M3 | Medium |

## Track F: outside the frame

| ID | Experiment | Why it could pay | Type | Effort | Odds |
|---|---|---|---|---|---|
| F1 | Borderless fullscreen with direct flip (no Windows composition) | In a window, Windows composes the frame on the same weak GPU | L | S | Medium |
| F2 | Cap background CPU threads (shader, decimation and hash workers) once the race runs | Gated by M8 | L | S | Low-medium |
| F3 | Upload only the constant ranges a shader uses (today about 12 KB per draw) | CPU and bandwidth; matters more once the GPU is faster | L | M | Low |
| F4 | Share of textures uploaded block-compressed against decoded to RGBA; keep them compressed where possible | Texture cache traffic. Mip drop did nothing, so modest | L | S to check | Low |

## Track G: resolution-class levers (each needs your OK: rule 7 says native)

| ID | Experiment | Why it could pay | Effort | Odds |
|---|---|---|---|---|
| G1 | Horizontal-only scale (for example 1024 × 720 stretched, as many Xbox 360 games shipped) | 20% fewer pixel-shader runs, far less visible than a uniform drop | S-M | High |
| G2 | Dynamic scene scale that holds a target frame rate, with a floor (say 75%) | Native whenever the scene allows | M | High |
| G3 | Transparent layer at half size (blended, depth-tested, no depth write), composited back | The ceiling switch exists (`SKIP_BLENDED=3`) | L | By its ceiling |

## Track H: protecting what we have (no new fps)

| ID | Task |
|---|---|
| H1 | Per-draw variants in the pipeline manifest, so a new session starts at full speed (150 variants compiled in a first run today) |
| H2 | Multiply cuts checked in menus and on other tracks (open from phase 7; part of M10) |
| H3 | Why the stock run failed three times in the phase 7 batch |

## Order of work

1. **Wave 0** first, in one sitting: M1–M5 and M9 are cheap and decide the order of everything else.
2. **Wave 1, the cheap shots** (S effort, run as one batch): B1, B2, A1, A2, A3, A4, B3, C5, C6, E6, F1.
3. **Wave 2, the builds M1–M3 point at:** C1 + C2 (depth-only), B5, B4, C3, E1, E2, E7, then B6 if M7 says go.
4. **Wave 3, the big ones:** D2 then D1 (overdraw), D3, B9 + B10 (dxc), A6.
5. **Track G** only on your word.

Reports come in batches of four or five with a summary table, as before.

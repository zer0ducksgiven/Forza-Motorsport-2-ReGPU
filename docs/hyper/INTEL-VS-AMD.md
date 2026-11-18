# Intel HD 630 vs AMD Vega laptop: the same benchmarks on both (15 Nov 2026)

Intel box: i3-7100T / HD 630 (numbers from `hyper-package.zip`, `repo/out/iq_stats.json`, `iq2_stats.json`).
Laptop: Ryzen 3 5300U / Vega iGPU, branch `hyper-intel` (3af446d), run 15 Nov with `tools/amd_redo_intel.sh`.
Same scripts, same switches, same method on both: native 1280x720, `FM2_ANISOTROPY=16`, upscaler off, parked race,
30 s, 3 interleaved rounds, median fps. Laptop raw lines: `out/bench.txt` labels `iq-`, `iq2-`, `ax-`, `preset-`,
`ah0-`, `ah1-`, `az-`. Laptop medians: `out/amd_*_stats.json`, diffs `out/amd_iq_diffs.json`.

Native: Intel 14.9 fps (GPU 66.8 ms, CPU 60.8 ms). Laptop 42.8 to 43.2 fps (GPU 23.1 to 24.0 ms, CPU 19.6 ms).
Laptop noise: the two native runs differ by up to 2.8% in one group, so anything under 3% is noise.

## 1. Each speed-up alone vs native

"Share" = GPU ms saved as a percentage of that machine's native GPU frame.

| Speed-up | Intel fps gain | Intel GPU ms (share) | Laptop fps gain | Laptop GPU ms (share) | Wins on |
|---|---|---|---|---|---|
| Lean fetch v3 (`LEAN_FETCH=3`) | +49.0% | -25.7 (38%) | +9.0% | -1.75 (7%) | Intel |
| Lean fetch v2 | +26.2% | -16.3 (24%) | +8.4% | -1.88 (8%) | Intel |
| Fetch specialisation (`FETCHSPEC`) | +14.8% | -9.6 (14%) | +7.4% | -1.06 (4%) | Intel |
| Plain multiply (`PLAIN_MUL=5`) | +13.4% | -12.2 (18%) | +8.8% | -1.97 (8%) | Intel |
| No discard (`NODISCARD`) | +12.8% | -6.4 (10%) | +2.1% (noise) | -0.43 | Intel only |
| Frozen helpers (`FREEZE=120`) | +12.8% | -4.6 (7%) | +4.2% | -0.97 (4%) | Intel |
| Swizzle select | +6.0% | -11.3 (17%) | +1.2% (noise) | -0.20 | Intel only |
| Shader optimiser | +1.3% | -6.4 (10%) | +2.6% (noise) | -0.91 | Intel only (GPU ms) |
| Lean LOD | +1.3% | -3.5 (5%) | +1.2% (noise) | -0.29 | Intel only (GPU ms) |
| Helpers at 25% | +18.8% | -10.1 (15%) | +20.9% | -3.96 (17%) | Both, equal share |
| Helpers at 50% | +12.8% | -7.1 (11%) | +17.2% | -3.36 (14%) | Both |
| Cull 256 px + probe + hysteresis (Intel's fix) | +6.7% | -5.1 (8%) | +17.2% | -4.00 (17%) | Laptop |
| Cull 64 px (laptop's version) | -2.0% | +2.2 | +12.3% | -2.68 (11%) | Laptop only |
| Cull 0 px draws | -7.4% | +5.2 | +8.4% | -1.98 (8%) | Laptop only |
| Anisotropy off | +2.7% | -1.5 (2%) | +9.0% | -1.93 (8%) | Laptop |
| Anisotropy 2x | +1.3% | -1.1 | +7.9% | -1.40 (6%) | Laptop |
| Decimation 10% | +2.7% | -1.7 | +4.6% | -1.05 (4%) | Small on both |
| 10:10:10:2 copies | +1.3% | -0.5 | +2.6% (noise) | -0.68 | Neither |
| A2M as alpha test, lean vertex fetch, resolve by size, tail specialisation | within noise | | within noise | | Neither |

## 2. Stacks and presets

| Stack | Intel fps | Intel gain | Laptop fps | Laptop gain |
|---|---|---|---|---|
| Laptop strict Turbo | 18.8 | +26.2% | 52.0 | +20.6% |
| Laptop Turbo (strict + lean v3) | 23.5 | +57.7% | 52.6 | +22.0% |
| Intel Turbo v1 (`iturbo`) | 30.1 | +102.0% | 55.9 | +29.7% |
| Intel Turbo v2 (shader cuts, no lean fetch) | 31.1 | +111.6% | 52.0 | +20.6% |
| Intel Turbo v3 (v2 + lean v3 + lean vfetch) | 34.2 | +132.7% | 53.2 | +23.4% |
| Intel Turbo v4 | 24.9 | +69.4% | 48.7 | +13.0% |
| Mild / Low / Extreme at native res | 18.6 / 19.5 / 19.5 | +25 / +31 / +31% | 48.9 / 51.4 / 53.7 | +14 / +19 / +25% |
| Potato, laptop set | (not run alone) | | 71.5 | +65.7% |
| Potato, Intel set | 46.6 | +212.8% | 77.3 (forced by env) | +79.1% |

**Changed 15 Nov:** the shader-cut set is now Potato's default on AMD too (`hyper.cpp`; `REGPU_HYPER_POTATO_SET=laptop`
brings the older set back). Check, 3 interleaved rounds (`pg-*`): native 42.2, old laptop set 71.3 (+66%), new default
Potato 78.1 (+82%), env-forced set 78.2. Before that change, `REGPU_HYPER=potato` and `auto` on the laptop selected the laptop set (71.7 / 71.8 fps in the preset bench): the Intel
shader cuts only switch on for GPU vendor 0x8086. The Intel set is 8% faster than the laptop set on the laptop too
(GPU 12.0 vs 13.3 ms), so the vendor gate costs the laptop speed.

Preset bench (`tools/preset_bench.sh`; its base does not force 16x anisotropy, so "off" is 0x): laptop off 46.8,
mild 49.5, low 51.7, extreme 53.5, potato 71.7, auto 71.8. Intel: 14.9, 17.9, 18.9, 19.7, 44.3, 43.6.

Pixel difference vs native (mean levels, round 2 screenshot): same order and nearly the same values on both machines.
Laptop noise floor 0.44. Laptop: Intel Turbo v1 0.90, strict 1.48, laptop Turbo 1.67, potato 5.52. Intel: 1.01, 0.90,
1.08, 6.68. One difference: A2M alone is 1.14 on the laptop vs 0.56 on Intel.

## 3. Potato plus upscaling to 1600x900 (Intel set)

| Config | Intel parked / driving | Laptop parked / driving | Laptop 1% low |
|---|---|---|---|
| Stock 720p | 14.9 / - | 43.2 / 42.3 | 18 / 16 |
| Potato 720p | 47.3 / 48.0 | 79.2 / 76.0 | 20 / 17 |
| Potato + FSR 1 to 900p | 36.7 / 38.7 | 66.7 / 65.9 | 15 / 15 |
| Potato + FSR 3 to 900p | 19.3 / 19.3 | 35.0 / 33.6 (see below) | 8 / 5 |
| Full stack + FSR 3 | 18.7 / 19.3 | 34.2 / 34.2 | 9 / 8 |

FSR 1 costs 16 to 22% of the Potato frame rate on both. FSR 3 costs about 57% on both.

**Resolved (15 Nov, later): the "fast FSR 3 state" was not the race.** The runs at 60 to 69 fps (`aw-fsr3`, `ah0-pfsr3-1`,
`az-pfsr3-2`) were stuck on the loading screen: 28 to 169 draws a frame against about 1,400 in the race. The bench only
flagged a stuck run above 250 fps, and with a temporal upscaler the loading screen runs at about 60 fps.
`tools/hyper_bench.sh` now also reruns any run whose last logged frame has under 400 draws. Six clean runs after the fix
(`bb0-pfsr3-1..3` parked, `bb1-pfsr3-1..3` driving): 33.2 / 34.8 / 33.5 and 34.6 / 31.8 / 33.2 fps, GPU 28.5 to 30.4 ms.
So Potato + FSR 3 on the laptop is about 34 fps, as in the table. The 300 s FSR 3 warm-up never reached the race, which
is why first-round FSR 3 runs compiled shaders. Labels `ba0-*`/`ba1-*` are invalid (Intel set not loaded: they are
stock + FSR 3 at 900p, 21 to 25 fps in the race, or stuck).

## 4. What this says about the two machines

1. **The HD 630 is limited by pixel-shader arithmetic; the Vega is not.** Every cut that makes the translated shaders
   do less maths (lean fetch, fetch specialisation, plain multiply, swizzle, no discard) removes 10 to 38% of the
   Intel GPU frame and only 0 to 8% of the laptop's. This is the whole reason Intel gets +213% and the laptop +79% from
   the same set.
2. **Cuts that remove work (fewer or smaller surfaces, fewer draws) are worth the same share of the frame on both.**
   Helper surfaces at 25% save 15% of the GPU frame on Intel and 17% on the laptop.
3. **Draw culling is a laptop win and an Intel risk.** The laptop gains 8 to 17% from all three cull variants. On Intel
   the simple versions cost speed (the pixel-count queries are expensive there) and only the probe + hysteresis version
   helps. That version is also the best one on the laptop (+17.2% vs +12.3% for the laptop's own 64 px cull).
4. **Texture filtering matters on the laptop only.** Capping anisotropy is +8 to 9% on the Vega and +1 to 3% on the HD 630.
5. **Turbo ranks differently.** On Intel the best near-lossless stack is v3 (34.2 fps); on the laptop it is v1 (55.9 fps,
   +29.7%, diff 0.90), which beats the laptop's own Turbo (+22.0%, diff 1.67) on both speed and picture.
6. **CPU time falls with GPU time on both** (the renderer thread waits on the GPU), so neither machine shows a separate
   CPU limit in this scene.
7. **Absolute result:** the HD 630 with Potato (46.6 fps) is faster than the laptop running stock (43 fps).

## 5. Failed or inconclusive

- FSR 3 "two states": explained in section 3 (loading-screen runs counted as race runs). Still open: with FSR 3 the
  race start gets stuck on the loading screen in roughly a third of launches.
- `iw-shaderopt` warm-up gave up after 3 crashes/timeouts; the measured `iq-shaderopt` runs completed (44.6 / 44.2 / 43.5).
- Runs with shader compiles during the race (first round only): `iq2-iturbo3-1` (27), `iq2-iturbo4-1` (19),
  `ah1-p720-1` (22), `ah0-maxfsr3-1` (92), `ah1-pfsr3-1` (24). Medians use all three rounds.
- Labels `ay-*` in `out/bench.txt` are invalid (the FSR variables were empty; they are plain Potato at 720p). Ignore them.
- The 36 to 38 fps native seen in the first smoke test did not recur: 12 native runs today are 42.4 to 44.2 fps,
  matching 9 Nov (42).

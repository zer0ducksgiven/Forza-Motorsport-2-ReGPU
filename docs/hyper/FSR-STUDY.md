# FSR study: 720p upscaled to 900p, Hyper off (Intel HD 630, 11 Nov 2026)

Base: stock ReGPU, `REGPU_HYPER=off`, `FM2_ANISOTROPY=16`. Parked race, 3 interleaved rounds, 30 s, median fps from 10 s in.
Script: `Forza-Motorsport-2-ReGPU/tools/fsr_bench.sh` (labels fs-*), one warm-up run per FSR config first.
Versions in the shipped AMD upscaler DLL (FidelityFX SDK 2.3.0): FSR 3.1.5 and FSR 2.3.4 (no FSR 4).
Test switches added (not Hyper): `FM2_FSR_VERSION=<0 FSR 3.1.5 | 1 FSR 2.3.4>` and `FM2_TEMPORAL_SCALE=<percent>` (fsr3.cpp, settings.cpp).

| Config | Rendered -> output | fps (3 rounds) | Median | vs 720p | GPU ms | 1% low |
|---|---|---|---|---|---|---|
| Native 720p | 1280x720 -> 1280x720 | 14.9 / 15.0 / 15.0 | 15.0 | — | 66.9 | 7.9 |
| FSR 1 | 1280x720 -> 1600x900 | 13.7 / 13.6 / 13.6 | 13.6 | −9% | 74.1 | 5.2 |
| Native 900p | 1600x900 -> 1600x900 | 11.8 / 11.9 / 11.9 | 11.9 | −21% | 84.3 | 6.2 |
| FSR 2.3.4 | 1280x720 -> 1600x900 | 11.2 / 11.1 / 11.3 | 11.2 | −25% | 90.1 | 3.7 |
| FSR 3.1.5 | 1280x720 -> 1600x900 | 10.9 / 10.9 / 10.9 | 10.9 | −27% | 95.3 | 3.4 |

Screenshots: out/bench_fs-<name>-2.png; fence and crowd crops: out/fs_crops.png.

## Where FSR 2/3's cost goes (FM2_GPU_PASSES profile, one ~60 s run each)
Timestamps were added around the temporal upscaler dispatch (renderer.cpp, "temporal upscaler" pass marks).
Per-pass sums are approximate (the GPU overlaps work between marks), but the dispatch's own cost is clear:

| Config | GPU ms per frame (bench) | Temporal upscaler dispatch alone | Biggest scene pass (main view, w640) |
|---|---|---|---|
| Native 720p | 56.1 | — | 35.1 |
| Native 900p | 84.2 | — | 44.1 |
| FSR 1 | 75.6 | — (EASU + RCAS not marked separately) | 28.3 |
| FSR 2.3.4 | 90.7 | **16.5 ms** | 24.7 |
| FSR 3.1.5 | 89.5 | **18.9 ms** | 24.1 |
| FSR 3, motion vectors off | 102.0 | 24.9 ms | 33.0 |

- The scene itself costs the same under FSR 2/3 as at native 720p: it is still drawn at 1280x720.
- The temporal upscaler's own dispatch (FSR 2: 16.5 ms, FSR 3: 18.9 ms per frame) is the whole extra cost, and alone it is about as much as rendering real 900p costs over 720p (~17-28 ms).
- Motion vectors are not the problem: the run without them was slower everywhere (every pass up ~35%), so that machine state was off; inconclusive, not repeated.
- FSR 3 costs ~2.4 ms more than FSR 2 for no visible gain parked.

## Decision (user, 2026-11-13)
FSR 1 is the upscaler for 720p -> 900p on this machine (Hyper off): `FM2_RESOLUTION=1600x900 FM2_UPSCALER=fsr1 FM2_FSR_SCALE=80`.
FSR 2.3.4 and 3.1.5 stay available behind `FM2_TEMPORAL_UPSCALERS=1` but are not used: their dispatch alone costs 16.5-18.9 ms a frame on the HD 630.

## Potato + upscaling to 900p (13 Nov) — can it hold 30 fps?
`tools/fsr_potato.sh`: Potato, 16x forced, 3 interleaved rounds after long warm-ups (first try froze: Potato + FSR 2/3 must build a motion version of every per-draw shader, seconds each, plus >20 s driver pipeline builds; once cached, no compiles in the measured runs).

| Config | Rendered -> output | fps (3 rounds) | Median | vs Potato 720p | GPU ms | 1% low |
|---|---|---|---|---|---|---|
| Potato 720p | 720p -> 720p | 48.1 / 47.2 / 47.7 | 47.7 | — | 20.6 | 12.4 |
| Potato + FSR 1 | 720p -> 900p | 37.9 / 38.2 / 38.2 | **38.2** | −20% | 26.8 | 8.8 |
| Potato + FSR 2.3.4 | 720p -> 900p | 32.6 / 32.6 / 32.3 | **32.6** | −32% | 31.6 | 6.9 |
| Potato + FSR 3.1.5 | 720p -> 900p | 32.4 / 32.3 / 32.4 | **32.4** | −32% | 31.6 | 6.6 |

All three hold 30 fps parked. FSR 2/3 cost ~11 ms of GPU per frame here (less than the 17-19 ms without Potato, where the GPU was saturated).
Artifact: FSR 2/3 show a faint ghost of the loading screen over the first seconds of the race (the temporal history is not reset when the race view appears); gone by 40 s. 1% lows drop to 6.6-6.9 fps with FSR 2/3 (8.8 with FSR 1).

## Decision (team, 2026-11-13)
FSR 2 support is dropped. Supported: FSR 1 and FSR 3 (3.1.5).

## Potato + FSR while driving (13 Nov, tools/fsr_potato_drive.sh, auto-driver, 3 rounds)
| Config | fps (3 rounds) | Median | 1% low |
|---|---|---|---|
| Potato 720p | 47.4 / 46.1 / 49.4 | 47.4 | 12.1 |
| Potato + FSR 1 -> 900p | 37.9 / 37.4 / 38.1 | 37.9 | 8.8 |
| Potato + FSR 3 -> 900p | 31.4 / 31.7 / 33.2 | 31.7 | 7.0 |
(FSR 2 also measured, 32.3, but dropped by the team.) Clips: _fsr/drive_*.mp4. Showcase page: https://claude.ai/artifact/JvEUZ4z5xvkRTWrWHwdm8T

## 14 Nov: CORRECTION, Potato + FSR 3 speed
Every Potato + FSR 3 measurement before 14 Nov (31-33 fps parked/driving, FSR-STUDY and the FSR showcase page) ran FSR 3 without HDR because RESOLVE1010 kept the scene in R10G10B10A2: edges flickered (6.3% parked). Fix: no 10:10:10 resolve while a temporal upscaler is set (TemporalUpscalerSet(), by setting). Correct HDR numbers, median of 3, parked / driving / 1% low: Potato + FSR 3 19.3 / 19.3 / ~5.5; full stack (+ frozen shadows) + FSR 3 18.7 / 19.3 / ~5.3; Potato + FSR 1 36.7 / 38.7 / ~9; Potato 720p 47.3 / 48.0. Parked flicker of the fixed full stack + FSR 3: 0.012%. One run hung on the loading screen (169 draws) and is excluded.

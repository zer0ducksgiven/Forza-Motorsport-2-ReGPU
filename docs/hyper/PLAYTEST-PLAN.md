# Playtest plan (HD 630, draft 14 Nov)

Starts after the round-6 review. FSR 3 does not reach 30 fps on the HD 630 (about 19).

## Builds under test
| Mode | Switches | Measured (parked / driving fps) |
|---|---|---|
| Native | Hyper off, 16x filtering, 720p | 14.9 / — |
| Potato 720p | `REGPU_HYPER=potato` (cull 128) | 46.7 / 48.0 |
| Potato + FSR 1 900p | + `upscaler=fsr1`, render 80% of 1600x900 | 36.7 / 38.7 |
| Full stack + FSR 3 900p | Potato + frozen shadows + FSR 3 (HDR) | 18.7 / 19.3, flicker fixed (0.012%) |

## What each session covers
1. Full race, several tracks (not only Nissan Speedway), day and dusk, with traffic: look for vanishing cars, flicker, shimmer, ghosting.
2. Menus, replays, photo mode, garage: modes the bench never touched.
3. First launch on a clean shader cache: how long compiles freeze the game (FSR 3 needed a 15-min warm-up).
4. 30-minute stint: frame time spikes, memory growth, crashes.
5. Wheel or pad: input latency feel at ~19 fps (FSR 3), ~38 fps (FSR 1) vs ~48 fps (Potato).

## How to report
Per session: mode, track, what you saw, timestamp; a screenshot or a short capture for any visual problem. Logs: `fm2.exe` stderr (bench tools write `out/*.err`).

## Known before playtest
- Loading-screen ghost at race start with Potato + FSR.
- FSR 3 1% lows about 5.5 fps (vs 12 at Potato 720p).
- FSR 3 is still marked unfinished in ReGPU.
- The presets page still uses the old 0x-filtering stock baseline.

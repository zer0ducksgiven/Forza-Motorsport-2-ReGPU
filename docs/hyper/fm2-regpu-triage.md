# FM2 → ReGPU commit triage

FM2 renderer commits (`src/gpu`, `src/render`) made after ReGPU was split off. The split is ReGPU's first
commit, "Import the renderer from fm2-port, unmodified", taken at FM2 `ff5a229` (0.5.2, 22 Oct 2026).
103 commits up to FM2 `f4f941c` (5 Nov).

**Method.** For each commit, its added code lines (25+ characters, comments left out, `FM2_` read as
`REGPU_`) were looked up in ReGPU `main` and in PGR4's copy (`thirdparty/ReGPU`). Low scores were then
checked by hand, because ReGPU often makes the same fix with different code. "Match" is the share of a
commit's lines found word for word in ReGPU. PGR4's copy gave the same result as ReGPU except where noted.

## Summary

| Bucket | Commits | What to do |
|---|---|---|
| A. Already in ReGPU | 43 | Nothing. Check while proving parity |
| B. General, missing from ReGPU | 33 | Port into ReGPU as console-behaviour or performance changes, each behind a `REGPU_*` switch |
| C. FM2-only | 11 | Stay in FM2, through ReGPU hooks; 2 need a new general hook |
| D. No code to move | 16 | Reverted pairs, comments, seed refreshes, version bumps, merges |

## Highest value for us (a Radeon iGPU laptop)

1. **`1146bcb` Shaders: the register file as 64 variables, not an indexable array.** fxc declares
   `float4 r[64]` as an indexable temp. An AMD driver can keep that in scratch memory: a Ryzen 5800H's
   Radeon ran a 720p race at 29 fps, GPU-bound. Not in ReGPU or PGR4. ReGPU split out only the colour
   exports (`oC[4]`) this way. This is likely our biggest single gain.
2. **`a3e5496` Shaders: forward-only control flow runs once, not in a `[loop]`.** ReGPU still wraps every
   shader in `for (guard < 1024)`. That costs GPU time on every shader without loops.
3. **Vertex arena series** (`a6edeb7` `0c9a747` `620a77c` `8bbd2a4` `816898e` `d430385` `f893212`). Large
   vertex buffers kept on the GPU, with changes found by write-watching pages. This is for PCs where
   zero-copy memory isn't available. Whether this laptop needs it depends on our driver: check the log
   once FM2 runs.
4. **`00253c9` Draw constants built in ordinary memory, copied in sequential blocks.** An FX-8350 spent
   6–35 ms a frame writing them scattered into write-combined memory. A slow CPU gains.
5. **`7e93a2f` Texture cache capped at 1 GB.** PGR4's copy has its own video-memory budget eviction.
   Merge the two into one approach, since we have 7.3 GB of RAM shared with the GPU.

## Measured on our laptop (5 Nov 2026)

On the Ryzen 3 5300U's Vega iGPU (uncapped `--racenow` race at Nissan Speedway, `tools/hyper_bench.sh`):
- **`1146bcb` (register file as variables):** about +15% fps (67 to ~79), GPU time 18.6 to 14.8 ms per frame. Scott only tested it on an RTX 4070, where it changed nothing.
- **`a3e5496` (forward-only control flow):** about -1 ms of GPU time per frame (~7%); the fps change is within noise.

Neither is in ReGPU or PGR4's copy. Raise both with Scott first.

## A. Already in ReGPU (43)

Matched 60%+ or confirmed by hand. ✓ = hand-checked: different code, same fix.

| Commit | Match | Subject |
|---|---|---|
| 28020f0 | 96% | Don't translate microcode that cannot be a shader |
| 0052c9b | 83% | Explicit LOD in vertex shaders |
| 4936dbb | 75% | Loops read the loop constant registers |
| fff9133 | 96% | Guest memory heap startup self-test |
| bcc3eff | 100% | Capture traces a whole frame |
| bb3d6cd | 88% | Textures hashed ahead on workers, constants by mask |
| 06060df | 100% | GPU time per shader pair |
| 66f0605 | 62% | Pipeline cache names include compiled code |
| 4638234 | 66% | Baked vertex fetch by default |
| 36f4df5 | 100% | Command processor: padding NOPs in one load |
| 16d1d43 | 88% | Resolve stale-check every 8th frame; one submission |
| 044501f | 100% | Trace-frame check without a static guard |
| bac3fd4 | 72% | Per-draw switches at namespace scope |
| d542b82 | 73% | Textures due next frame hashed ahead |
| 1c3ed90 | 100% | Resolve baseline skipped for recent resolves |
| 33d2682 | 83% | Repeat shader loads reuse the upload record |
| b5df266 | 69% | FSR 1 on the whole frame, render scale |
| be9e778 | 91% | Upload ring grows; changing vertex buffers copied |
| b9d8e93 | 100% | FM4 merge: winding, stencil faces, mip chains, DRED |
| 00066fa | 89% | FM4 merge: shader and resolve fixes |
| acd2b1d | 62% | Pipeline seed list mechanism (the list itself is FM2's) |
| b866298 | 75% | Pipeline names include fixed-function state |
| 703b5c9 | 91% | Index check hashes four lanes at a time |
| a4564cc | 100% | Pipeline library compacted |
| dd624e1 | 100% | Shader scan below the game's threads |
| 99724e6 | 100% | Mirrored axis clamped after the fold |
| 113e0b0 | 92% | Leaner ALU code |
| f61653e | 88% | Screen passes wait for their pipeline |
| 40bec93 | ✓ | Packed mip tail (`PackedMipOffset`) |
| 2335e43 | ✓ | Index buffers checked in full |
| 7c3149e | ✓ | 4444 has R in the low nibble (`REGPU_4444_R_HIGH`) |
| 6b3aa22 | ✓ | Mirrored address modes |
| d168e1d | ✓ | Polygon offset slope term |
| b0a5b37 | ✓ | D3D9 half-pixel centres, polygon offset |
| 573c8f1 | ✓ | NGX initialised at device creation |
| 67c90e2 | ✓ | Upscale after the last tile's resolves |
| 6d66163 | ✓ | Frame time and slowest 1% in the log |
| ca9ba8d | ✓ | Upload ring 128 MB when memory is mapped |
| ce5bf5a | ✓ | Guest samplers in a descriptor table |
| 84b451b | ✓ | Bloom reads images averaged to game size |
| 0c09b18 | ✓ | Depth resolves read as 8888 give D24S8 words |
| 1428224 | ✓ | Anisotropy setting |
| bd4438a | 50% | Zero-copy on Windows 10: ID3D12Device3 fallback (in ReGPU's README) |

## B. General, missing from ReGPU (33)

| Commit | Match | Subject | Note |
|---|---|---|---|
| 1146bcb | 2% | Register file as 64 variables | **AMD performance**, confirmed missing |
| a3e5496 | 25% | Forward-only control flow without a loop | **GPU performance**, confirmed missing |
| 00253c9 | 0% | Draw constants in ordinary memory | CPU performance |
| e8d6f83 | 20% | Vertex fetch table not cleared each draw | CPU performance |
| 6bdf39a | 30% | Per-draw constant copy trimmed | Partly ReGPU's own 4884e92: diff both |
| b4ed2e1 | 0% | Float constants straight into the ring | From ReGPU 2fd47c3, then changed: diff both |
| a6edeb7 | 37% | Vertex arena | Performance where memory isn't mapped |
| 0c9a747 | 9% | Vertex arena: write-watch instead of hashing | Needs the host's write-watch (FM2 `memory.h`): new `Host` callbacks |
| 620a77c | 42% | Vertex arena: indexed draws stay on GPU | |
| 8bbd2a4 | 0% | Write watch reported on slow frames | |
| 816898e | 0% | Vertex arena on by default | |
| d430385 | 7% | Arena: unwatchable memory is hashed | |
| f893212 | 0% | Arena off where it can't watch | |
| 7e93a2f | 6% | Texture cache capped at 1 GB | Reconcile with PGR4's budget eviction |
| 5024dd7 | 0% | Low performance mode | Setting already exists in FM2's launcher |
| 8a854bd | 15% | Compile every shader; translate loops and calls | Check: ReGPU translates loops differently |
| 83be568 | 0% | Pixel shaders mipmap again (vertex stage only) | Accuracy |
| 3cfb37c | 0% | `FORCE_LOD` absolute mip (RADV) | Driver workaround switch |
| ab7933d | 42% | Depth-only: no alpha test when the VS writes no interpolator | ReGPU has `REGPU_NO_KILL_SKIP`: may be the same, diff |
| bf58ecd | 0% | Depth-only: alpha-test only a textured alpha | |
| e641bfc | 0% | Bloom game size: one cache slot per base | Fixes frame-time spikes |
| fb43dc0 | 0% | Bloom: fractional scales averaged by area | |
| 4841af5 | 33% | Menu videos: no mixed frames | Video texture handling |
| 480e94a | 0% | Render thread's fence wait sleeps | FM2's title wait address: needs a hook (the game's wait loop) |
| 3ab28c1 | 0% | System poll at boot | Diagnostics |
| ee34757 | 0% | PERF times MSAA scale and index lookup | Diagnostics |
| 3ba766b | 0% | Benchmark: compile counts | Diagnostics |
| 905ddaa | 0% | `TRACE_DIRECT` in-place vertex buffers | Diagnostics |
| 558f803 | 0% | Draw trace: polygon offset | Diagnostics |
| 4a44f65 | 0% | findhex, movable watchpoint | Diagnostics |
| 8190b1d | 0% | Log stencil clears that miss their surface | Diagnostics |
| 9b6b512 | 32% | SMAA 1x | A feature any game could use: belongs in ReGPU as an option |
| 3bad1ef | 33% | Boot screen while stored pipelines compile | From ReGPU ff7b878: diff both |

## C. FM2-only, through hooks (11)

| Commit | Subject | Hook |
|---|---|---|
| 221aafb | Car meshes move with their car (squash) | `rewriteShader` + `onDraw` (vuser constants) |
| 6157467 | DualSense / Xbox Series button icons (`ps_glyphs.h`) | Texture replacement: **new hook** (by texture hash) |
| 2b107d2 | Button icons centred | Same |
| e0dd88a | Keyboard prompt text box; cursor hidden after pad use | **New host hook**: `PresenterAskText`, `PresenterControllerUsed` |
| 555f54d | Ultrawide: menus 16:9, races wide | `SetGameWidescreen` plus a per-frame switch: a small new hook |
| f2e98ed | Ultrawide: race needs 3D scenery | Same |
| f1f4b62 | Ultrawide: car diagram in the 16:9 area | Same |
| e1e5702 | In-race music toast, car sound mix | An overlay drawn on the frame: `onSwap` plus a draw-overlay hook |
| 971d224 | FM2 Live names | Port code |
| 2b70905 | Launcher: hide the shader compile box | `bootScreen` |
| b83a88e | SDL3 racing wheels (presenter) | Port input code |

## D. No code to move (16)

`9a6c99b` + revert `f71d6f8`, `c517786` + revert `47f5e1d`, `7bef791` (comment), pipeline seed
refreshes `f9ee06d` `6fc52fb` `5e23f4c` `f38a475` `33979f4` (the list's contents reach ReGPU through FM2's `staticShaders` hook), version commits `07b8a00` `21be6f1` `8efabe3` `e4f2736`, merges
`9fc3b64` `e2c448f`.

## What this means for the migration

- **The hooks cover most of bucket C.** Four new general hooks are needed: text entry and controller
  state for the presenter, texture replacement, an overlay draw, and a per-frame widescreen toggle. That
  follows ReGPU's rule: "If a fix needs something the hooks can't express, add a general hook."
- **Bucket B is the real work: 33 commits**, most of them small. The vertex arena (7 commits, about 300
  lines) and SMAA (260 lines plus a shader) are the large ones.
- **Do the two AMD shader fixes first** (`1146bcb`, `a3e5496`). They're self-contained in `hlsl.cpp`,
  general, and the most likely to help this laptop. They can go into ReGPU before the rest of the
  migration.
- Commits marked "diff both" came from ReGPU and then changed in FM2. Compare the two versions and keep
  the better one.

Caveats: match scores are an automated measure. "✓" means the fix was found by reading the code, not by
testing. Nothing has been run, because the build needs the disc image.

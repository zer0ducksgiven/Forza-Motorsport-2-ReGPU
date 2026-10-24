# Forza Motorsport 2 ReGPU

A native Windows port of **Forza Motorsport 2** (Xbox 360). Nothing is emulated:

- **CPU:** the game's entire PowerPC executable is statically recompiled to x64 C++
  with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (51,034 functions), running on our own
  implementation of the Xbox 360 kernel and XAM. No interpreter, no JIT.
- **GPU:** a native Direct3D 12 renderer replaces Xenos GPU emulation. The game's
  Xenos shader microcode is translated to HLSL, its render state to D3D12 pipelines,
  EDRAM resolves to GPU copies, and its tiled textures are decoded natively.

No game data is included: you need your own copy of the disc. See
[FM2-native-scope.md](FM2-native-scope.md) for the plan and [TESTING.md](TESTING.md)
for playing it.

For support or to get involved in the development of ReGPU, join our Discord server:
https://discord.gg/pVEpmSk2G3

## Install

The installer contains no game files. You need your own copy of the European disc image and the title update.

1. Download `FM2-Setup-0.3.0.exe` from the [releases page](https://github.com/zer0ducksgiven/Forza-Motorsport-2-ReGPU/releases) and run it. No admin rights are needed.
2. Choose an install folder (default `%LOCALAPPDATA%\FM2`).
3. Choose your European disc image (`.iso`). It is checked against this build.
4. Choose the title update file (named like `TU_16L61VA_...`). It is checked against this build.
5. Optionally, choose a folder with your DLC packages. If you have none, keep the default and this step is skipped.
6. The installer builds the game files from the disc, unpacks the title update into `Documents\fm2\update`, copies any DLC into `Documents\fm2\dlc`, and offers a desktop shortcut.

The installer bundles its own Python. The launcher needs Microsoft's .NET 10 Desktop Runtime; the installer downloads and installs it when it is missing (Windows asks for administrator rights for that step). WebView2, which the launcher draws its window with, is part of Windows 11.

### Building the installer

Needs Inno Setup 6 and a Python 3.12 embeddable package unpacked into `out\pyembed\py`. Then from the repository root:

```
powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1 -Version 0.1.0
```

It stages `installer\payload` from `build\tu` and the scripts, and writes `installer\dist\FM2-Setup-0.3.0.exe`.

## Shaders compiled before you drive, smooth frame times while you do

Shader compilation stutter is the usual weak point of running Xbox 360 games on PC.
Emulators and emulator-based ports translate each shader the first time the game uses
it, so every new car, track and effect hitches the first time it appears, unless you
first play through the game to fill a cache, or run a long "prepare shaders" step.

This port finds every shader up front instead:

- **On the first start**, it scans the game's files (the mesh archives and the
  executable) and finds the shader microcode directly, about 4,800 shaders in
  about 4 seconds. All of them are compiled behind a loading screen built from the game's own art
  (about 3.5 minutes on 16 threads, once). Later starts load the compiled shaders from
  the cache instantly.
- **Copies the game patches at load** (each vertex shader is rewritten for the vertex
  layout of the mesh it draws) share the bytecode of the original: the vertex layout
  is passed to the shader per draw instead of being compiled in.
- **The thread that feeds the GPU never blocks on disk or bookkeeping**: pipeline
  cache saves, cache file writes and shader lookups all happen off it, and draws never
  wait on a scan of guest memory.

Measured on the test machine below, through a full race with every GPU pipeline
built fresh: **no renderer-caused frame-time spikes, 59.9 fps average, 59.1 fps
minimum**, against hundreds of slow frames before this work. Run with `FM2_PERF=1`
and every slow frame is logged with what caused it.

For comparison, the existing emulator-based port
([hl360-Forza2](https://github.com/0herculabs/hl360-Forza2), v0.3.0) runs the Xbox 360 GPU
through a Xenia-derived emulation layer with a shader warm-up step; its own changelog
notes menus locked to half rate and 144 Hz running slower than 60 Hz. Details in
[FM2-native-scope.md](FM2-native-scope.md).

## System requirements

| | Minimum (tested: about 60 fps at 720p) | Recommended (tested) |
|---|---|---|
| OS | Windows 10 64-bit, 22H2 | Windows 11 23H2 or newer |
| CPU | AMD FX-8350 (4 modules / 8 threads, 2012) | AMD Ryzen 7 5800X (8 cores / 16 threads) |
| GPU | NVIDIA GeForce GTX 970 (4 GB), DirectX 12 feature level 12_0 | NVIDIA GeForce RTX 4070 |
| RAM | 8 GB (16 GB tested) | 16 GB or more (64 GB tested) |
| Disk | 6 GB (extracted game ~4.7 GB, plus shader and pipeline caches) | an SSD shortens the first start |

**What the minimum PC gets.** The FX-8350 + GTX 970 PC above (Windows 10 22H2, 1280x720, **Low performance
mode** on) averages **71-74 fps** in the benchmark race, with 1% lows of **49-51 fps**: a steady 60 most of
the time, with dips in the busiest moments. Its first start compiles shaders for a few minutes longer than a
modern PC. Anything slower than this is untested; older CPUs than an FX-8350 are likely below 60.

**Low performance mode** (launcher, Play tab) is for CPUs of that age: on the FX-8350 it was worth about
20% (69.5 vs 57.3 fps). Leave it off on a modern CPU, where the optimisation it turns off is faster.

**Windows 11 users: update to 23H2 or newer** (Settings, Windows Update), with a GPU driver from 2024 or
newer. Windows 10 now gets zero-copy memory too (below), but Windows 11 23H2+ uses the newer, more direct
path, and it's the configuration most of the testing was done on.

### For full performance: zero-copy memory

The renderer lets your GPU read the game's memory directly, with no copying: Direct3D 12 maps the game's
memory as a GPU heap. It uses `ID3D12Device13` where it can (Windows 11 23H2 or newer with a 2024 or
newer GPU driver) and falls back to `ID3D12Device3`, which Windows 10 (1709 or later) also has. Either
way the game tests at start that the GPU really sees the memory. When neither works, the game still
runs but copies its vertex data to the GPU every frame instead: lower frame rates and more hitches,
especially on slower CPUs (here, 150 fps instead of 211 at the same spot).

The launcher shows **Reduced performance** at the top when the game found it couldn't use zero-copy,
and says which Windows version and GPU driver you have. The game's `out\play.log` says it too, near the
start: `renderer: guest memory not mappable; vertex data is copied`. Updating Windows (Settings, Windows
Update) and installing the latest driver from NVIDIA, AMD or Intel is the fix to try first.

## Status

The game boots, plays its intro, and runs the menus, race setup and races natively at
up to 360 fps (menus included) and up to 4K, and experimentally on ultrawide displays, with car and track
rendering, shadows, HDR resolves and the HUD, and full 5.1
audio: the game's sound engine (FMOD) mixes into our render driver, which plays through
XAudio2, and its XMA sound data is decoded with FFmpeg's XMA decoder. Game time follows
the clock, so the game runs at real speed at any frame rate. A race with a full grid runs
at about 140-165 fps at 2560x1440 on a fast desktop (Nurburgring free run 180-240 fps),
where the GPU and the command processor are close to equal limits; menus reach the cap.

Saves work: the signed-in gamer profile is the profile name from the launcher (`Player`
by default), and its saves live in `Documents\fm2\profiles\<name>\content`.

## Reporting bugs and sharing logs

The launcher's **Report a bug** tab sends a report straight to the developers: a title, what happened, an
optional Discord name for a reply, and (on by default, switchable) your last `out\play.log`. The same tab
has an opt-in, **off by default**: **Share play.log after each session**, which sends each session's log
when you next open the launcher, so problems on hardware we don't have get found.

What's in a log: your Windows version, CPU, RAM, GPU and driver, displays, game settings, frame rates
and anything that went wrong. Your **Windows user name and PC name are removed** before anything leaves
your PC (the tab shows the log exactly as it would be sent), and again on the server. Reports go to a
private repository only the developers can read; they're never public. The server
(`server/reports`) is a small Cloudflare Worker, in this repository.

## Launcher and settings

The Start menu and desktop shortcuts open the launcher (`launcher\FM2Launcher.exe`, source in
`launcher/FM2Launcher`: a .NET 10 WinForms window hosting WebView2, its page in `wwwroot`). Hover over any
option for an explanation.

- **Play**: profile name, resolution, aspect ratio, windowed or fullscreen, max FPS, VSync,
  anti-aliasing (Off, 2x, 4x, 8x MSAA), enhanced lighting (experimental), skip intro videos, and the
  game's language (English, German, French, Spanish, Italian, Japanese, Korean, Simplified and Traditional
  Chinese, as on the disc; the disc's Polish, Czech and Hungarian text can't be picked yet). Every change is
  saved to `Documents\fm2\settings.ini` straight away.
- **Settings**: the shader cache (size, delete, rebuild now), the DLC packs (switch one off to keep it out of
  the game, add packages, open the folder), and the install and user folders.
- **Updates**: checks GitHub for the latest release, shows its notes, and downloads and installs its
  `FM2-x.y.z-patch.zip` over the install. Files of the running launcher are renamed to `*.old` and replaced;
  the next start removes them.

`FM2Launcher.exe --play` starts the game with the saved settings without opening the window, and
`--apply <patch.zip>` installs a downloaded patch. `tools/make_patch.py <version>` builds the patch zip
(`fm2.exe`, the launcher published by `installer/build-installer.ps1`, and the changelog).

`fm2.exe` started on its own (`play.cmd`) still shows its built-in settings window first. That window has
the profile name,
resolution, aspect ratio (Auto, 16:9, 21:9, 32:9; the image gets black bars when it
differs from the resolution), windowed or borderless fullscreen, VSync, and max FPS
(30-360). Play saves them to `Documents\fm2\settings.ini`. With VSync on, the cap
is also the display's refresh rate. `FM2_NO_LAUNCHER=1` skips the launcher and uses the
saved settings; these variables override the file for one run: `FM2_PROFILE`,
`FM2_RESOLUTION`, `FM2_RENDER_SCALE`, `FM2_MAX_FPS`, `FM2_VSYNC`, `FM2_FULLSCREEN`.

### Upscaling (removed from the launcher for now)

The upscaling code (AMD FSR 1, FSR 3 and NVIDIA DLSS) is still in the source and its
environment variables still work, but the launcher no longer offers it. It is not part
of 0.1.0's settings.

## Building

Needs Visual Studio 2022 Build Tools, LLVM (clang-cl), CMake, Ninja, Python 3, and an
FFmpeg Windows x64 **LGPL shared** build (for XMA audio decoding), for example
`ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip` from
[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases): extract it so that
`thirdparty/ffmpeg/include`, `lib` and `bin` exist. The build copies the DLLs it needs
next to `fm2.exe`; FFmpeg is linked dynamically and stays under its own LGPL license
(`thirdparty/ffmpeg/LICENSE.txt`).

The upscalers need the AMD FidelityFX SDK and the NVIDIA DLSS SDK in `thirdparty/`
(not committed; FSR 1's shaders are compiled from the FidelityFX headers at build time):

```
git clone --depth 1 --branch v2.3.0 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git thirdparty/fidelityfx
git clone --depth 1 --branch v310.9.16 https://github.com/NVIDIA/DLSS.git thirdparty/dlss
```

```
git submodule update --init --recursive
git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-fm2.patch
py -3 tools/xiso.py extract "<your FM2 disc>.iso" game
tools\vcenv.cmd cmake -S tools/xexdump -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
sh tools/recomp.sh
tools\vcenv.cmd cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link
tools\vcenv.cmd ninja -C build/rel fm2
build\rel\fm2.exe game
```

Supported: European `default.xex`, SHA-256 `9726066f…9b120a`. The recompiled
code (`out/ppc`, ~131 MB) is generated from your own copy and never committed.

## Debugging switches

| Variable | Effect |
|---|---|
| `FM2_WATCHDOG=1` (or a period in ms) | Every 3 s: each guest thread's call stack (recompiled frames are `sub_<guest address>`), what it waits on, GPU state |
| `FM2_GPUTRACE=1` | Log every PM4 packet the command processor runs |
| `FM2_TRACE_OBJ=<hex>` | Log KeSetEvent/KeResetEvent/KeWait on one guest object |
| `FM2_TRACE_SWAP=1` | Log D3D presents and swaps |
| `FM2_WATCH=<hex>` | Log every change of one guest word |
| `FM2_WATCHW=<hex>` or `<ptr>,<offset>` | Hardware write watchpoint on a guest byte; logs the writing guest functions |
| `FM2_TRACE_IMPORTS=<seconds>` | Log kernel/XAM calls made after that time (`FM2_TRACE_LIMIT` per name, default 5) |
| `FM2_TRACE_FILES=1` | Log file opens and misses |
| `FM2_AUTOSTART=1` | Tap Start + A every 3 s |
| `FM2_DUMP_SHADERS=1` | Save every distinct shader's microcode to `Documents/fm2/shaders` |
| `FM2_DUMP_FRAME=<n>` | Write the n-th rendered frame to `frame.png` |
| `FM2_DUMP_AT=<seconds>` | Write the first frame after that long to `frame.png` (comparable screenshots at any frame rate) |
| `FM2_PERF=1` | Once a second: fps, renderer time by section, thread CPU, GPU time; slow frames with their cause (`FM2_SLOW_MS` sets the threshold, default 25) |
| `FM2_GPU_PASSES=1` | GPU time per pass (between render target switches and resolves), every 2 s |
| `FM2_PROFILE=<thread id>` | Sample that guest thread's host instruction pointer and log the top functions every 5 s |
| `FM2_NO_NATIVE_BC`, `FM2_NO_INDEXED`, `FM2_NO_RECORD_THREAD`, `FM2_NO_SLOT_MEMO`, `FM2_COMMITTED_TEXTURES`, `FM2_ALL_CONSTANTS` | Turn off one renderer optimisation (A/B checks) |
| `FM2_RESOLUTION=<w>x<h>` | Override the launcher resolution: render at that size, any aspect (e.g. `3440x1440`). Wider than 16:9 is **experimental**: the 3D view widens (Hor+), the HUD and menus keep 16:9 proportions in the centre, but some objects at the sides still pop in (game-side culling) |
| `FM2_WIDE_FOV=<0-100>` | Ultrawide only: how far the view widens (100 full Hor+, 0 the 16:9 view zoomed to fill) |
| `FM2_RENDER_SCALE=<1-4>` | Render at that multiple of 1280x720 (2 = 2560x1440, 3 = 3840x2160) |
| `FM2_DUMP_AUDIO=<file.wav>` | Write everything the game plays (6-channel float, 48 kHz) to a WAV file |
| `FM2_TRACE_DRAWS=<n>` | Log every draw, skip, resolve, PM4 packet and texture upload of frame n |
| `FM2_DUMP_TEXTURE=<hex address>` | Write that texture, decoded, to `texture.png` |
| `FM2_DUMP_HLSL=1` | Log each translated shader |
| `FM2_TRACE_PRED=1` | Log predicated packets under tiling |
| `FM2_TEXVIEW=1` | Show decoded textures instead of rendering |
| `FM2_HEADLESS=1` | No window |
| `FM2_SKIP_VS=<hash>,...` | With `FM2_DUMP_FRAME=n`: drop those vertex shaders in frame n+1 and write it to `frame2.png` (A/B under the same camera) |
| `FM2_SKIP_DRAWS=a-b` | Drop draws a..b-1 of the dumped frame |
| `FM2_TRACE_VFETCH=1` | Print every translated vertex fetch (format, stride, offset, scale) |
| `FM2_NO_DIRECT_FETCH=1` | Copy all vertex data instead of reading large buffers in place |

`build/rel/xenos_dis.exe <shaders...>` disassembles captured Xenos microcode;
`--hlsl` translates and compiles them instead.

`tools/callgraph.py` indexes the generated code (`build`, `callers sub_X`,
`at <address>`); `build/tools/dis.exe <xex> <address> <count>` disassembles.

## Layout

| Path | Contents |
|---|---|
| `config/` | XenonRecomp config and jump tables for FM2 |
| `patches/` | Our XenonRecomp changes: FM2's extra VMX/PPC instructions, non-trapping divides, MSR[EE] hook |
| `src/` | Runtime: guest memory, loader, kernel/XAM imports, threads, renderer, audio output and XMA decoding |
| `src/hooks/` | Overrides of recompiled functions (`sub_X`): XAPI fibers on host fibers, D3D instrumentation |
| `tools/` | Disc extraction, XEX dump/disassembler, analysis scripts |

## Acknowledgements

The Xbox 360 GPU (Xenos) is undocumented. Our understanding of its hardware comes from the
[Xenia](https://github.com/xenia-project/xenia) project's research, used under its BSD licence:
the GPU register map, command packet layouts, texture formats and tiling, and the shader
microcode format. The renderer itself is our own code, written for this port; Xenia's
documentation of the hardware made that possible.

The game code is recompiled with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

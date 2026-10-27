# Forza Motorsport 2 ReGPU on Linux (Proton)

This is the same `fm2.exe` as the Windows release, run through Proton. Direct3D 12 goes through
vkd3d-proton, and Microsoft's `d3dcompiler_47.dll` (included) replaces Wine's shader compiler,
which can't compile the port's shaders (that was the cause of the black screen in earlier builds).

Nothing from the game is included. You need your own **European** Forza Motorsport 2 disc image
(`.iso`) and its **title update** file (`TU_16L61VA_...`), the same ones the Windows installer asks for.

## Requirements

- Steam with **Proton Experimental** (or Proton 10/11) installed. In Steam: Library, search
  "Proton Experimental", Install.
- `python3` (for the one-time setup).
- A Vulkan GPU driver (Mesa for AMD/Intel, the proprietary driver for NVIDIA).
- For the launcher: WebKitGTK 4.1 and GTK 3 (`libwebkit2gtk-4.1`, on most desktops already), and
  SDL3 (`libSDL3.so.0`) for the Controls tab's live wheel inputs.

## Install

```sh
tar xzf FM2-Linux-<version>.tar.gz
cd FM2-Linux
./setup.sh "/path/to/Forza Motorsport 2.iso" "/path/to/TU_16L61VA_..." [dlc folder]
./launcher/FM2Launcher
```

`./play.sh` still starts the game straight away, without the launcher.

`setup.sh` builds the game files into `game/` and unpacks the title update into the Proton
prefix (`~/.fm2-linux/compatdata`). Saves, settings and the shader cache live there too, in
`pfx/drive_c/users/steamuser/Documents/fm2`.

The first start compiles every shader (a few minutes on a fast CPU). Later starts reuse the cache.

## Launcher

`launcher/FM2Launcher` is the Windows launcher built for Linux: the same pages and code (play,
profiles and cheats, Controls and force feedback, photos, DLC, Forza Plus 2, reports). It runs natively
and starts the game through Proton as `play.sh` does, with the same prefix (`FM2_COMPATDATA`,
`PROTON` and `FM2_LINUX_HOME` work the same). Settings are saved to `settings.ini` in the documents
folder above. Updating from the launcher isn't there yet: unpack the new release over this folder.

## Steam / Steam Deck

Add `fm2.exe` as a non-Steam game, then in its Properties:

- Compatibility: force **Proton Experimental**.
- Launch options: `WINEDLLOVERRIDES="d3dcompiler_47=n" %command% game`

and run `setup.sh` once first (it writes into `~/.fm2-linux`; Steam uses its own prefix for a
non-Steam game, so copy `~/.fm2-linux/compatdata/pfx/drive_c/users/steamuser/Documents/fm2/update`
into that prefix's `Documents/fm2/update`, or use `play.sh`).

## Updating

Download the new release, unpack it over this folder (your `game/` folder and the prefix are kept).

## Known gaps

Tested with Proton 11 on NVIDIA with keyboard and an Xbox controller. Not yet tested: this title-update build of the exe under Proton (the base-disc build was), AMD GPUs and the
Steam Deck, DLC, ultrawide. DLSS is not available under Proton. Please report what works and what
doesn't on GitHub, with the end of the terminal output if something fails.

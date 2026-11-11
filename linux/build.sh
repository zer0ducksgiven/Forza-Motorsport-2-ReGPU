#!/bin/sh
# Builds fm2.exe on Linux, for Windows: clang-cl + an xwin (MSVC/Windows SDK) sysroot +
# lld-link. Verified on clang 23 / CMake 3.31 + 4.4 / Proton 11.
#
#   linux/build.sh              # everything, into $FM2_LINUX_HOME (default ~/.fm2-linux)
#   linux/build.sh --reconfig   # throw the build directory away first
#
# Prerequisites: clang + clang-cl, lld-link, llvm-lib, llvm-rc, llvm-mt, cmake >= 3.25,
# ninja, python3, git, wine (for the fxc shim), curl. The game (game/default.xex, from your
# own disc: tools/xiso.py extract <iso> game) and thirdparty/{ffmpeg,fidelityfx,dlss} (see
# the README) are needed too; this script checks for them by name.
set -e
LINUX_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$LINUX_DIR")
HOME_DIR=${FM2_LINUX_HOME:-$HOME/.fm2-linux}
SYSROOT=$HOME_DIR/sysroot
BUILD=$HOME_DIR/build
SHIMBIN=$HOME_DIR/shims
XWIN_VERSION=0.10.0
XWIN=$HOME_DIR/xwin

[ "$1" = "--reconfig" ] && rm -rf "$BUILD"
mkdir -p "$HOME_DIR" "$SHIMBIN"

say() { printf '\n== %s\n' "$*"; }
die() { printf 'linux/build.sh: %s\n' "$*" >&2; exit 1; }

say "checking the toolchain"
for tool in clang-cl lld-link llvm-lib llvm-rc llvm-mt cmake ninja python3 git wine; do
    command -v "$tool" > /dev/null || die "missing $tool (Arch: clang lld llvm cmake ninja python git wine)"
done
cmake_version=$(cmake --version | head -1 | cut -d' ' -f3)
case $cmake_version in
    2.*|3.0.*|3.1[0-9].*|3.2[0-4].*) die "CMake 3.25 or newer is needed (have $cmake_version)" ;;
esac

say "the game and the third-party SDKs"
[ -f "$ROOT/game/default.xex" ] || die "no game/default.xex: run 'python3 tools/xiso.py extract <your disc>.iso game' first"
for dep in thirdparty/ffmpeg/include/libavcodec/avcodec.h thirdparty/fidelityfx/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll \
    thirdparty/dlss/include/nvsdk_ngx.h; do
    [ -e "$ROOT/$dep" ] || die "missing $dep (see the README's Building section)"
done

say "the MSVC and Windows SDK headers and libraries (xwin)"
if [ ! -d "$SYSROOT/sdk/include/um" ]; then
    if [ ! -x "$XWIN" ]; then
        curl -sL -o "$HOME_DIR/xwin.tar.gz" \
            "https://github.com/Jake-Shadle/xwin/releases/download/$XWIN_VERSION/xwin-$XWIN_VERSION-x86_64-unknown-linux-musl.tar.gz" ||
            die "could not download xwin"
        tar xzf "$HOME_DIR/xwin.tar.gz" -C "$HOME_DIR"
        mv "$HOME_DIR/xwin-$XWIN_VERSION-x86_64-unknown-linux-musl/xwin" "$XWIN"
    fi
    "$XWIN" --accept-license splat --output "$SYSROOT" || die "xwin splat failed"
fi

export INCLUDE="$SYSROOT/crt/include;$SYSROOT/sdk/include/ucrt;$SYSROOT/sdk/include/um;$SYSROOT/sdk/include/shared;$SYSROOT/sdk/include/winrt"
export LIB="$SYSROOT/crt/lib/x86_64;$SYSROOT/sdk/lib/um/x86_64;$SYSROOT/sdk/lib/ucrt/x86_64"

say "the fxc stand-in (the FSR 1 shaders are compiled by fxc at build time)"
if [ ! -f "$SHIMBIN/fxc.exe" ]; then
    clang-cl --target=x86_64-pc-windows-msvc /MT /O2 /std:c++20 -D_CRT_SECURE_NO_WARNINGS \
        -c "$LINUX_DIR/shims/fxc.cpp" -Fo"$SHIMBIN/fxc.obj" || die "could not compile the fxc shim"
    clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link /Fe"$SHIMBIN/fxc.exe" "$SHIMBIN/fxc.obj" d3dcompiler.lib ||
        die "could not link the fxc shim"
fi
# fxc.exe is linked against Microsoft's d3dcompiler_47, which has to sit next to it (Wine's
# own compiler cannot do the FSR 1 shaders either).
if [ ! -f "$SHIMBIN/d3dcompiler_47.dll" ]; then
    for candidate in ${FM2_D3DCOMPILER:-} "$(find "$HOME/.steam/steam/steamapps/common" \
        "$HOME/.local/share/Steam/steamapps/common" -maxdepth 5 -iname d3dcompiler_47.dll 2>/dev/null | head -1)"; do
        if [ -n "$candidate" ] && [ -f "$candidate" ] && [ "$(stat -c%s "$candidate")" -gt 2000000 ]; then
            cp "$candidate" "$SHIMBIN/d3dcompiler_47.dll"
            break
        fi
    done
fi
cat > "$SHIMBIN/fxc" <<EOF
#!/bin/sh
# fxc -> d3dcompiler_47 under Wine. d3dcompiler_47.dll must sit next to fxc.exe.
WINEDEBUG=-all WINEPREFIX="$HOME_DIR/wine-fxc" WINEDLLOVERRIDES="d3dcompiler_47=n" \\
    exec wine "$SHIMBIN/fxc.exe" "\$@"
EOF
chmod +x "$SHIMBIN/fxc"
cp "$LINUX_DIR/shims/py" "$SHIMBIN/py" && chmod +x "$SHIMBIN/py"
export PATH="$SHIMBIN:$PATH"

say "the recompiled game code (out/ppc)"
if [ ! -d "$ROOT/out/ppc" ] || [ -z "$(ls -A "$ROOT/out/ppc" 2>/dev/null)" ]; then
    # XenonRecomp runs on the host; the code it writes is then cross-compiled for Windows.
    cmake -S "$ROOT/thirdparty/XenonRecomp" -B "$HOME_DIR/tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > /dev/null
    ninja -C "$HOME_DIR/tools" XenonRecomp/XenonRecomp > /dev/null
    ( cd "$ROOT/thirdparty/XenonRecomp" && git apply ../../patches/xenonrecomp-fm2.patch 2>/dev/null || true )
    ninja -C "$HOME_DIR/tools" XenonRecomp/XenonRecomp > /dev/null
    mkdir -p "$ROOT/out/ppc"
    ( cd "$ROOT" && "$HOME_DIR/tools/XenonRecomp/XenonRecomp" config/fm2.toml thirdparty/XenonRecomp/XenonUtils/ppc_context.h > out/recomp.log 2>&1 )
    echo "recompiled $(ls "$ROOT/out/ppc" | wc -l) files, $(du -sh "$ROOT/out/ppc" | cut -f1)"
fi

say "fm2.exe"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$LINUX_DIR/toolchain-windows.cmake" \
    -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES="$LINUX_DIR/cmake-clang-cl-features.cmake"
ninja -C "$BUILD" fm2

say "built $BUILD/fm2.exe"

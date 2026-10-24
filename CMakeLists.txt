cmake_minimum_required(VERSION 3.25)
project(fm2 C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "fm2 needs clang-cl (the recompiled code uses clang builtins)")
endif()

add_subdirectory(thirdparty/XenonRecomp EXCLUDE_FROM_ALL)

# Build for the title-updated executable instead (tools/xexmap.py, tools/xexremap.py): the
# recompiled code is out/ppc_tu4, the guest addresses in the hooks are translated into the
# build folder, and the loader applies the update's default.xexp to your default.xex.
option(FM2_TITLE_UPDATE "Build for the title-updated executable" OFF)

# Recompiled game code (out/ppc, produced by XenonRecomp from your own
# default.xex; never committed).
if(FM2_TITLE_UPDATE)
    set(FM2_PPC_DIR "${CMAKE_SOURCE_DIR}/out/ppc_tu4")
else()
    set(FM2_PPC_DIR "${CMAKE_SOURCE_DIR}/out/ppc")
endif()
file(GLOB FM2_PPC_SOURCES CONFIGURE_DEPENDS "${FM2_PPC_DIR}/*.cpp")
if(NOT FM2_PPC_SOURCES)
    message(FATAL_ERROR "out/ppc is empty: run XenonRecomp first (see README)")
endif()
add_library(fm2_ppc STATIC ${FM2_PPC_SOURCES})
target_include_directories(fm2_ppc PUBLIC "${FM2_PPC_DIR}" thirdparty/XenonRecomp/thirdparty/simde)
target_compile_options(fm2_ppc PRIVATE -Wno-everything)
target_compile_options(fm2_ppc PUBLIC -msse4.1 -fno-strict-aliasing)
# Function names in crash reports and profiles.
target_compile_options(fm2_ppc PRIVATE -gline-tables-only "/FI${CMAKE_SOURCE_DIR}/src/ppc_hooks.h")

# The recompiled code reads the host's raw timestamp counter for mftb; give it the console's
# timebase instead (tools/patch_mftb.py, Fm2Timebase in src/kernel/runtime.cpp).
execute_process(COMMAND py -3 -I "${CMAKE_SOURCE_DIR}/tools/patch_mftb.py" "${FM2_PPC_DIR}"
                OUTPUT_VARIABLE FM2_MFTB_RESULT RESULT_VARIABLE FM2_MFTB_CODE OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT FM2_MFTB_CODE EQUAL 0)
    message(FATAL_ERROR "tools/patch_mftb.py failed on ${FM2_PPC_DIR}")
endif()
message(STATUS "mftb: ${FM2_MFTB_RESULT}")

file(GLOB_RECURSE FM2_SOURCES CONFIGURE_DEPENDS src/*.cpp)
if(FM2_TITLE_UPDATE)
    set(FM2_TU_DIR "${CMAKE_SOURCE_DIR}/out/tu4")
    set(FM2_REMAP_DIR "${CMAKE_BINARY_DIR}/remapped")
    set(FM2_REMAP_ARGS "${FM2_TU_DIR}/map.json" "${CMAKE_SOURCE_DIR}/out/base.bin" "${FM2_TU_DIR}/image.bin")
    execute_process(COMMAND py -3 -I "${CMAKE_SOURCE_DIR}/tools/xexremap.py" scan ${FM2_REMAP_ARGS} "${CMAKE_SOURCE_DIR}/src"
                    OUTPUT_VARIABLE FM2_REMAP_LIST RESULT_VARIABLE FM2_REMAP_RESULT OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT FM2_REMAP_RESULT EQUAL 0)
        message(FATAL_ERROR "FM2_TITLE_UPDATE needs out/tu4/map.json (tools/xexmap.py build ...)")
    endif()
    string(REPLACE "\n" ";" FM2_REMAP_LIST "${FM2_REMAP_LIST}")
    # Sources with guest addresses are compiled from translated copies.
    foreach(rel ${FM2_REMAP_LIST})
        string(STRIP "${rel}" rel)
        if(rel MATCHES "\\.cpp$")
            list(REMOVE_ITEM FM2_SOURCES "${CMAKE_SOURCE_DIR}/src/${rel}")
            add_custom_command(OUTPUT "${FM2_REMAP_DIR}/${rel}"
                COMMAND py -3 -I "${CMAKE_SOURCE_DIR}/tools/xexremap.py" source ${FM2_REMAP_ARGS} "${CMAKE_SOURCE_DIR}/src/${rel}" "${FM2_REMAP_DIR}/${rel}"
                DEPENDS "${CMAKE_SOURCE_DIR}/src/${rel}" "${FM2_TU_DIR}/map.json" "${CMAKE_SOURCE_DIR}/tools/xexremap.py"
                COMMENT "Translating guest addresses in ${rel}")
            list(APPEND FM2_SOURCES "${FM2_REMAP_DIR}/${rel}")
        endif()
    endforeach()
    # Weak fallbacks for the patched executable's imports instead of src/kernel/stubs.cpp.
    list(REMOVE_ITEM FM2_SOURCES "${CMAKE_SOURCE_DIR}/src/kernel/stubs.cpp")
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/generated/stubs_tu.cpp"
        COMMAND py -3 -I "${CMAKE_SOURCE_DIR}/tools/genstubs.py" "${FM2_PPC_DIR}/ppc_recomp_shared.h" "${CMAKE_BINARY_DIR}/generated/stubs_tu.cpp"
        DEPENDS "${FM2_PPC_DIR}/ppc_recomp_shared.h" "${CMAKE_SOURCE_DIR}/tools/genstubs.py"
        COMMENT "Generating import stubs for the patched executable")
    list(APPEND FM2_SOURCES "${CMAKE_BINARY_DIR}/generated/stubs_tu.cpp")
endif()

# Upscalers: AMD FidelityFX SDK (MIT) and NVIDIA DLSS SDK, fetched into thirdparty/ (see README).
set(FM2_FFX "${CMAKE_SOURCE_DIR}/thirdparty/fidelityfx/Kits/FidelityFX")
if(NOT EXISTS "${FM2_FFX}/upscalers/fsr3/include/gpu/fsr1/ffx_fsr1.h")
    message(FATAL_ERROR "thirdparty/fidelityfx is missing: clone FidelityFX-SDK v2.3.0 there (see README)")
endif()
# FSR 1 compute shaders, compiled with fxc at build time and embedded.
set(FM2_GENERATED "${CMAKE_BINARY_DIR}/generated")
add_custom_command(OUTPUT "${FM2_GENERATED}/fsr1_cs.h"
    COMMAND py -3 -I "${CMAKE_SOURCE_DIR}/tools/embed_hlsl.py" "${CMAKE_SOURCE_DIR}/src/gpu/fsr1.hlsl" "${FM2_GENERATED}/fsr1_cs.h"
            EasuMain,RcasMain,DepthCopyMain,GammaMain,EnhanceMain "${FM2_FFX}/api/internal/gpu" "${FM2_FFX}/upscalers/fsr3/include/gpu/fsr1"
    DEPENDS "${CMAKE_SOURCE_DIR}/src/gpu/fsr1.hlsl" "${CMAKE_SOURCE_DIR}/tools/embed_hlsl.py"
    COMMENT "Compiling FSR 1 shaders")
list(APPEND FM2_SOURCES "${FM2_GENERATED}/fsr1_cs.h")
set(FM2_DLSS "${CMAKE_SOURCE_DIR}/thirdparty/dlss")
if(NOT EXISTS "${FM2_DLSS}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "thirdparty/dlss is missing: clone NVIDIA/DLSS v310.9.16 there (see README)")
endif()
add_executable(fm2 ${FM2_SOURCES} "${CMAKE_SOURCE_DIR}/src/fm2.rc")
if(FM2_TITLE_UPDATE)
    target_compile_definitions(fm2 PRIVATE FM2_TITLE_UPDATE=1)
endif()
target_include_directories(fm2 PRIVATE src "${FM2_GENERATED}" "${FM2_DLSS}/include")
target_link_libraries(fm2 PRIVATE "${FM2_DLSS}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib" "${FM2_FFX}/signedbin/amd_fidelityfx_loader_dx12.lib")
target_include_directories(fm2 PRIVATE "${FM2_FFX}/api/include" "${FM2_FFX}/upscalers/include")
add_custom_command(TARGET fm2 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${FM2_FFX}/signedbin/amd_fidelityfx_loader_dx12.dll" "${FM2_FFX}/signedbin/amd_fidelityfx_upscaler_dx12.dll" $<TARGET_FILE_DIR:fm2>)
add_custom_command(TARGET fm2 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${FM2_DLSS}/lib/Windows_x86_64/rel/nvngx_dlss.dll" $<TARGET_FILE_DIR:fm2>)
# Microsoft's HLSL compiler ships beside fm2.exe: Windows loads it from there first, and under Proton
# (WINEDLLOVERRIDES=d3dcompiler_47=n) it replaces Wine's, which can't compile the bindless shaders.
set(FM2_D3DCOMPILER "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/d3dcompiler_47.dll" CACHE FILEPATH "Redistributable d3dcompiler_47.dll")
add_custom_command(TARGET fm2 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FM2_D3DCOMPILER}" $<TARGET_FILE_DIR:fm2>)
target_link_libraries(fm2 PRIVATE fm2_ppc XenonUtils synchronization winmm onecore d3d12 dxgi d3dcompiler)
target_compile_definitions(fm2 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(fm2 PRIVATE -gline-tables-only)
target_link_options(fm2 PRIVATE /STACK:0x1000000 /DEBUG)
target_link_libraries(fm2 PRIVATE dbghelp xaudio2 bcrypt winhttp)

# XMA decoding through FFmpeg's xma2 decoder (LGPL, linked dynamically): a shared build
# with include/, lib/ and bin/ in thirdparty/ffmpeg (see README).
set(FM2_FFMPEG "${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg")
if(NOT EXISTS "${FM2_FFMPEG}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "thirdparty/ffmpeg is missing: extract an FFmpeg win64 LGPL shared build there (see README)")
endif()
target_include_directories(fm2 PRIVATE "${FM2_FFMPEG}/include")
target_link_libraries(fm2 PRIVATE "${FM2_FFMPEG}/lib/avcodec.lib" "${FM2_FFMPEG}/lib/avutil.lib")
file(GLOB FM2_FFMPEG_DLLS "${FM2_FFMPEG}/bin/avcodec-*.dll" "${FM2_FFMPEG}/bin/avutil-*.dll" "${FM2_FFMPEG}/bin/swresample-*.dll")
add_custom_command(TARGET fm2 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FM2_FFMPEG_DLLS} $<TARGET_FILE_DIR:fm2>)

# Racing wheels and their force feedback through SDL3 (zlib), linked dynamically (see README).
set(FM2_SDL3 "${CMAKE_SOURCE_DIR}/thirdparty/sdl3")
if(NOT EXISTS "${FM2_SDL3}/include/SDL3/SDL.h")
    message(FATAL_ERROR "thirdparty/sdl3 is missing: copy SDL3's VC development package there (see README)")
endif()
target_include_directories(fm2 PRIVATE "${FM2_SDL3}/include")
target_link_libraries(fm2 PRIVATE "${FM2_SDL3}/lib/SDL3.lib")
add_custom_command(TARGET fm2 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FM2_SDL3}/lib/SDL3.dll" $<TARGET_FILE_DIR:fm2>)

# Xenos shader disassembler for captured microcode.
# hlsl.cpp reads a live switch and logs, so the disassembler pulls those in too.
add_executable(xenos_dis tools/xenos_dis.cpp src/gpu/ucode.cpp src/gpu/hlsl.cpp src/live_switch.cpp src/log.cpp)
target_include_directories(xenos_dis PRIVATE src)
target_link_libraries(xenos_dis PRIVATE d3dcompiler)

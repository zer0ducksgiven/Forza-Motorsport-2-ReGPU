// The main menu's X button opened the Xbox Live Marketplace, which never answers here: the game sat on a
// loading screen for good. X now asks to quit, with the game's own pop-up (the menu changes are in
// src/kernel/zip_overlay.cpp); its Yes calls Game:ShowMarketplace(), whose native (sub_822B9990, reached
// through the Lua binding's handler sub_822BBF98) is replaced here to quit. Both exist only in the
// title-updated executable, so the name is pasted together (tools/xexremap.py translates sub_ literals
// from the disc executable).
#include <cstdio>

#include <windows.h>

#include "input/wheel.h"
#include "gpu/renderer.h"
#include "kernel/imports.h"
#include "log.h"

#if FM2_TITLE_UPDATE
#define TU_SUB(address) sub_##address

PPC_FUNC(TU_SUB(822B9990))
{
    Log("quit: chosen from the main menu");
    WheelStop();
    RendererSaveCaches();
    fflush(nullptr);
    ExitProcess(0);
    (void)ctx;
    (void)base;
}
#endif

// Whether the player is driving, for the racing wheel's forces (input/wheel.cpp fades them out
// when not). The game's force feedback object (the one sub_82379590 sends the wheel's effects
// from) has a byte at +5 that it turns on while the player drives: with it off the game sends no
// steering force and the centring spring at full strength (gain 255), which is what the Xbox 360
// wheel did in menus, pauses and replays. sub_82360378 is its setter (reached only through a
// virtual call); the state is taken here on the way through.
#include "input/wheel.h"
#include "kernel/imports.h"
#include "log.h"

PPC_FUNC(sub_82360378)
{
    const bool driving = uint8_t(ctx.r4.u32) != 0;
    static int last = -1;
    if (int(driving) != last)
    {
        Log("wheel: the game says %s", driving ? "driving" : "not driving");
        last = driving;
    }
    WheelSetDriving(driving);
    *GuestPtr<uint8_t>(ctx.r3.u32 + 5) = uint8_t(ctx.r4.u32);
    (void)base;
}

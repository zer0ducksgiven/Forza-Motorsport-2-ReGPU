// Replays are recorded to the console's cache partition (cache:\), which the game mounts at boot with
// XMountUtilityDrive (linked into the title, reached through its one caller in the game's storage start-up).
// That code reads the hard drive's partition header and formats the cache partition as FATX through disk
// ioctls, none of which exist here, so cache: was never mounted: no race was recorded and Watch Replay wasn't
// offered after a race. cache: is already Documents\fm2\cache (src/kernel/imports_io.cpp), so the mount now
// just succeeds, setting the flag the real one sets. Its address is the title-updated executable's, so the
// name is pasted together as in quit_button.cpp.
//
// Game:HasHarddrive() (Watch Replay and Save Replay after a race, Replay Theatre in Options) returns a byte of the
// game's system object that only the console's storage notifications set, so its native now says yes too.
#include "kernel/imports.h"
#include "log.h"

#if FM2_TITLE_UPDATE
#define TU_SUB(address) sub_##address

PPC_FUNC(TU_SUB(82772980))
{
    // (formatIfNeeded, bytesPerCluster, cacheSize)
    // The mounted flag; pasted together like the name, so tools/xexremap.py leaves this title update address be.
    *GuestPtr<uint32_t>((0x82A5u << 16) + 0x3928) = __builtin_bswap32(1);
    Log("system: cache: mounted (replays are recorded)");
    Return(ctx, X_STATUS_SUCCESS);
    (void)base;
}

PPC_FUNC(TU_SUB(825BCAB8))
{
    ctx.r3.u64 = 1;
    (void)base;
}
#endif

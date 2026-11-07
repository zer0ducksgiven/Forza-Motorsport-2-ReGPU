// FM2_ARGS: options for the game's own command-line parser (CForzaCommandLineParameters), for
// testing. Retail blanks the command-line buffer in sub_82464098 (TU 8247F500) before parsing it; this hook
// puts FM2_ARGS there instead. "launch=true track=N" boots through the developers'
// CommandLineCircuitRace flow straight into a race. Booleans take "true", not "1".
#include <cstdlib>
#include <cstring>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

PPC_FUNC_IMPL(__imp__sub_82464098);
PPC_FUNC(sub_82464098)
{
    const uint32_t obj = ctx.r3.u32;
    __imp__sub_82464098(ctx, base);
    const char* args = getenv("FM2_ARGS");
    if (!args || !*args)
        return;
    char* buf = GuestPtr<char>(obj + 4); // 0x3fc bytes, see TU sub_8247FA28
    strncpy(buf, args, 0x3FB);
    buf[0x3FB] = 0;
    Log("cmdline: %s", buf);
}

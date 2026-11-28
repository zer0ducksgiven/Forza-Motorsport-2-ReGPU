// Profile-guided optimisation of ReGPU and FM2's own code (from the FM3/FM4 ports; FM4 on the Deck: race 66-70 -> 86-88
// fps). Only in a build configured with -DFM2_PGO=gen (ReGPU and src/ compiled with -fprofile-instr-generate): the game
// can end through ExitProcess, which skips the profile runtime's exit handler, so the profile is written on request:
// PgoWrite() (the benchmark calls it at its end, regpu_host/bench.cpp) or the debug server's "env FM2_PGO_WRITE 1". It goes
// to LLVM_PROFILE_FILE (name each run's file: Wine reuses process ids). Then llvm-profdata merge into pgo/fm2.profdata and
// configure -DFM2_PGO=use (CMakeLists.txt). The recompiled game code (fm2_ppc) is not instrumented.
#include <windows.h>

#include <thread>

#include "log.h"

#ifdef FM2_PGO_GEN
extern "C" int __llvm_profile_write_file(void);
void PgoWrite()
{
    const int r = __llvm_profile_write_file();
    Log("pgo: profile written (%d)", r);
}
namespace
{
struct PgoWriter
{
    PgoWriter()
    {
        std::thread([] {
            bool last = false;
            for (;;)
            {
                Sleep(500);
                char v[4] = {};
                const bool on = GetEnvironmentVariableA("FM2_PGO_WRITE", v, sizeof(v)) && v[0] == '1';
                if (on && !last)
                    PgoWrite();
                last = on;
            }
        }).detach();
    }
} g_pgoWriter;
} // namespace
#else
void PgoWrite() {}
#endif

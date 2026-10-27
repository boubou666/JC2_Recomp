// Game-specific runtime setup for Jade Cocoon 2 (NTSC-U, SLUS-20309).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"

#include <iostream>

namespace
{
    constexpr uint32_t kJc2NtscEntry = 0x00100008u;

    void applyJc2(PS2Runtime &runtime)
    {
        (void)runtime;
        std::cout << "[jc2] Jade Cocoon 2 (NTSC-U SLUS-20309) overrides applied" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Jade Cocoon 2 (USA)", "SLUS_203.09", kJc2NtscEntry, 0u, &applyJc2)

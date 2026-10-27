// Game-specific runtime setup for Jade Cocoon 2 (NTSC-U, SLUS-20309).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr uint32_t kJc2NtscEntry = 0x00100008u;

    // Code overlays: the ELF reserves 0x940000 (zero-size LOAD segments up to memsz 0x16800) and the game fills it
    // from CDDATA.000 at run time.
    constexpr uint32_t kOverlayBase = 0x00940000u;
    constexpr uint32_t kOverlayMaxSize = 0x00040000u;

    // JC2_OVERLAY_DUMP=<dir> (bring-up aid): saves each distinct overlay image as <dir>/ovl_<fnv1a>.bin once it has
    // stayed unchanged for two polls (the game has finished loading it), for wrapping into an ELF and recompiling as
    // a fork overlay module. Reads guest RAM from a host thread: a capture can race a load, hence the stability check.
    uint64_t fnv1a(const uint8_t *data, size_t size)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash = (hash ^ data[i]) * 1099511628211ull;
        }
        return hash;
    }

    // Used length: up to the last non-zero word, so an image is not padded with the rest of the slot.
    size_t usedLength(const uint8_t *data, size_t size)
    {
        size_t end = size;
        while (end >= 4 && std::memcmp(data + end - 4, "\0\0\0\0", 4) == 0)
        {
            end -= 4;
        }
        return end;
    }

    void overlayDumpThread(PS2Runtime *runtime, std::string dir)
    {
        std::set<uint64_t> saved;
        std::vector<uint8_t> current(kOverlayMaxSize);
        uint64_t lastHash = 0;
        while (!runtime->isStopRequested())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const uint8_t *rdram = runtime->memory().getRDRAM();
            std::memcpy(current.data(), rdram + kOverlayBase, kOverlayMaxSize);
            const size_t used = usedLength(current.data(), current.size());
            if (used == 0)
            {
                lastHash = 0;
                continue;
            }
            const uint64_t hash = fnv1a(current.data(), used);
            if (hash != lastHash)
            {
                lastHash = hash;
                continue;
            }
            if (!saved.insert(hash).second)
            {
                continue;
            }
            char name[64];
            std::snprintf(name, sizeof(name), "/ovl_%016llx.bin", static_cast<unsigned long long>(hash));
            const std::string path = dir + name;
            if (FILE *file = std::fopen(path.c_str(), "wb"))
            {
                std::fwrite(current.data(), 1, used, file);
                std::fclose(file);
                std::cout << "[jc2] overlay image 0x" << std::hex << used << std::dec << " bytes -> " << path
                          << std::endl;
            }
        }
    }

    void applyJc2(PS2Runtime &runtime)
    {
        if (const char *dir = std::getenv("JC2_OVERLAY_DUMP"); dir && *dir)
        {
            std::thread(overlayDumpThread, &runtime, std::string(dir)).detach();
            std::cout << "[jc2] dumping overlay images at 0x940000 to " << dir << std::endl;
        }
        std::cout << "[jc2] Jade Cocoon 2 (NTSC-U SLUS-20309) overrides applied" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Jade Cocoon 2 (USA)", "SLUS_203.09", kJc2NtscEntry, 0u, &applyJc2)

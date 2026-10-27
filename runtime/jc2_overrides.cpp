// Game-specific runtime setup for Jade Cocoon 2 (NTSC-U, SLUS-20309).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "runtime/ee_scheduler.h"

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

    uint32_t rd32(const uint8_t *rdram, uint32_t addr)
    {
        return *reinterpret_cast<const uint32_t *>(rdram + (addr & 0x1FFFFFCu));
    }

    // JC2_TRACE_SYNC=1 (bring-up aid): the game's semaphore helpers, wait 0x485BD0(&id) and signal
    // 0x485C30(&id, fromInterrupt), with the thread, the semaphore id and the caller, and the libcdvd callback
    // 0x1509E0 (registered by 0x14FA10 through sceCdCallback). Boot 4: both game threads wait on semaphore 4.
    template <uint32_t Address, void (*Function)(uint8_t *, R5900Context *, PS2Runtime *)>
    void syncTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_calls = 0;
        const uint32_t n = ++s_calls, ra = getRegU32(ctx, 31), a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
        const int thread = runtime->eeScheduler().currentThreadId();
        const uint32_t sema = (a0 >= 0x100000u && a0 < 0x2000000u) ? rd32(rdram, a0) : 0u;
        if (n <= 64u)
            std::fprintf(stderr, "[jc2:sync] enter 0x%x #%u thread=%d ra=0x%x a0=0x%x (*a0=%d) a1=0x%x\n", Address, n,
                         thread, ra, a0, static_cast<int>(sema), a1);
        Function(rdram, ctx, runtime);
        if (n <= 64u)
            std::fprintf(stderr, "[jc2:sync] leave 0x%x #%u thread=%d -> 0x%x\n", Address, n, thread,
                         getRegU32(ctx, 2));
    }

    // With JC2_TRACE_SYNC: every CreateSema (0x119EE0) with its ee_sema_t {count, max, init, attr} and the id it got.
    // Boot 6: the loader's SignalSema on semaphore 4 returned -1 (over its maximum) right before the deadlock.
    void createSemaTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t param = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
        const int thread = runtime->eeScheduler().currentThreadId();
        CreateSema_0x119ee0(rdram, ctx, runtime);
        std::fprintf(stderr, "[jc2:sync] CreateSema thread=%d ra=0x%x param=0x%x count=%d max=%d init=%d attr=0x%x -> %d\n",
                     thread, ra, param, static_cast<int>(rd32(rdram, param)), static_cast<int>(rd32(rdram, param + 4)),
                     static_cast<int>(rd32(rdram, param + 8)), rd32(rdram, param + 16),
                     static_cast<int>(getRegU32(ctx, 2)));
    }

    void applyJc2(PS2Runtime &runtime)
    {
        if (const char *trace = std::getenv("JC2_TRACE_SYNC"); trace && *trace == '1')
        {
            runtime.registerFunction(0x00485BD0u, &syncTrace<0x485BD0u, FUN_00485bd0_0x485bd0>);
            runtime.registerFunction(0x00485C30u, &syncTrace<0x485C30u, FUN_00485c30_0x485c30>);
            runtime.registerFunction(0x001509E0u, &syncTrace<0x1509E0u, FUN_001509e0_0x1509e0>);
            runtime.registerFunction(0x0014FA10u, &syncTrace<0x14FA10u, FUN_0014fa10_0x14fa10>);
            runtime.registerFunction(0x00119EE0u, &createSemaTrace);
            runtime.registerFunction(0x00119F00u, &syncTrace<0x119F00u, SignalSema_0x119f00>);
            std::cout << "[jc2] tracing the semaphore helpers and the CD callback" << std::endl;
        }
        if (const char *dir = std::getenv("JC2_OVERLAY_DUMP"); dir && *dir)
        {
            std::thread(overlayDumpThread, &runtime, std::string(dir)).detach();
            std::cout << "[jc2] dumping overlay images at 0x940000 to " << dir << std::endl;
        }
        std::cout << "[jc2] Jade Cocoon 2 (NTSC-U SLUS-20309) overrides applied" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Jade Cocoon 2 (USA)", "SLUS_203.09", kJc2NtscEntry, 0u, &applyJc2)

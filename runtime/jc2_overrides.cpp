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
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

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

    // JC2_TRACE_SCENE=1 (bring-up aid): logs each change of the scene state the main loop 0x15C140 runs on (gp-0x74C0
    // = 0x6C47B0; index into the 19-scene table at 0x4E6FA0, M2_MODE_MAYU2 = 2 is the game). The loop, and main with
    // it, ends when a scene leaves a state of 19 or more. Boot 11: the state stayed 0, main never got there. Polls
    // from a host thread. JC2_WATCH=<hex address> also logs each change of that guest word with the EE thread running:
    // boot 12, main's 0x15C0E0 saved ra at 0x1FFFFE0 (sp 0x1FFFFE0) and returned to 0.
    constexpr uint32_t kSceneState = 0x006C47B0u;

    void sceneTraceThread(PS2Runtime *runtime)
    {
        uint32_t last = ~0u;
        const char *watch = std::getenv("JC2_WATCH");
        const uint32_t watchAddress = watch && *watch ? static_cast<uint32_t>(std::strtoul(watch, nullptr, 16)) : 0u;
        uint32_t lastWatched = ~0u;
        while (!runtime->isStopRequested())
        {
            const uint8_t *rdram = runtime->memory().getRDRAM();
            const uint32_t state = rd32(rdram, kSceneState);
            if (state != last)
            {
                std::fprintf(stderr, "[jc2:scene] state %u -> %u (EE cycle %llu)\n", last, state,
                             static_cast<unsigned long long>(runtime->eeScheduler().currentCycle()));
                last = state;
            }
            static uint64_t s_lastSmode2 = ~0ull;
            if (const uint64_t smode2 = runtime->memory().gs().smode2; smode2 != s_lastSmode2)
            {
                std::fprintf(stderr, "[jc2:gs] SMODE2 0x%llx -> 0x%llx (EE cycle %llu)\n",
                             static_cast<unsigned long long>(s_lastSmode2), static_cast<unsigned long long>(smode2),
                             static_cast<unsigned long long>(runtime->eeScheduler().currentCycle()));
                s_lastSmode2 = smode2;
            }
            if (watchAddress != 0u)
            {
                const uint32_t value = rd32(rdram, watchAddress);
                if (value != lastWatched)
                {
                    std::fprintf(stderr, "[jc2:watch] 0x%x: 0x%x -> 0x%x (EE thread %d, cycle %llu)\n", watchAddress,
                                 lastWatched, value, runtime->eeScheduler().currentThreadId(),
                                 static_cast<unsigned long long>(runtime->eeScheduler().currentCycle()));
                    lastWatched = value;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    // JC2_TRACE_CALLS=1 (bring-up aid): logs each call of main's init chain 0x15C0E0 -> 0x15B780 and of 0x15B780's
    // callees, in order, with its thread and ra, and each EE thread end. Boot 11: thread 1 returned to pc 0 inside it.
    std::map<uint32_t, PS2Runtime::RecompiledFunction> g_tracedCalls;

    void callTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pc = ctx->pc;
        auto it = g_tracedCalls.upper_bound(pc);
        if (it == g_tracedCalls.begin())
            return;
        --it;
        static uint32_t s_calls = 0;
        if (++s_calls <= 2000u)
            std::fprintf(stderr, "[jc2:call] 0x%x%s thread=%d ra=0x%x sp=0x%x\n", it->first,
                         pc == it->first ? "" : " (resume)", runtime->eeScheduler().currentThreadId(),
                         getRegU32(ctx, 31), getRegU32(ctx, 29));
        it->second(rdram, ctx, runtime);
    }

    // JC2_TRACE_GS=1 (bring-up aid): each sceGsResetGraph(mode, interlace, omode, ffmode) and the SMODE2 it leaves.
    // The intro movie and the menu show the top half of the picture at twice the height: the presenter halves the
    // buffer in interlaced frame mode (SMODE2 INT=1 FFMD=1), and JC2 draws 224 full lines with DH 223.
    constexpr uint32_t kGsResetGraph = 0x0010F7F8u;
    PS2Runtime::RecompiledFunction g_gsResetGraph = nullptr;

    constexpr uint32_t kGsSetDefDispEnv = 0x0010FA00u;
    PS2Runtime::RecompiledFunction g_gsSetDefDispEnv = nullptr;

    // sceGsSetDefDispEnv(env, psm, w, h, dx, dy): its DH is h - 1, or 2h - 1 in interlaced frame mode (SMODE2 3).
    void gsSetDefDispEnvTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t env = getRegU32(ctx, 4), psm = getRegU32(ctx, 5), w = getRegU32(ctx, 6), h = getRegU32(ctx, 7);
        const uint64_t smode2 = runtime->memory().gs().smode2;
        g_gsSetDefDispEnv(rdram, ctx, runtime);
        uint64_t display = 0;
        std::memcpy(&display, rdram + ((env + 0x18u) & 0x1FFFFF8u), sizeof(display));
        std::fprintf(stderr, "[jc2:gs] sceGsSetDefDispEnv(env 0x%x, psm %u, w %u, h %u) with SMODE2 0x%llx -> DISPLAY 0x%llx (DH %u)\n",
                     env, psm, w, h, static_cast<unsigned long long>(smode2), static_cast<unsigned long long>(display),
                     static_cast<unsigned>((display >> 44) & 0x7FFu));
    }

    void gsResetGraphTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7);
        g_gsResetGraph(rdram, ctx, runtime);
        std::fprintf(stderr, "[jc2:gs] sceGsResetGraph(mode %u, interlace %u, omode %u, ffmode %u) -> SMODE2 0x%llx\n",
                     a0, a1, a2, a3, static_cast<unsigned long long>(runtime->memory().gs().smode2));
    }

#ifdef _WIN32
    // JC2_GUARD=<hex guest address> (bring-up aid): logs the host code address (and the EE thread) of every write
    // to the 16 bytes at that guest address, through a guard page on its host page re-armed after one single step.
    // Map the address back with addr2line -e ps2EntryRunner.debug. Boot 13: EE thread 1 itself zeroed main's
    // 0x15C0E0 saved ra at 0x1FFFFE0; a 200 us poll cannot say which code did it.
    PS2Runtime *g_guardRuntime = nullptr;
    uint8_t *g_guardPage = nullptr;
    uint8_t *g_guardLo = nullptr;
    std::atomic<int> g_guardHits{0};
    thread_local bool t_guardRearm = false;

    LONG WINAPI guardHandler(EXCEPTION_POINTERS *info)
    {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code == STATUS_GUARD_PAGE_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        {
            uint8_t *const address = reinterpret_cast<uint8_t *>(info->ExceptionRecord->ExceptionInformation[1]);
            if (address < g_guardPage || address >= g_guardPage + 4096)
                return EXCEPTION_CONTINUE_SEARCH;
            const bool write = info->ExceptionRecord->ExceptionInformation[0] == 1;
            if (write && address + 16 > g_guardLo && address < g_guardLo + 16 && g_guardHits.fetch_add(1) < 64)
            {
                const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                std::fprintf(stderr, "[jc2:guard] write at guest +%d of the slot: host rip 0x%llx (image +0x%llx), EE "
                                     "thread %d, cycle %llu\n",
                             static_cast<int>(address - g_guardLo), static_cast<unsigned long long>(info->ContextRecord->Rip),
                             static_cast<unsigned long long>(info->ContextRecord->Rip - base),
                             g_guardRuntime->eeScheduler().currentThreadId(),
                             static_cast<unsigned long long>(g_guardRuntime->eeScheduler().currentCycle()));
            }
            info->ContextRecord->EFlags |= 0x100u; // single step, then re-arm the guard
            t_guardRearm = true;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code == EXCEPTION_SINGLE_STEP && t_guardRearm)
        {
            t_guardRearm = false;
            DWORD old = 0;
            VirtualProtect(g_guardPage, 4096, PAGE_READWRITE | PAGE_GUARD, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void installGuard(PS2Runtime &runtime, uint32_t guestAddress)
    {
        g_guardRuntime = &runtime;
        g_guardLo = runtime.memory().getRDRAM() + (guestAddress & 0x1FFFFF0u);
        g_guardPage = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(g_guardLo) & ~uintptr_t{4095});
        AddVectoredExceptionHandler(1, &guardHandler);
        DWORD old = 0;
        const BOOL ok = VirtualProtect(g_guardPage, 4096, PAGE_READWRITE | PAGE_GUARD, &old);
        std::cout << "[jc2] guarding guest 0x" << std::hex << guestAddress << std::dec << (ok ? "" : " (VirtualProtect failed)")
                  << std::endl;
    }
#endif

    void applyJc2(PS2Runtime &runtime)
    {
#ifdef _WIN32
        if (const char *guard = std::getenv("JC2_GUARD"); guard && *guard)
            installGuard(runtime, static_cast<uint32_t>(std::strtoul(guard, nullptr, 16)));
#endif
        if (const char *trace = std::getenv("JC2_TRACE_GS"); trace && *trace == '1')
        {
            if (PS2Runtime::RecompiledFunction original = runtime.lookupFunction(kGsResetGraph))
            {
                g_gsResetGraph = original;
                runtime.registerFunction(kGsResetGraph, &gsResetGraphTrace);
            }
            if (PS2Runtime::RecompiledFunction original = runtime.lookupFunction(kGsSetDefDispEnv))
            {
                g_gsSetDefDispEnv = original;
                runtime.registerFunction(kGsSetDefDispEnv, &gsSetDefDispEnvTrace);
            }
        }
        if (const char *trace = std::getenv("JC2_TRACE_CALLS"); trace && *trace == '1')
        {
            static const uint32_t kTraced[] = {
                0x15C0E0u, 0x15B780u, 0x15C210u, 0x165390u, 0x119D70u, 0x119DD0u, 0x11A160u, 0x11C788u, 0x121E28u,
                0x121E60u, 0x124630u, 0x13D8E0u, 0x13DD10u, 0x150EF0u, 0x151460u, 0x151F00u, 0x156610u, 0x156BF0u,
                0x156C50u, 0x1594A0u, 0x1595E0u, 0x15BB50u, 0x15BF90u, 0x15C220u, 0x15C580u, 0x160D60u, 0x160DC0u,
                0x165590u, 0x17AD50u, 0x20DCF0u, 0x234260u, 0x290050u, 0x292010u, 0x2B03F0u, 0x41BAB0u, 0x41F890u,
                0x478090u, 0x4867B0u, 0x494B60u, 0x498FB0u,
            };
            for (const uint32_t address : kTraced)
            {
                if (PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address))
                {
                    g_tracedCalls[address] = original;
                    runtime.registerFunction(address, &callTrace);
                }
            }
            runtime.eeScheduler().addThreadEndObserver(
                [&runtime](int id)
                {
                    static int s_ends = 0;
                    if (id <= 0 || ++s_ends > 200)
                        return;
                    const GuestThread *thread = runtime.eeScheduler().thread(id);
                    std::fprintf(stderr, "[jc2:call] EE thread %d ended (stack 0x%x size 0x%x, sp 0x%x)\n", id,
                                 thread ? thread->stack : 0u, thread ? thread->stackSize : 0u,
                                 thread ? getRegU32(const_cast<R5900Context *>(&thread->context), 29) : 0u);
                });
            std::cout << "[jc2] tracing main's init calls" << std::endl;
        }
        if (const char *trace = std::getenv("JC2_TRACE_SCENE"); trace && *trace == '1')
            std::thread(sceneTraceThread, &runtime).detach();
        if (const char *trace = std::getenv("JC2_TRACE_SYNC"); trace && *trace == '1')
        {
            runtime.registerFunction(0x00485BD0u, &syncTrace<0x485BD0u, FUN_00485bd0_0x485bd0>);
            runtime.registerFunction(0x00485C30u, &syncTrace<0x485C30u, FUN_00485c30_0x485c30>);
            runtime.registerFunction(0x001509E0u, &syncTrace<0x1509E0u, FUN_001509e0_0x1509e0>);
            runtime.registerFunction(0x0014FA10u, &syncTrace<0x14FA10u, FUN_0014fa10_0x14fa10>);
            runtime.registerFunction(0x00119EE0u, &createSemaTrace);
            // main (0x141D10) runs these five in turn, then returns 0 to crt0's Exit: one of them returned early.
            runtime.registerFunction(0x00100220u, &syncTrace<0x100220u, FUN_00100220_0x100220>);
            runtime.registerFunction(0x0015C0E0u, &syncTrace<0x15C0E0u, FUN_0015c0e0_0x15c0e0>);
            runtime.registerFunction(0x0015C140u, &syncTrace<0x15C140u, FUN_0015c140_0x15c140>);
            runtime.registerFunction(0x0015C120u, &syncTrace<0x15C120u, FUN_0015c120_0x15c120>);
            runtime.registerFunction(0x00100260u, &syncTrace<0x100260u, FUN_00100260_0x100260>);
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

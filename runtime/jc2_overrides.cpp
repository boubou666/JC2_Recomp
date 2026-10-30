// Game-specific runtime setup for Jade Cocoon 2 (NTSC-U, SLUS-20309).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "ps2_runtime_macros.h"
#include "Stubs/MPEG.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
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
        // JC2_WATCH=<hex address>[,<byte count>]: with a count, the whole range is logged (as words) when any of it
        // changes. Boot 57: JC2's file-load request table at 0x8B9860 (4 slots of 32 bytes).
        const char *watch = std::getenv("JC2_WATCH");
        char *watchEnd = nullptr;
        const uint32_t watchAddress = watch && *watch ? static_cast<uint32_t>(std::strtoul(watch, &watchEnd, 16)) : 0u;
        const uint32_t watchBytes =
            watchEnd && *watchEnd == ',' ? std::min<uint32_t>(static_cast<uint32_t>(std::strtoul(watchEnd + 1, nullptr, 0)) & ~3u, 256u) : 0u;
        std::vector<uint32_t> lastRange(watchBytes / 4u, ~0u);
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
            if (watchAddress != 0u && watchBytes != 0u)
            {
                bool changed = false;
                for (uint32_t i = 0; i < lastRange.size(); ++i)
                {
                    const uint32_t value = rd32(rdram, watchAddress + i * 4u);
                    changed |= value != lastRange[i];
                    lastRange[i] = value;
                }
                if (changed)
                {
                    std::string line;
                    char word[12];
                    for (const uint32_t value : lastRange)
                    {
                        std::snprintf(word, sizeof(word), " %08x", value);
                        line += word;
                    }
                    std::fprintf(stderr, "[jc2:watch] 0x%x:%s (EE thread %d, cycle %llu)\n", watchAddress, line.c_str(),
                                 runtime->eeScheduler().currentThreadId(),
                                 static_cast<unsigned long long>(runtime->eeScheduler().currentCycle()));
                }
            }
            else if (watchAddress != 0u)
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

    // JC2_TRACE_SCRIPT=1 (bring-up aid): the event scripts (code overlays such as m2esA001's 0x940710, run every frame)
    // are sequences of calls to 0x1F3390(ctx, opcode, args...), each returning 0 while its command is not done (the
    // script then stops and runs again next frame). Logs each distinct (opcode, a2, a3, result) once, with the
    // caller: the command a stuck scene waits on keeps returning 0.
    // The script calls three command functions: 0x1EB5D0 (most commands), 0x1F4B20 and 0x1F3390.
    // JC2_TRACE_PCM=<count>: logs the movie PCM stream callback (0x153880, registered for sceMpegStrPCM): the data
    // pointer and length libmpeg hands it, the 4 bytes there (the private stream 1 sub-header, ff a0 00 00, when the
    // pointer is right) and the PTS, for the first <count> calls.
    constexpr uint32_t kPcmCallback = 0x00153880u;
    PS2Runtime::RecompiledFunction g_pcmCallback = nullptr;
    uint32_t g_pcmTraceLeft = 0;

    // The movie PCM queue (JC2_PCM_QUEUE=1, off by default: it does not cure the New-game movie's noise, see below). The callback copies a packet's PCM into the game's
    // ring (object at *(user + 0x5C): +0 header state, +0x30 base, +0x34 write position, +0x38 bytes used, +0x3C size,
    // 96 KB for the movies; reserve 0x155950, commit 0x155AE0) as far as it fits, returns 0 only when nothing fit, and
    // the bytes that did not fit are dropped: libmpeg would hand that packet again later, the libmpeg HLE ignores the
    // result. With odd-sized payloads every drop put the PCM one byte off: the New-game movie's noise (boots 102-108).
    // Here the bytes the ring cannot take wait in a host FIFO, in order, and fill the ring as it drains. The ring still
    // fills to the brim as on the console (the movie stream starts from a full ring: queueing whole packets only left
    // it short of full and the movie silent, boots 109-110). Boot 113: the ring then stays exactly full for the whole
    // movie (used = size, never lowered) while the AutoDMA stream to the IOP runs on, and the noise is unchanged: the
    // consumer does not free this ring the way assumed, so the cause lies elsewhere. The source is read as the callback reads it: from
    // data + 4 (past the ff a0 00 00 sub-header), wrapping at the end of the PSS ring (object at *(user + 0x50): +0
    // base, +4 size).
    constexpr uint32_t kPcmRingCommit = 0x00155AE0u;
    bool g_pcmQueueOn = false;
    std::deque<uint8_t> g_pcmPending;
    uint32_t g_pcmQueueUser = 0;

    void pcmCallbackQueued(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cb = getRegU32(ctx, 5), user = getRegU32(ctx, 6);
        const uint32_t data = rd32(rdram, cb + 8u), len = rd32(rdram, cb + 12u);
        const uint32_t ring = rd32(rdram, user + 0x5Cu), pss = rd32(rdram, user + 0x50u);
        if (user != g_pcmQueueUser)
        {
            g_pcmPending.clear(); // another movie
            g_pcmQueueUser = user;
        }
        const auto headerDone = [&] { return rd32(rdram, ring) != 0u; };
        const auto room = [&] { return rd32(rdram, ring + 0x3Cu) - rd32(rdram, ring + 0x38u); };
        const uint32_t pcm = len > 4u ? len - 4u : 0u;
        const uint32_t pssBase = rd32(rdram, pss), pssSize = rd32(rdram, pss + 4u);
        const auto sourceByte = [&](uint32_t i)
        {
            uint32_t addr = data + 4u + i;
            if (pssSize != 0u && addr >= pssBase + pssSize)
                addr -= pssSize;
            return rdram[addr & 0x1FFFFFFu];
        };

        // A ring back in its header state is a restarted stream: what was pending belongs to the old one.
        if (!g_pcmPending.empty() && !headerDone())
        {
            std::fprintf(stderr, "[jc2:pcmq] ring 0x%x restarted, %zu pending bytes dropped\n", ring, g_pcmPending.size());
            g_pcmPending.clear();
        }

        // The pending bytes first, as far as the ring takes them.
        static const PS2Runtime::RecompiledFunction s_commit = runtime->lookupFunction(kPcmRingCommit);
        if (!g_pcmPending.empty() && headerDone() && s_commit)
        {
            const uint32_t n = static_cast<uint32_t>(std::min<size_t>(room(), g_pcmPending.size()));
            if (n != 0u)
            {
                const uint32_t base = rd32(rdram, ring + 0x30u), size = rd32(rdram, ring + 0x3Cu);
                uint32_t pos = rd32(rdram, ring + 0x34u);
                for (uint32_t i = 0; i < n; ++i)
                {
                    rdram[(base + pos) & 0x1FFFFFFu] = g_pcmPending.front();
                    g_pcmPending.pop_front();
                    pos = (pos + 1u) % size;
                }
                R5900Context commit = *ctx;
                SET_GPR_U32(&commit, 4, ring);
                SET_GPR_U32(&commit, 5, n);
                SET_GPR_U32(&commit, 31, 0u);
                commit.pc = kPcmRingCommit;
                s_commit(rdram, &commit, runtime);
            }
        }

        if (g_pcmPending.empty())
        {
            // The game's own copy, then what it could not take waits.
            const bool header = !headerDone();
            const uint32_t free = header ? pcm : room();
            g_pcmCallback(rdram, ctx, runtime);
            for (uint32_t i = free; i < pcm; ++i)
                g_pcmPending.push_back(sourceByte(i));
            return;
        }
        for (uint32_t i = 0; i < pcm; ++i)
            g_pcmPending.push_back(sourceByte(i));
        static size_t s_peak = 0;
        if (g_pcmPending.size() > s_peak + 0x10000u)
        {
            s_peak = g_pcmPending.size();
            std::fprintf(stderr, "[jc2:pcmq] %zu bytes pending; ring 0x%x: used=0x%x size=0x%x\n", s_peak, ring,
                         rd32(rdram, ring + 0x38u), rd32(rdram, ring + 0x3Cu));
        }
        setReturnU32(ctx, 1u);
        ctx->pc = getRegU32(ctx, 31);
    }

    void pcmCallbackTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_pcmTraceLeft != 0u)
        {
            --g_pcmTraceLeft;
            const uint32_t cb = getRegU32(ctx, 5);
            const uint32_t data = rd32(rdram, cb + 8u), len = rd32(rdram, cb + 12u);
            const uint8_t *p = rdram + (data & 0x1FFFFFFu);
            std::fprintf(stderr, "[jc2:pcm] data=0x%x len=%u head=%02x%02x%02x%02x pts=0x%x%08x\n", data, len, p[0], p[1], p[2],
                         p[3], rd32(rdram, cb + 20u), rd32(rdram, cb + 16u));
        }
        if (g_pcmQueueOn)
            pcmCallbackQueued(rdram, ctx, runtime);
        else
            g_pcmCallback(rdram, ctx, runtime);
    }

    constexpr uint32_t kScriptCommands[] = {0x001EB5D0u, 0x001F4B20u, 0x001F3390u};
    std::map<uint32_t, PS2Runtime::RecompiledFunction> g_scriptCommands;

    void scriptCommandTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t function = ctx->pc, ra = getRegU32(ctx, 31), opcode = getRegU32(ctx, 5),
                       a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7);
        const auto it = g_scriptCommands.find(function);
        if (it == g_scriptCommands.end())
            return;
        it->second(rdram, ctx, runtime);
        const uint32_t result = getRegU32(ctx, 2);
        static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> s_seen;
        if (s_seen.size() < 8192u && s_seen.insert({function, ra, opcode, a2, a3, result}).second)
            std::fprintf(stderr, "[jc2:script] 0x%x ra=0x%x a1=0x%x a2=0x%x a3=0x%x -> %u (EE cycle %llu)\n", function,
                         ra, opcode, a2, a3, result, static_cast<unsigned long long>(runtime->eeScheduler().currentCycle()));
    }

    // Native frame loop (JC2_NATIVE=1, the default in scripts/run.sh): no vblank paces the game. JC2's flip
    // (0x15A150) counts its frames (0x6D6190) and waits in 0x15B400(target, &late) until the vblank handler's counter
    // (0x6D61AC) reaches the frame count; a late frame sets the counter back to target - 1 and counts itself
    // (0x6D61B0). It returns the CSR FIELD bit, which picks the half-line offset of the next field. The game steps
    // one fixed frame per call.
    // Here the wait blocks the main thread until the frame's deadline on the PC clock (EeScheduler::waitHostDeadline;
    // the FramePacer cap, 60 by default, 0 = no hold): while it waits, JC2's lower-priority threads run (loads, movie
    // decoding, the work the next frame draws from). Holding the frame with FramePacer::frameEnd() instead slept the
    // EE executor itself, starved those threads, and every frame came out black (boots 67-76); blocked this way,
    // boot 82 runs the menus, the movies and the first scene at 57-60 fps. At the deadline EE time is charged with
    // the real time the frame took (timers and the IOP's loads run on it), the frame is shown, and the wait returns
    // on time.
    constexpr uint32_t kFrameWait = 0x0015B400u;
    constexpr uint32_t kVBlankCounter = 0x006D61ACu;
    constexpr double kEeClockHz = 294912000.0;

    void nativeFrameEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t target = getRegU32(ctx, 4), late = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
        // As an on-time frame: not late. The vblank counter is left to the vblank handler,
        // which still counts on EE time; JC2_NATIVE_COUNTER=1 sets it to the target as before (bring-up A/B: movie
        // sound turned to white noise with the native loop, boots 95-98).
        static const bool s_setCounter = [] { const char *v = std::getenv("JC2_NATIVE_COUNTER"); return v && v[0] == '1'; }();
        if (s_setCounter)
            std::memcpy(rdram + kVBlankCounter, &target, sizeof(target));
        const uint32_t onTime = 0u;
        std::memcpy(rdram + (late & 0x1FFFFFCu), &onTime, sizeof(onTime));
        // The return value is the CSR FIELD bit at the wake-up, as the original reads it (the vblanks still run on EE
        // time). JC2_NATIVE_FIELD=parity returns the frame count's parity instead (the first version; with it the movie
        // soundtracks came out byte-swapped, as white noise, boots 95-99).
        static const bool s_parityField = [] { const char *v = std::getenv("JC2_NATIVE_FIELD"); return v && std::strcmp(v, "parity") == 0; }();
        static const uint32_t s_fieldPhase = static_cast<uint32_t>((runtime->memory().gs().csr.load() >> 13) & 1u) ^ (target & 1u);
        const uint32_t parityField = (target & 1u) ^ s_fieldPhase;

        // The deadline: one frame of the cap after the previous one, or now if the frame ran late.
        static auto s_next = std::chrono::steady_clock::now();
        const uint32_t cap = runtime->framePacer().cap();
        const auto now = std::chrono::steady_clock::now();

        // JC2_FPS=1: the frame's work (from the previous deadline to this frame end, guest threads included), mean and
        // worst over 300 frames: the headroom below the cap's frame time.
        static const bool s_logWork = [] { const char *v = std::getenv("JC2_FPS"); return v && v[0] == '1'; }();
        if (s_logWork)
        {
            static double s_sum = 0.0, s_worst = 0.0;
            static uint32_t s_count = 0;
            const double work = std::chrono::duration<double, std::milli>(now - s_next).count();
            s_sum += work;
            s_worst = std::max(s_worst, work);
            if (++s_count == 300u)
            {
                std::fprintf(stderr, "[jc2:work] frame work %.2f ms mean, %.2f ms worst (300 frames)\n", s_sum / s_count, s_worst);
                s_sum = s_worst = 0.0;
                s_count = 0;
            }
        }
        s_next = cap == 0u ? now : std::max(now, s_next + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                               std::chrono::duration<double>(1.0 / cap)));
        runtime->eeScheduler().waitHostDeadline(
            s_next,
            [runtime]
            {
                EeScheduler &scheduler = runtime->eeScheduler();
                static uint64_t s_lastFrameCycle = scheduler.currentCycle();
                static auto s_lastFrame = std::chrono::steady_clock::now();
                const auto woke = std::chrono::steady_clock::now();
                const double seconds = std::min(0.1, std::chrono::duration<double>(woke - s_lastFrame).count());
                s_lastFrame = woke;
                const uint64_t accounted = scheduler.currentCycle() - s_lastFrameCycle;
                const uint64_t realCycles = static_cast<uint64_t>(seconds * kEeClockHz);
                if (realCycles > accounted)
                    scheduler.accountCycles(static_cast<uint32_t>(std::min<uint64_t>(realCycles - accounted, 0xFFFFFFFFu)));
                s_lastFrameCycle = scheduler.currentCycle();
                scheduler.presentGuestFrame();
            },
            [runtime, parityField, ra](R5900Context &resume)
            {
                const uint32_t field = s_parityField ? parityField
                                                     : static_cast<uint32_t>((runtime->memory().gs().csr.load() >> 13) & 1u);
                setReturnU32(&resume, field);
                resume.pc = ra;
            });
    }

    // JC2_FPS=1 (and the native loop): the game's frames per second, read from a host thread every 5 s: the flip's
    // frame count (0x6D6190) and the frames that missed their vblank (0x6D61B0). Wrapping the flip instead broke the
    // game (boot 74): it sleeps in its frame wait, where the runtime suspends the thread.
    constexpr uint32_t kFrameCount = 0x006D6190u;
    constexpr uint32_t kLateFrames = 0x006D61B0u;

    // JC2_DUMP=<hex address>,<bytes>,<seconds>,<file> (bring-up aid): after that many seconds, writes that much guest
    // RAM to the file, once (e.g. the movie audio ring 0x18A0000, while the New-game movie plays).
    void dumpThread(PS2Runtime *runtime, uint32_t address, uint32_t bytes, double seconds, std::string path)
    {
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        if (runtime->isStopRequested())
            return;
        const uint8_t *rdram = runtime->memory().getRDRAM();
        if (FILE *out = std::fopen(path.c_str(), "wb"))
        {
            std::fwrite(rdram + (address & 0x1FFFFFFu), 1, std::min<uint32_t>(bytes, 0x2000000u - (address & 0x1FFFFFFu)), out);
            std::fclose(out);
            std::fprintf(stderr, "[jc2:dump] 0x%x + 0x%x -> %s\n", address, bytes, path.c_str());
        }
    }

    void fpsThread(PS2Runtime *runtime)
    {
        uint8_t *const rdram = runtime->memory().getRDRAM();
        auto last = std::chrono::steady_clock::now();
        uint32_t lastFrames = rd32(rdram, kFrameCount), lastLate = rd32(rdram, kLateFrames);
        while (!runtime->isStopRequested())
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            const auto now = std::chrono::steady_clock::now();
            const uint32_t frames = rd32(rdram, kFrameCount), late = rd32(rdram, kLateFrames);
            const double seconds = std::chrono::duration<double>(now - last).count();
            std::fprintf(stderr, "[jc2:fps] %.1f fps (%u frames, %u late, in %.1f s)\n", (frames - lastFrames) / seconds,
                         frames - lastFrames, late - lastLate, seconds);
            last = now;
            lastFrames = frames;
            lastLate = late;
        }
    }

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
        {
            const char *trace = std::getenv("JC2_TRACE_PCM");
            const char *queue = std::getenv("JC2_PCM_QUEUE");
            g_pcmTraceLeft = trace && *trace ? static_cast<uint32_t>(std::strtoul(trace, nullptr, 0)) : 0u;
            g_pcmQueueOn = queue && queue[0] == '1';
            if ((g_pcmQueueOn || g_pcmTraceLeft != 0u) && (g_pcmCallback = runtime.lookupFunction(kPcmCallback)))
                runtime.registerFunction(kPcmCallback, &pcmCallbackTrace);
        }
        if (const char *trace = std::getenv("JC2_TRACE_SCRIPT"); trace && *trace == '1')
        {
            for (const uint32_t address : kScriptCommands)
            {
                if (PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address))
                {
                    g_scriptCommands[address] = original;
                    runtime.registerFunction(address, &scriptCommandTrace);
                }
            }
        }
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
        // Movie audio reaches the game as far ahead as the libmpeg HLE demuxes video (by default up to 128 decoded
        // pictures, about 4 s), more than JC2's movie audio ring holds: the IOP played some of it before the game had
        // byte-swapped it (the PSS audio is big-endian PCM), and the New-game movie came out as noise, blocks in the
        // right and the wrong byte order (boots 102-104). A lead of a few pictures, as TMB's.
        {
            size_t picturesAhead = 6u;
            if (const char *value = std::getenv("JC2_MOVIE_PICTURES_AHEAD"); value && *value)
                picturesAhead = static_cast<size_t>(std::max(1, std::atoi(value)));
            ps2_stubs::setMpegMaxPicturesAhead(picturesAhead);
        }
        const char *native = std::getenv("JC2_NATIVE");
        const char *fps = std::getenv("JC2_FPS");
        if (const char *dump = std::getenv("JC2_DUMP"); dump && *dump)
        {
            char *end = nullptr;
            const uint32_t address = static_cast<uint32_t>(std::strtoul(dump, &end, 16));
            const uint32_t bytes = end && *end == ',' ? static_cast<uint32_t>(std::strtoul(end + 1, &end, 0)) : 0u;
            const double seconds = end && *end == ',' ? std::strtod(end + 1, &end) : 0.0;
            if (bytes != 0u && end && *end == ',')
                std::thread(dumpThread, &runtime, address, bytes, seconds, std::string(end + 1)).detach();
        }
        if ((native && native[0] == '1') || (fps && fps[0] == '1'))
            std::thread(fpsThread, &runtime).detach();
        if (native && native[0] == '1')
        {
            // JC2_NATIVE_FRAMES=0 keeps the vblank presentation (a bring-up split: the frame wait replaced, the
            // frames still latched at the vblanks).
            const char *frames = std::getenv("JC2_NATIVE_FRAMES");
            runtime.eeScheduler().setNativeFrames(!frames || frames[0] != '0');
            runtime.registerFunction(kFrameWait, &nativeFrameEnd);
            std::cout << "[jc2] native loop: frames paced by the host (cap " << runtime.framePacer().cap()
                      << " fps, 0 = unlimited)" << std::endl;
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

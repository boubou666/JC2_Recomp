# Jade Cocoon 2 (NTSC-U, SLUS-20309) recompilation notes

## The disc

| File | Notes |
|---|---|
| `SLUS_203.09` | SHA-1 `fa778a8dcb5a16e8146976fe4ab444856f04c47b`, stripped (empty `.symtab`), one 6 MB `WAX` segment at `0x00100000`, entry `0x00100008` |
| `CDDATA.000` / `CDDATA.LOC` | 1.3 GB data archive and its index |
| `IRX/` | `IOPRP23.IMG`, Sony modules (`LIBSD`, `MCMAN/MCSERV`, `PADMAN`, `MTAPMAN`, `SIO2MAN`), HDD stack (`ATAD`, `DEV9`, `HDD`, `PFS`), i.mode (`IMODEDRV`, `IMODEMAN`, `M2IMODE`), and Genki's own `M2IOPSYS`, `M2SOUND`, `MODHSYN`, `MODMIDI`, `UAMAN` |
| `MOV/`, `VOICE/` | movies and voice streams |

`SYSTEM.CNF` sets `HDDUNITPOWER = NICHDD`, so the game may probe the HDD.

## Pipeline

Same as the other ports on the fork (TLOTR_TTA_Recomp scripts): `extract_iso.sh` → `build_tools.sh` → `ghidra.sh`
(the function map, needed because the ELF is stripped) → `analyze.sh` (SDK stubs) → `recomp.sh` → `build_runtime.sh`
→ `run.sh`.

## Log

### Day 0 (2026-10-27)

- Fork at `boubou666/PS2Recomp@c0b08a0`; it already has `58b5c8b`, `02398e0`, `9dc213a`, `ebc558a` and `281f7d9`.
- `ps2_analyzer`: 244 SDK functions to stub, 324 recognised without a runtime handler, 40 jump tables.
- Ghidra headless: 72,636 entries. The first recompile had 69,751 unhandled instructions, all in data
  (0x500000 and up) that shares the code's segment. `tools/filter_ghidra_map.py` drops the entries in the data spans
  (`ghidra.sh` runs it); the recompile is then clean: 72,296 functions, 252 stubs, 0 errors, 6,044 unresolved
  JR/JALR handled by fallback entries. `ps2_vu_aot`: 9 VU1 microcode fragments.
- Code ranges: `0x00100000-0x004E3E80` (ends with a kernel-mode handler at `0x4E3DE8`, `lui k0,0x8007` / `eret`,
  probably copied into kernel memory) and `0x006B2C10-0x006B9310`.
- **Overlays:** the ELF carries ~1,390 zero-size LOAD segments at `0x00940000` (memsz 0x800-0x16800), so the game
  loads code there at runtime. That code is not in the ELF and is not recompiled yet.
- Runner build: 16 min at -j16 (2,468 steps), debug info split off.

## Boot

1. `SetupHeap base=0x97e800`, then at once `[guest-branch:missing-target] JALR 0x10dc04 -> 0x6b2b00`: crt0 runs the
   static constructors (table `0x6B9310-0x6B9764`, 277 pointers) and the first one, `0x6B2B00`, was in a range the
   map filter had dropped as data. The late code block starts at `0x6B2B00` (a pointer table and zero padding before
   it), not at `0x6B2C10`; every other constructor was already in the code ranges. Filter fixed; `ghidra.sh` now keeps
   the unfiltered export in `build/ghidra/functions_unfiltered.csv`.
2. The constructors run; `SifInitRpc`, thread 2 (entry `0x150A60`, priority 4), two GS clear packets and display
   uploads. Then the main thread spins at `0x1249B0`: `FUN_00124910` binds cdvdfsv RPC `0x8000059A`
   (`CD_SERVER_DISKREADY`) and retries while `client->server` stays 0. Nothing serves it: the game binds it before
   any IOP reset or module load, so it expects the boot ROM's CDVDFSV, and the analyzer did not recognise this
   function. It is libcdvd's `sceCdDiskReady` (mode in a0, `sceSifCallRpc` at 0x11CFC8, returns the reply word), now
   stubbed to the runtime's handler. The other cdvdfsv binders: `0x80000592` in `sceCdInit` and `0x80000597` in
   `sceCdSearchFile` (both stubbed); `0x80000595` (0x1241A8) and `0x80000593` (0x1244C0) are only called from stubbed
   functions.
3. Past the disc check: `sceCdSearchFile("\CDDATA.LOC;1")` (lsn 0x100000, 0x14814 bytes) and `sceCdRead` of its 42
   sectors to `0x97F1E0` from the loader thread (2, `0x150A60`). The main thread then waits in `WaitSema`
   (`0x119F20`, called from `0x485BD0` on the semaphore its argument points to) for the rest of the run.
   Inlined-stub scan (FFOP's generic recompiler bug: a stub address turned into a goto label): 0 in JC2.
4. Why it waits (boots 4-5, `PS2X_STATE_DUMP` and `JC2_TRACE_SYNC=1`): both EE threads wait on semaphore 4, and
   the IOP has run no instruction (JC2 loads no module yet). Semaphore 4 (`0x6C4764`) is the loader's mutex and
   semaphore 5 (`0x6C4768`) its request event; the helpers are wait `0x485BD0(&id)` and signal `0x485C30(&id, isr)`.
   The loader (thread 2) takes the mutex and issues `sceCdRead`; the HLE completes it at once and queued the
   `sceCdCallback` function `0x1509E0` as an interrupt, which ran on top of the loader (thread=2, ra=0) before
   `sceCdRead` returned. Its first step is to take the mutex the loader holds: deadlock, and the main thread queues
   behind it. On hardware `sceCdInitEeCB` gives libcdvd a callback thread, which just waits for the release; the
   runtime's `sceCdInitEeCB` did nothing. Fork fix on `jc2/cd-callback-thread`: `sceCdInitEeCB(priority, stack,
   size)` creates that thread, and read completions start the callback on it (one at a time).
5. Boot 6, on the fix: the CD callback now runs on its own thread (3), and the first overlay image is captured
   (`build/overlays/ovl_fa0b7fafeef6b3f2.bin`, 0x40000 bytes at 0x940000). Still stuck, differently: the loader
   (2) waits on semaphore 5 while it holds mutex 4, and the callback, which signals 5, waits for 4 (main too).
   Just before, the loader's `SignalSema(4)` (`0x485C30` from `0x15036C`) returned -1, a signal refused as over
   the maximum: if the runtime has the wrong maximum for 4, a release is lost there. Boot 7 traces `CreateSema`.
6. Boot 7 (`CreateSema` traced): semaphores 1-4 are all `init 1, max 1` (4 from `0x485B10`, the mutex). The
   runtime reads them right; JC2 just signals over the maximum as a matter of course (the first `SignalSema`, on
   3, already returns -420 `KE_SEMA_OVF`). With the maximum enforced, the loader's release-before-take of mutex 4
   is dropped, it then holds the only token while it waits on 5, and the callback, which signals 5, can never
   take 4: a deadlock that does not depend on timing. The retail game runs, so the EE kernel most likely does not
   cap the count. Fork branch `jc2/ee-sema-overflow`: EE `SignalSema` counts past `max_count` (IOP untouched).
   Merged to main as 5b67c2b. Boot 12 still logs it for semaphores 3 and 4.
7. Boots 8-10: M2IOPSYS binds the EE servers 0x10001/0x10003 (IOP -> EE RPC, merged) and needs the EE -> IOP
   handoff (`jc2/ee-iop-handoff`, local, tests in, not pushed). Then EE thread 1 ends with pc 0, ra 0.
8. Boots 11-14, why main ends. Not the scene loop: its state (`0x6C47B0`) stays 0, and main never calls `0x15C140`.
   Main's first call `0x15C0E0` saves ra (0x141D28) at 0x1FFFFE0 (its sp, main's stack is 0x1F00000-0x2000000),
   runs `0x15B780`, `0x15C210` and `0x165390` to the end, then returns to 0: the slot was zeroed meanwhile, by thread
   1 itself (boot 13, `JC2_WATCH`). Boot 14 (`JC2_GUARD`, a guard page on that host page) names the writer:
   `ps2_syscalls::ReferThreadStatus` (Thread.cpp:375, `*status = {}`), once per frame (every ~4.92M EE cycles), with
   its status struct at about 0x1FFFFD0. That is a handler's frame on an invocation stack: interrupt, alarm and
   callback invocations queued with sp 0 get `EeScheduler::invocationStackTop()`, which takes stacks from
   `PS2Runtime::reserveAsyncCallbackStack`, and those start at `m_asyncCallbackStackTop = PS2_RAM_SIZE`.
   `configureReservedMemory`, which would lower that top and set a floor, has no caller. So the first
   invocation stack is 0x1FFC000-0x1FFFFF0: the top of main's stack. JC2's per-frame handler (the vblank
   interrupt, or its alarm) writes over main's frames. Fix to make in the fork: keep the invocation stacks out of
   the main thread's stack, either by calling `configureReservedMemory` with a region below the main stack (the
   ELF's `SetupThread` stack/size give it), or by reserving them from the guest heap. Any port whose main stack is
   at the top of RAM (SetupThread stack -1) can be hit the same way.

## Overlays (plan)

- The ELF has no code for `0x940000`: no `lui 0x94`, no `jal` into it, and the data words that point there look like
  asset data. The overlays come from `CDDATA.000` at run time and are reached through registers.
- The fork recompiles overlays as modules (README "Overlay modules"): wrap each image into an ELF at its load
  address, map it with Ghidra, recompile with `module_name` / `module_load` / `module_guard_size`; at run time an
  address with no main-ELF function resolves in the module whose guard bytes match guest memory.
- To do once boot reaches them: capture each image as the game loads it (SCTPT2 does an overlay capture too), then
  one module per image.

## For later (room findings)

- Below the field rate: TMB's vblank hold (fork `346ccc9`, `EeScheduler::setVBlankHold`/`setVBlankHoldLimit`): a game
  override turns it on when the frame's work starts after its vblank wait and off when processing ends, so a frame
  just over 16.7 ms does not wait a whole extra field. Hook points to find in JC2 once it renders.
- Risky HLE (FFOP): `sceVif1PkOpenDirectCode`/`CloseGifTag`/`AddGsAD` broke packets in FFOP; JC2 binds none of them.
- Heap corruption suspect (room, FFOP): if the runtime switches EE threads where hardware never would, two threads
  can carve the same newlib chunk (`__malloc_lock` is a no-op in many PS2 builds). JC2 has a second thread from
  boot (entry `0x150A60`): suspect it first if JC2 shows overlapping allocations.
- To pick up: fork main `0a62202b..61788601` (VU1 AOT about 2.8x faster on the exact path; SC3 40 -> 60 fps). Update
  the submodule, rerun `scripts/recomp.sh` (ps2_vu_aot), full rebuild. Batched with the next runtime change.
- If JC2 dies with "EE invocation stack space exhausted" (VP2S): `EeScheduler::invocationStackTop` keeps one 16 KiB
  stack per (thread, nesting depth) forever, from the reserved range past its HLE pools; enlarge the range.
- Possible generic VU1 race (FFOP, after main's vu1/aot-speed): an intermittent endless microprogram with garbage
  lower ops, as if a fragment was not refreshed after an upload. If JC2 hits a VU1 stall, dump VU1 code memory.
- With `jc2/ee-sema-overflow` (sc3-port): a vblank or interrupt handler that `iSignalSema`s a max-1 semaphore every
  field now accumulates signals while its waiter is late, so the waiter catches up back to back. Doubled game
  ticks after a hitch would point here.
- Sound, expected later (FFOP, room): LIBSD `sceSdVoiceTrans`/`sceSdBlockTrans` stuck busy (-201) and a sound thread
  waiting forever on a semaphore, as if the SPU2 DMA completion interrupt (IOP DMA 4/7) or the handler set with
  `sceSdSetTransIntrHandler` never fires. JC2 loads Sony LIBSD (and Genki's M2SOUND/MODHSYN/MODMIDI on top).
- Renderer (BaldHuman, 2026-10-27): paraLLEl-GS is to be dropped from every port, JC2 included, once sc3-port's GPU
  renderer (gs/gpu-renderer) is done. Do not make JC2 depend on paraLLEl-GS; check JC2 on the CPU renderer and the
  new GPU renderer.
- Known main regression (room, 2026-10-27): merge `6dd26a0` (`sctpt2/ipu-in-dma`) gives VP2S a black screen; fix on
  `sctpt2/ipu-release`. JC2's local integration tree is on `6dd26a0`: a frozen screen around movies points here first.
- Room regression harness (sc3-port, `tools/port_regress.sh` on `tools/port-regress`): each port provides
  `scripts/regress.sh <build-dir> <out-dir>` writing `<out-dir>/summary.txt` (key=value: port, ps2recomp, route_reached,
  exit ok|hang|crash|timeout, duration_s, frames_presented, fps_mean, fps_p10, frames_over_25ms,
  gs_frames_per_5s_median/min, spu2_underruns, error_lines, warning_lines; NA when unknown), `last_frame.png` and
  `run.log`. To add for JC2 once it reaches a scripted scene.
- `ffop/iop-module-start-thread` (FFOP, coming): IRX start routines run on a loader thread, so waits inside them (LIBSD's)
  block instead of returning -418. Also covers the IOP -> EE RPC WAIT call made outside any IOP thread, which
  `jc2/iop-to-ee-rpc` can only log. Pick it up when it lands.

## Missed functions to look for (VP2S, 2026-10-28)

Each one hangs only when the game first calls it: the runtime logs `[guest-branch:missing-target]` and the thread
goes dormant. (1) C++ this-adjusting thunks (`j method` + `addiu a0,a0,-N` + 2 nops, 16-aligned) after .text,
before the vtables; (2) one-instruction virtual methods (`jr ra` + delay slot, padded) in the same place;
(3) branch targets that cross into another function (needs its own entry); (4) overlay modules not on the disc
in a form found statically: hook the game's module init and save them at run time (JC2: 0x940000); (5) a CMake
`file(GLOB)` of module dirs without `CONFIGURE_DEPENDS` leaves new modules out of the link. Scans:
VP2S_Recomp `tools/find_entry_points.py`, `tools/wrap_elf.py`.

## Display and movies (boots 16-29, jc2/native)

- Zoomed/cropped intro and menu: JC2 calls `sceGsSetDefDispEnv(640x224)` before `sceGsResetGraph(interlace,
  frame)`. libgraph builds the display from its parameters, whose defaults (`0x4E2AF0`: 1, 2, 1) are already
  interlaced NTSC frame mode, so DH is 447; the runtime stub read SMODE2 (still 0) and wrote DH 223, and the
  presenter showed the top 112 lines doubled. Fork fix cfbdc01 (stub takes the mode from `g_gparam`).
- The intro story, the title and the long dark part after it are all one movie, `MOV/ADV0.PSS` (one decoder
  instance, 2700 pictures, ~90 s at 30 fps). Each picture is uploaded as 1120 16x16 IMAGE transfers (CT32, TBP
  0x2520) and drawn as one sprite over the 640x224 frame. The dark part's pictures are dark as decoded (GS trace
  `build/gs/dark28.gsr`, rebuilt uploads); not checked against a real console yet. START at the title opens the
  main menu; idling on the menu starts the 3D demo.
- Tools: `PS2X_GS_RECORD` + `tools/gs_trace_dump.py` (decode a trace, rebuild uploads), `ps2_gs_replay --present`
  (picture as the window shows it), `PS2X_MPEG_TRACE_LIMIT`, `PS2X_PAD_SCRIPT` (START opens the menu, not CROSS).

## Sound

- Default SPU2 path: white noise (24k zero crossings/s, RMS 18900 flat, clipping). `PS2X_SPU2_WALLCLOCK=1`
  (now the default in `scripts/run.sh`): music, but ~277 AutoDMA underruns per 75 s (`iop_spu2.cpp`
  `fillInputHalf`): the IOP driver restarts core0's AutoDMA late because the IOP only advances with the EE. The
  fix is the IOP on the PC clock (native layer 2, not written yet).

## Native path, boots 30-51 (2026-10-29 night)

- Native IOP: all 13 IRX recompiled (`scripts/iop_recomp.sh`, fork `tools/iop_recomp.py` from
  `jc2/iop-recomp-scan`: pointer-reached functions, prologues after a function end), `PS2X_IOP_NATIVE=1` by
  default; boot 33 compared them with the interpreter by behaviour: identical SPU2 counters (key-ons, AutoDMA
  bytes, underruns, peak) and identical sound loudness. `PS2X_IOP_NATIVE_CHECK=1` reported nothing, but it only
  checks TMB's voice search (VP2S, 2026-10-29), so it says nothing for JC2's modules. Export the variables: a bare `VAR=1 cmd` inside another `bash -c` did not reach the runner (boot 31).
- Sound: `PS2X_SPU2_WALLCLOCK=1` + `PS2X_IOP_WALLCLOCK=1` by default; the 230 "AutoDMA underruns" per 75 s leave
  no silent gap in the output (boots 33, 37).
- Native GS: `PS2X_GS_RENDERER=native` by default. Fork fix 732b622: the backend wrote depth through
  `gl_Position.z = z*2-1`, which collapses small Z; JC2's movie sprite (Z 1 over a Z 0 clear, ZTST GREATER) was
  dropped. Captures from the native backend are 1280 wide with a white right half (capture buffer, not the screen).
- Movies: libmpeg's `sceMpegIsEnd` (0x118108) was missed by the analyzer; left native it read libmpeg state the HLE
  never sets, so the opening movie never ended (manual stub in the TOML).
- Code overlays: 1383 CodeWarrior `MWo3` modules in CDDATA.000 (`tools/find_overlays.py`; slots 0x940000 (559),
  0x963000 (347), 0x967000, 0x96A800, 0x96B000, 0x972000, 0x976800). `scripts/overlays.sh`: VP2S_Recomp's
  `wrap_elf.py`, `tools/overlay_functions.py` (function map without Ghidra), ps2_recomp module mode (guard: the 64-byte
  header with the module's name) into `generated/overlays/`; one 23-minute build. 1368 build; 15 `SC_*` modules
  (non-empty constructor table) still fail in wrap_elf/ps2_recomp.
- Main ELF: 99 functions reached only through pointers or from overlays added to `entry_points`
  (`tools/overlay_main_targets.py`; boot 46 stopped at 0x194FB0, a leaf after `jr ra; addiu sp` + padding).
- `scripts/recomp.sh` keeps `generated/iop` and `generated/overlays` (`tools/sync_tree.py --keep`).
- Now (boot 48-51): New game -> opening movie -> first in-game 3D scene (the hero with his portrait, no text box),
  which never moves on: no button changes it (boot 50), the main thread runs its scene loop, the loader idles, the
  sound driver (IOP RPC 0x10000, fno 0x102, server 0xAE3E0 in M2SOUND) is polled every frame and the EE keeps
  sending audio data over SIF DMA. Hypothesis: the scene waits for a voice stream to end. Not confirmed.
- First in-game scene (boots 52-56): overlay m2esA001's script (0x940710, run every frame) is a list of command
  calls (0x1EB5D0 for most, interpreter with a jump table at 0x6944A0; also 0x1F4B20, 0x1F3390), each returning
  non-zero while running (the script then yields). It stops at command 0xCB (handler 0x1F0DC4): done when the global
  0x879FAC == 2 (a three-way cursor kept by 0x48BD20, Left/Right; that routine never runs in this scene) or when
  0x477C50(0) returns 0 (0x41DA80: status of one of four slots in a table at 0x8B9860, probably memory-card
  ports/slots). Meanwhile the game finds no save (`GetDir '/BASLUS-20309MAYU2'` -> 0 entries) and polls the card
  (GetInfo + Sync, ~216 times). Hypothesis: a memory-card prompt after "no save found" that is neither drawn nor
  driven. JC2_TRACE_SCRIPT=1 logs the script's commands, JC2_WATCH=879fac the cursor.
- Solved (boots 57-62): the slots at 0x8B9860 are file-load requests (state +0x10, handle +0xC; 0x41D3F0 steps them).
  Slot 0 waited in state 9 for the stream record of its handle (0x89DD40 + handle*64 + 0x280: chunk 0x2000 bytes,
  EE source +0x290, IOP buffer +0x2A4, bytes in flight +0x2B0) to come back to 0 bytes in flight. The EE sends each
  chunk to the IOP with sceSifSetDma (0x486920), and the IOP answers through an EE RPC server (sid 0x10001,
  handler 0x234A60, codes 0x400-0x405; 0x403 = chunk done). The runtime refused that call ("[sifrpc] IOP -> EE
  call sid=0x10001 fno=1027: no EE server queue, refused"): SifRegisterRpc read cfunc, cbuf and qd from the stack,
  while the EE EABI passes them in t0-t2, so the server's queue was 0. Fixed on jc2/native c3ae3e1. Boot 62: the
  scene goes on (the hero walks the room with a "!" bubble, the next shots and portraits follow).
  JC2_WATCH=<addr>,<bytes> logs a whole range when any word of it changes (needs JC2_TRACE_SCENE=1).
- Dialogue text box (boots 62-66): the scene ran, but the text box was never drawn (GS record: no draw at all, on the
  native and the CPU GS alike). One `[guest-branch:missing-target]` gave it away: 0x10DC04 (the static constructor
  walk run when an overlay loads) called 0x964080, SC_A005's static initializer. CodeWarrior keeps the initializers
  at the end of the data, just before the constructor table (header +0x18..+0x1C), and the table follows text + data
  on the disc. `tools/find_overlays.py` cut each module at text + data, so the table was lost and
  `tools/overlay_functions.py` never listed the initializers: 174 modules (SC_* scenes, BTLEF_* battle effects).
  Now the extract keeps the table, the function map lists the initializers, and `tools/wrap_overlay.py` runs VP2S's
  `wrap_elf.py` with JC2's text end (text size + 0x40) so its .init/.ctors sections do not overlap .text. Boot 66:
  the dialogue box and its text show, and the conversation moves on (Levant appears).

## Frame rate and the native frame loop (boots 67-76, 2026-10-29)

- On the vblank grid (the default) JC2 holds 60 fps: `JC2_FPS=1` samples the flip's frame count (0x6D6190) and its
  late frames (0x6D61B0) every 5 s; boot 75: 59.8-60.0 through the intro, menus and the first scene, one dip to 31-49
  during a scene load (143 late frames).
- The frame wait is 0x15B400(target, &late): the vblank handler 0x15B360 counts vblanks (0x6D61AC) and wakes the main
  thread (0x6D61A4 set before it sleeps); a late frame sets the counter back and counts itself; the wait returns the
  CSR FIELD bit (half-line offset of the next field). All flips go through 0x15A150 (called from 0x15BC80).
- `JC2_NATIVE=1` replaces the wait with a FramePacer frame end (as VP2S/TMB): a steady 59.6 fps, the game goes on,
  but every frame is black, with the native or CPU GS, presented at the frame end, after the flip, newest buffer or
  on the vblanks (`JC2_NATIVE_FRAMES=0`, boot 76). Cause: the main thread never sleeps, so the lower-priority threads
  that run while it waits for the vblank starve. A JC2 native loop has to let them run (a real sleep to the next host
  frame) before it can replace the grid. Wrapping the flip 0x15A150 broke the game (boot 74): it sleeps inside.
- VU1: `PS2X_VU1_STATS=1` (jc2/native 1453fde) counts the microcode pairs run compiled and interpreted. Boot 77
  (intro, menus, first scene): 509M pairs compiled by `ps2_vu_aot`, 0 interpreted.
- Movie sound: with sc3's AutoDMA wall-clock change (sc3/native-spu2-adma 4fc53883) JC2's movie soundtracks (PCM
  through core 0's AutoDMA sound data input) came out as white noise (boot 77: 24000 zero crossings/s, ~180
  clipped samples/s); the same build without it is clean (boot 78: ~1000 crossings/s, nothing clipped). Reverted on
  jc2/native (3465dff) and reported; sc3 is fixing it.
- Native frame loop, working (boot 82, default since): the frame end blocks the main thread with
  `EeScheduler::waitExternal`; a host thread sleeps until the frame deadline (FramePacer cap, 1/cap after the previous
  deadline, or now when late) and posts a task that charges the real EE cycles, presents the frame and completes the
  wait. JC2's lower-priority threads run while the main thread waits. 57-60 fps through the intro, menus, opening
  movie and first scene with its dialogue; the scene-load dip is 57 fps (31 on the vblank grid).
- Open (intermittent): boot 84 (native loop, CROSS every 1 s from 200 s) stayed black from the end of the opening
  movie (~220 s) to the end of the run, the game running at 60 fps; boot 85, the same input, played the scene.
  A race; candidate: the IOP's instant CD completions (TTA's tlotr/iop-cd-timing, PS2X_IOP_CD_SECTORS_PER_SECOND).
- Frame cost (boot 90, `JC2_FPS=1` logs the frame work at the cap): menus and movies 2-3 ms mean, the first in-game
  scene 7.5-15.7 ms mean and 18-26 ms worst (60 fps leaves 16.7 ms). Profile of that scene (boot 91,
  `PS2X_PROFILE=50 PS2X_PROFILE_DELAY=230`, symbolized with MSYS2 `addr2line` against `ps2EntryRunner.debug`): the EE
  thread is busy 48%, about half in the recompiled game code; then the VIF unpack (4.7%), PSMT8 texture uploads into
  the CPU GS memory mirror (`GSMem::WriteP8` + `PixelStorageTraits<19>`, ~5.5%), the IOP clock (2.7%) and the VU1
  dispatcher around the AOT microcode (~6%). The GS worker is busy 35%, over half of it on one address of
  `GS::workerLoop` (a spin-wait). Shared runtime layers (TMB's VIF1/adapter work), not JC2 code.

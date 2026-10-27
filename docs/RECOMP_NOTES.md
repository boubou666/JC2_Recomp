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

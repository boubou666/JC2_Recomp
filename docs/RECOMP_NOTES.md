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

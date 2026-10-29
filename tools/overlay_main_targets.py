"""Main-ELF functions that only JC2's code overlays reach, for the TOML entry_points: Ghidra maps the main ELF alone and
never sees the overlays' JALs into it, nor the main-ELF addresses they hold in data or build with LUI + ADDIU/ORI.
Boot 46: after the opening, the game called 0x194FB0 through a pointer and stopped there (missing target).

Targets kept: inside the main ELF's code ranges (as tools/filter_ghidra_map.py), not already a function start in the
map, and a function start themselves: an ADDIU $sp, $sp, -n prologue, or the first instruction after a JR $ra and
its delay slot (JAL targets are kept whatever they start with: a JAL is a call).
  python -I tools/overlay_main_targets.py game/SLUS_203.09 recomp/ghidra/jc2_functions.csv build/overlays_disc
"""
import csv
import glob
import os
import struct
import sys

CODE_RANGES = [(0x100000, 0x4E3E80), (0x6B2B00, 0x6B9310)]  # the ELF's code (tools/filter_ghidra_map.py)
DATA_RANGES = [(0x4E3E80, 0x6B2B00), (0x6B9310, 0x8C2800)]  # and its data
JR_RA = 0x03E00008


def elf_words(path):
    elf = open(path, 'rb').read()
    phoff, = struct.unpack_from('<I', elf, 0x1C)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x2A)
    words = {}
    for i in range(phnum):
        p_type, off, vaddr, _, filesz, _ = struct.unpack_from('<6I', elf, phoff + i * phentsize)
        if p_type == 1 and filesz:
            for o in range(0, filesz - 3, 4):
                words[vaddr + o] = struct.unpack_from('<I', elf, off + o)[0]
    return words


def main():
    words = elf_words(sys.argv[1])
    with open(sys.argv[2]) as f:
        starts = {int(row['Start'], 16) for row in csv.DictReader(f)}

    def in_code(addr):
        return addr % 4 == 0 and any(lo <= addr < hi for lo, hi in CODE_RANGES)

    def function_start(addr):
        w = words.get(addr)
        if w is None:
            return False
        if (w >> 16) == 0x27BD and w & 0x8000:
            return True
        # Skip the zero padding back to the delay slot; JR $ra is just before it (or is it, under a nop slot).
        back = addr - 4
        while words.get(back) == 0 and words.get(back - 4) != JR_RA:
            back -= 4
        return words.get(back - 4) == JR_RA or words.get(back) == JR_RA

    found = {}
    for path in sorted(glob.glob(os.path.join(sys.argv[3], '*.bin'))):
        image = open(path, 'rb').read()
        load, text, _, _ = struct.unpack_from('<4I', image, 8)
        hi = {}
        for off in range(0x80, len(image) - 3, 4):
            addr, w = load + off, struct.unpack_from('<I', image, off)[0]
            op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            code = off < 0x40 + text
            candidates = []
            if code and op == 0x03:
                target = (addr & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if in_code(target):
                    candidates.append((target, True))
            if code and op == 0x0F:
                hi[rt] = (imm << 16, addr)
            elif code and op in (0x09, 0x0D) and rs in hi and addr - hi[rs][1] <= 32:
                candidates.append(((hi[rs][0] + (imm - 0x10000 if op == 0x09 and imm & 0x8000 else imm)) & 0xFFFFFFFF,
                                   False))
            if not code:
                candidates.append((w, False))
            for target, is_call in candidates:
                if in_code(target) and target not in starts and (is_call or function_start(target)):
                    found.setdefault(target, os.path.basename(path))
    # The main ELF's own data words too (function tables): 0x194FB0, a leaf function (no prologue, after padding)
    # held at 0x520C30, was missed by Ghidra and by find_code_pointers.py.
    for lo, hi_end in DATA_RANGES:
        for addr in range(lo, hi_end, 4):
            target = words.get(addr)
            if target is not None and in_code(target) and target not in starts and function_start(target):
                found.setdefault(target, 'main data 0x%x' % addr)
    for target in sorted(found):
        print('  "0x%08X",  # from %s' % (target, found[target]))
    print(len(found), 'main-ELF targets not in the map', file=sys.stderr)


main()

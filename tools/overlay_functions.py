"""Function map (the CSV ps2_recomp reads: Name,Start,End,Size) of a CodeWarrior code overlay found by
tools/find_overlays.py, without Ghidra: JC2's overlays (1383 on the disc) keep code and data apart (.text after a
0x80-byte header, then .data), so their functions can be found from the code itself.

Function starts: the start of .text; every JAL target inside .text; LUI + ADDIU/ORI pairs and data words holding an
address in .text that starts with a prologue (ADDIU $sp, $sp, -n: callbacks, function tables); and the first
instruction after a function's end (JR $ra, its delay slot, zero padding). The last rule partitions all of .text,
so nothing is left out; a function with several returns is cut in pieces, and a branch from one piece into the next
becomes a jump between functions, which ps2_recomp handles.
  python -I tools/overlay_functions.py <overlay>.bin <out>.csv
"""
import struct
import sys

JR_RA = 0x03E00008


def main():
    image = open(sys.argv[1], 'rb').read()
    load, text, data, bss = struct.unpack_from('<4I', image, 8)
    start = load + 0x80
    # .text as tools/wrap_elf.py (VP2S_Recomp) lays it out: from +0x80 to the header's text size + 0x40, past the
    # last instruction (the data that follows is not code).
    end = load + 0x40 + text
    words = {load + off: struct.unpack_from('<I', image, off)[0] for off in range(0, len(image) - 3, 4)}
    word = words.get
    end = min(end, load + (len(image) & ~3))  # a header text size past the file's end (a few BTLEF modules)

    def in_text(addr):
        return start <= addr < end and addr % 4 == 0

    def prologue(addr):
        w = word(addr)
        return w is not None and (w >> 16) == 0x27BD and (w & 0x8000) != 0

    starts = {start}
    hi = {}
    for addr in range(start, end, 4):
        w = word(addr)
        op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 0x03:  # JAL
            target = (addr & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if in_text(target):
                starts.add(target)
        if op == 0x0F:  # LUI
            hi[rt] = (imm << 16, addr)
        elif op in (0x09, 0x0D) and rs in hi and addr - hi[rs][1] <= 32:
            target = (hi[rs][0] + (imm - 0x10000 if op == 0x09 and imm & 0x8000 else imm)) & 0xFFFFFFFF
            if in_text(target) and prologue(target):
                starts.add(target)
        if w == JR_RA:
            nxt = addr + 8
            while nxt < end and word(nxt) == 0:
                nxt += 4
            if nxt < end:
                starts.add(nxt)
    for addr, w in words.items():
        if not in_text(addr) and in_text(w) and prologue(w):
            starts.add(w)

    ordered = sorted(starts)
    with open(sys.argv[2], 'w', newline='\n') as out:
        out.write('Name,Start,End,Size\n')
        for i, s in enumerate(ordered):
            e = ordered[i + 1] if i + 1 < len(ordered) else end
            out.write('FUN_%08x,0x%08X,0x%08X,%d\n' % (s, s, e, e - s))
    print('%s: .text %#x-%#x, %d functions' % (sys.argv[1], start, end, len(ordered)))


main()

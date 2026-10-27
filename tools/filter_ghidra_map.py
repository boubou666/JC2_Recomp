"""Drops the Ghidra map entries that start inside the ELF's data.

SLUS_203.09 has a single loadable segment (code and data together, no section names), so Ghidra's auto-analysis
also disassembles the data and labels it: ~60,000 bogus "functions" that ps2_recomp then fails to translate.
The data spans below were found from the recompile errors (every one sits in them, none in the code ranges).

Usage: python3 -I tools/filter_ghidra_map.py <functions.csv>    (rewritten in place)
"""
import csv
import sys

# [start, end) guest ranges that are data, not code.
DATA_RANGES = [
    (0x004E3E80, 0x006B2B00),  # after the last code (kernel-mode handler at 0x4E3DE8) up to the late code block,
    #                            which starts at the first static constructor (0x6B2B00, ctor table 0x6B9310-0x6B9764)
    (0x006B9310, 0x008C2800),  # after the late code block (last FUN 0x6B92D0) to the end of the segment's bss
]


def in_data(addr):
    return any(lo <= addr < hi for lo, hi in DATA_RANGES)


def main(path):
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    header, body = rows[0], rows[1:]
    kept = [r for r in body if not in_data(int(r[1], 16))]
    with open(path, "w", newline="") as f:
        csv.writer(f, lineterminator="\n").writerows([header] + kept)
    print(f"{path}: kept {len(kept)} of {len(body)} entries ({len(body) - len(kept)} in data dropped)")


if __name__ == "__main__":
    main(sys.argv[1])

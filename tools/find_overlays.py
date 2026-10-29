"""Finds the CodeWarrior code overlays ('MWo3' header) stored in Jade Cocoon 2's CDDATA.000 and writes each one, as
the game loads it at its load address, to <out>/<name>_<load>.bin (header included, code + data and the constructor
table that follows them, without bss).

The header (VP2S_Recomp tools/wrap_elf.py reads the same format): +0 'MWo3', +8 load address, +0xC text size,
+0x10 data size, +0x14 bss size, +0x18/+0x1C the constructor table's start and end, +0x20 the module's file name.
  python -I tools/find_overlays.py game/CDDATA.000 build/overlays_disc
"""
import os
import struct
import sys

MWO3 = 0x336F574D
SECTOR = 2048


def main():
    path, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    found = 0
    with open(path, 'rb') as f:
        offset = 0
        while True:
            f.seek(offset)
            head = f.read(0x40)
            if len(head) < 0x40:
                break
            if struct.unpack_from('<I', head, 0)[0] == MWO3:
                load, text, data, bss, ctors, ctors_end = struct.unpack_from('<6I', head, 8)
                name = head[0x20:0x40].split(b'\0')[0].decode('latin-1')
                size = text + data
                # The constructor table follows the data on the disc (SC_A005: text + data ends at the table, which
                # holds its static initializer 0x964080); the game loads it with the module.
                if load < ctors < ctors_end <= load + size + 0x1000:
                    size = max(size, ctors_end - load)
                if 0x100000 <= load < 0x2000000 and 0 < size < 0x400000 and name.isprintable():
                    f.seek(offset)
                    image = f.read(size)
                    stem = '%s_%06x' % (name.replace('.', '_'), load)
                    with open(os.path.join(out, stem + '.bin'), 'wb') as o:
                        o.write(image)
                    print('%-24s load %#08x text %#07x data %#06x bss %#06x at CDDATA offset %#x' %
                          (name, load, text, data, bss, offset))
                    found += 1
            offset += SECTOR
    print(found, 'overlays')


main()

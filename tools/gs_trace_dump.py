"""Decodes the GIF packets of a GS trace (PS2X_GS_RECORD, runtime/gs/gs_trace.h): tags, A+D / PACKED register writes
with their main fields, and IMAGE transfer sizes. For reading what a game draws, e.g. why a picture is scaled.
  python tools/gs_trace_dump.py <trace.gsr> [first] [count]
"""
import struct
import sys

REG = {0x00: 'PRIM', 0x01: 'RGBAQ', 0x02: 'ST', 0x03: 'UV', 0x04: 'XYZF2', 0x05: 'XYZ2', 0x06: 'TEX0_1', 0x07: 'TEX0_2',
       0x08: 'CLAMP_1', 0x09: 'CLAMP_2', 0x0A: 'FOG', 0x0C: 'XYZF3', 0x0D: 'XYZ3', 0x0E: 'A+D', 0x0F: 'NOP',
       0x14: 'TEX1_1', 0x15: 'TEX1_2', 0x16: 'TEX2_1', 0x17: 'TEX2_2', 0x18: 'XYOFFSET_1', 0x19: 'XYOFFSET_2',
       0x1A: 'PRMODECONT', 0x1B: 'PRMODE', 0x1C: 'TEXCLUT', 0x22: 'SCANMSK', 0x34: 'MIPTBP1_1', 0x3B: 'TEXA',
       0x3D: 'FOGCOL', 0x3F: 'TEXFLUSH', 0x40: 'SCISSOR_1', 0x41: 'SCISSOR_2', 0x42: 'ALPHA_1', 0x43: 'ALPHA_2',
       0x44: 'DIMX', 0x45: 'DTHE', 0x46: 'COLCLAMP', 0x47: 'TEST_1', 0x48: 'TEST_2', 0x49: 'PABE', 0x4A: 'FBA_1',
       0x4B: 'FBA_2', 0x4C: 'FRAME_1', 0x4D: 'FRAME_2', 0x4E: 'ZBUF_1', 0x4F: 'ZBUF_2', 0x50: 'BITBLTBUF',
       0x51: 'TRXPOS', 0x52: 'TRXREG', 0x53: 'TRXDIR', 0x54: 'HWREG', 0x60: 'SIGNAL', 0x61: 'FINISH', 0x62: 'LABEL'}


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def describe(reg, v):
    name = REG.get(reg, hex(reg))
    if name.startswith('TEX0'):
        return f'{name} tbp0={bits(v,0,14):#x} tbw={bits(v,14,6)} psm={bits(v,20,6)} tw={bits(v,26,4)} th={bits(v,30,4)} tcc={bits(v,34,1)} tfx={bits(v,35,2)} cbp={bits(v,37,14):#x}'
    if name.startswith('TEX1'):
        return f'{name} lcm={bits(v,0,1)} mxl={bits(v,2,3)} mmag={bits(v,5,1)} mmin={bits(v,6,3)} l={bits(v,19,2)} k={bits(v,32,12)}'
    if name == 'PRIM':
        return f'PRIM type={bits(v,0,3)} iip={bits(v,3,1)} tme={bits(v,4,1)} fge={bits(v,5,1)} abe={bits(v,6,1)} aa1={bits(v,7,1)} fst={bits(v,8,1)} ctxt={bits(v,9,1)} fix={bits(v,10,1)}'
    if name == 'UV':
        return f'UV u={bits(v,0,14)/16:.2f} v={bits(v,16,14)/16:.2f}'
    if name == 'ST':
        s = struct.unpack('<f', struct.pack('<I', bits(v, 0, 32)))[0]
        t = struct.unpack('<f', struct.pack('<I', bits(v, 32, 32)))[0]
        return f'ST s={s:.5f} t={t:.5f}'
    if name in ('XYZ2', 'XYZF2', 'XYZ3', 'XYZF3'):
        return f'{name} x={bits(v,0,16)/16:.2f} y={bits(v,16,16)/16:.2f} z={bits(v,32,32 if "F" not in name else 24)}'
    if name.startswith('XYOFFSET'):
        return f'{name} ofx={bits(v,0,16)/16:.2f} ofy={bits(v,32,16)/16:.2f}'
    if name.startswith('SCISSOR'):
        return f'{name} x0={bits(v,0,11)} x1={bits(v,16,11)} y0={bits(v,32,11)} y1={bits(v,48,11)}'
    if name.startswith('FRAME'):
        return f'{name} fbp={bits(v,0,9):#x} fbw={bits(v,16,6)} psm={bits(v,24,6)} fbmsk={bits(v,32,32):#x}'
    if name == 'BITBLTBUF':
        return f'BITBLTBUF sbp={bits(v,0,14):#x} sbw={bits(v,16,6)} spsm={bits(v,24,6)} dbp={bits(v,32,14):#x} dbw={bits(v,48,6)} dpsm={bits(v,56,6)}'
    if name == 'TRXPOS':
        return f'TRXPOS ssax={bits(v,0,11)} ssay={bits(v,16,11)} dsax={bits(v,32,11)} dsay={bits(v,48,11)} dir={bits(v,59,2)}'
    if name == 'TRXREG':
        return f'TRXREG rrw={bits(v,0,12)} rrh={bits(v,32,12)}'
    if name == 'TRXDIR':
        return f'TRXDIR xdir={bits(v,0,2)}'
    if name.startswith('CLAMP'):
        return f'{name} wms={bits(v,0,2)} wmt={bits(v,2,2)} minu={bits(v,4,10)} maxu={bits(v,14,10)} minv={bits(v,24,10)} maxv={bits(v,34,10)}'
    if name == 'RGBAQ':
        q = struct.unpack('<f', struct.pack('<I', bits(v, 32, 32)))[0]
        return f'RGBAQ r={bits(v,0,8)} g={bits(v,8,8)} b={bits(v,16,8)} a={bits(v,24,8)} q={q:.4f}'
    return f'{name} {v:#x}'


def packed(reg, lo, hi):
    """PACKED-mode register write as the A+D-equivalent (register, 64-bit value)."""
    if reg == 0x0E:
        return bits(hi, 0, 8), lo
    if reg == 0x01:
        return 0x01, bits(lo, 0, 8) | bits(lo, 32, 8) << 8 | bits(hi, 0, 8) << 16 | bits(hi, 32, 8) << 24
    if reg == 0x02:
        return 0x02, lo
    if reg == 0x03:
        return 0x03, bits(lo, 0, 14) | bits(lo, 32, 14) << 16
    if reg in (0x04, 0x05):
        adc = bits(hi, 47, 1)
        x, y = bits(lo, 0, 16), bits(lo, 32, 16)
        return (0x0D if adc else 0x05) if reg == 0x05 else (0x0C if adc else 0x04), x | y << 16 | bits(hi, 0, 32) << 32
    return reg, lo


def dump_packet(data, out):
    pos = 0
    while pos + 16 <= len(data):
        lo, hi = struct.unpack_from('<QQ', data, pos)
        pos += 16
        nloop, eop, pre, prim, flg, nreg = bits(lo, 0, 15), bits(lo, 15, 1), bits(lo, 46, 1), bits(lo, 47, 11), bits(lo, 58, 2), bits(lo, 60, 4) or 16
        regs = [bits(hi, 4 * i, 4) for i in range(nreg)]
        out.append(f'  TAG nloop={nloop} eop={eop} flg={flg} nreg={nreg} regs={[REG.get(r, hex(r)) for r in regs]}' + (f' {describe(0, prim)}' if pre else ''))
        if flg == 0:
            for _ in range(nloop):
                for r in regs:
                    a, b = struct.unpack_from('<QQ', data, pos)
                    pos += 16
                    reg, val = packed(r, a, b)
                    out.append('    ' + describe(reg, val))
        elif flg == 1:
            for _ in range(nloop * nreg):
                (v,) = struct.unpack_from('<Q', data, pos)
                pos += 8
            pos = (pos + 15) & ~15
            out.append(f'    (REGLIST {nloop}x{nreg})')
        else:
            out.append(f'    IMAGE {nloop * 16} bytes')
            pos += nloop * 16
        if eop:
            break
    return pos


def main():
    path = sys.argv[1]
    first = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    with open(path, 'rb') as f:
        magic, vram_size = struct.unpack('<II', f.read(8))
        assert magic == 0x31525347, 'not a GSR1 trace'
        f.seek(vram_size, 1)
        (state_size,) = struct.unpack('<I', f.read(4))
        f.seek(state_size, 1)
        index = 0
        while True:
            head = f.read(1 + 32 + 4)
            if len(head) < 37:
                break
            kind = head[0]
            (size,) = struct.unpack_from('<I', head, 33)
            data = f.read(size)
            if index >= first + count:
                break
            if index >= first:
                lines = []
                if kind == 0:
                    used = 0
                    while used < len(data):
                        n = dump_packet(data[used:], lines)
                        if n == 0:
                            break
                        used += n
                else:
                    lines.append(f'  kind {kind}, {size} bytes')
                print(f'#{index} {size} bytes')
                print('\n'.join(lines))
            index += 1


main()

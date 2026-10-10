#!/usr/bin/env python3
"""Wrap main.dol as a 32-bit big-endian PowerPC ELF so Ghidra (and REA on
top of it) can load it with every section at its real address.

    python tools/dol2elf.py data/extracted/sys/main.dol build/main.elf [--nop-paired]

--nop-paired replaces Gekko paired-single instructions (primary opcodes 4,
56, 57, 60, 61) with nop in text sections. Stock Ghidra has no Gekko
language, and those words would stop its disassembly; mostly they are
prologue/epilogue register saves, so the decompiled logic survives, but any
paired-single arithmetic is lost. For reading code only.

Text sections become PT_LOAD R+X, data sections R+W; the .bss range from the
DOL header becomes a zero-filled load segment. Entry point is the DOL's.
"""
import struct, sys
from pathlib import Path

def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    nop_paired = '--nop-paired' in sys.argv
    d = bytearray(src.read_bytes())
    offs = struct.unpack_from('>18I', d, 0)
    addrs = struct.unpack_from('>18I', d, 0x48)
    sizes = struct.unpack_from('>18I', d, 0x90)
    bss_addr, bss_size, entry = struct.unpack_from('>3I', d, 0xD8)
    if nop_paired:
        patched = 0
        for i in range(7):
            for o in range(offs[i], offs[i] + sizes[i], 4):
                if sizes[i] and d[o] >> 2 in (4, 56, 57, 60, 61):
                    d[o:o + 4] = bytes.fromhex('60000000')
                    patched += 1
        print(f'replaced {patched} paired-single instructions with nop')
    d = bytes(d)
    segs = []  # (vaddr, file bytes, memsz, flags)
    for i in range(18):
        if sizes[i]:
            segs.append((addrs[i], d[offs[i]:offs[i] + sizes[i]], sizes[i], 5 if i < 7 else 6))
    # .bss overlaps small data sections in some DOLs; only add the part past the last section.
    end = max(a + s for a, _, s, _ in segs)
    if bss_addr + bss_size > end:
        start = max(bss_addr, end)
        segs.append((start, b'', bss_addr + bss_size - start, 6))
    ehsize, phsize = 52, 32
    data_off = ehsize + phsize * len(segs)
    data_off = (data_off + 31) & ~31
    phdrs, blobs, at = b'', b'', data_off
    for vaddr, blob, memsz, flags in segs:
        phdrs += struct.pack('>8I', 1, at if blob else 0, vaddr, vaddr, len(blob), memsz, flags, 32)
        blobs += blob + bytes((-len(blob)) % 32)
        at += len(blob) + ((-len(blob)) % 32)
    ident = b'\x7fELF' + bytes([1, 2, 1, 0]) + bytes(8)  # ELF32, big endian, v1
    ehdr = ident + struct.pack('>HHIIIIIHHHHHH', 2, 20, 1, entry, ehsize, 0, 0x80000000,
                               ehsize, phsize, len(segs), 40, 0, 0)  # EM_PPC = 20
    out = ehdr + phdrs
    out += bytes(data_off - len(out)) + blobs
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(out)
    print(f'{dst}: {len(segs)} segments, entry {entry:#x}, {len(out)} bytes')

if __name__ == '__main__':
    main()

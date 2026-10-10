#!/usr/bin/env python3
"""Look around Custom Robo's main.dol (PowerPC) to find how the game reads
its own data. Research aid only; nothing here is shipped.

    python tools/dol.py xref 0x801c3bb3       # code that builds this address
    python tools/dol.py dis 0x80012345 80     # disassemble N instructions
    python tools/dol.py func 0x80012345       # disassemble the enclosing function

Needs capstone (pip install capstone).
"""
import struct, sys
from pathlib import Path
from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN

DOL = Path(__file__).resolve().parents[1] / 'data/extracted/sys/main.dol'

class _Word:
    def __init__(self, address, raw):
        self.address, self.mnemonic, self.op_str = address, '.word', raw.hex()

class Dol:
    def __init__(self, path=DOL):
        self.d = path.read_bytes()
        offs = struct.unpack_from('>18I', self.d, 0)
        addr = struct.unpack_from('>18I', self.d, 0x48)
        size = struct.unpack_from('>18I', self.d, 0x90)
        self.secs = [(i, o, a, s) for i, (o, a, s) in enumerate(zip(offs, addr, size)) if s]
        self.cs = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)

    def read(self, a, n):
        for i, o, base, s in self.secs:
            if base <= a < base + s:
                return self.d[o + a - base:o + a - base + n]
        return b''

    def text(self):
        for i, o, a, s in self.secs:
            if i < 7:
                yield a, self.d[o:o + s]

    def dis(self, a, n):
        """One instruction per word; Gekko paired-single ops capstone cannot
        decode come back as '.word' so the listing keeps going."""
        out = []
        blob = self.read(a, 4 * n)
        for k in range(0, len(blob) - 3, 4):
            got = list(self.cs.disasm(blob[k:k + 4], a + k))
            out.append(got[0] if got else _Word(a + k, blob[k:k + 4]))
        return out

    def xrefs(self, target):
        """lis rX,hi ; addi/ori rY,rX,lo pairs (within 32 instructions) that make target."""
        hi_s, lo = ((target + 0x8000) >> 16) & 0xFFFF, target & 0xFFFF
        hi_u = target >> 16
        hits = []
        for base, blob in self.text():
            words = struct.unpack(f'>{len(blob) // 4}I', blob)
            for i, w in enumerate(words):
                if w >> 26 != 15 or (w >> 16) & 31:  # lis = addis rD,0,imm
                    continue
                imm, rd = w & 0xFFFF, (w >> 21) & 31
                for j in range(i + 1, min(i + 32, len(words))):
                    x = words[j]
                    op, ra, ximm = x >> 26, (x >> 16) & 31, x & 0xFFFF
                    if ra != rd:
                        continue
                    if (op == 14 and imm == hi_s and ximm == lo) or (op == 24 and imm == hi_u and ximm == lo) \
                            or (op in (32, 34, 36, 38, 40, 44, 48, 50, 52, 54) and imm == hi_s and ximm == lo):
                        hits.append(base + 4 * j)
        return hits

    def func_start(self, a):
        """Walk back to the function prologue (stwu r1,-N(r1))."""
        while True:
            w = struct.unpack('>I', self.read(a, 4))[0]
            if w >> 16 == 0x9421:
                return a
            a -= 4

    def func(self, a, limit=2000):
        start = self.func_start(a)
        out = []
        for ins in self.dis(start, limit):
            out.append(ins)
            if ins.mnemonic == 'blr' and ins.address > a:
                break
        return out

def show(ins_list):
    for i in ins_list:
        print(f'{i.address:08x}  {i.mnemonic:8s} {i.op_str}')

if __name__ == '__main__':
    dol = Dol()
    cmd, args = sys.argv[1], [int(x, 0) for x in sys.argv[2:]]
    if cmd == 'xref':
        for h in dol.xrefs(args[0]):
            print(f'{h:08x} in function {dol.func_start(h):08x}')
    elif cmd == 'dis':
        show(dol.dis(args[0], args[1] if len(args) > 1 else 40))
    elif cmd == 'func':
        show(dol.func(args[0]))

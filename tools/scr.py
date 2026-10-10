#!/usr/bin/env python3
"""Story event scripts: scenario/snr_f_01-30.bin and snr_t_01-20.bin.

    python tools/scr.py                      # one line per script file
    python tools/scr.py snr_f_01 --names     # its named entry points
    python tools/scr.py snr_f_01 --code 2    # one script's words

'SCR\0', big-endian. Every offset in the file is relative to 0x40, which
is where the entry table starts -- not to the start of the file.

  0x00  'SCR\0', u32 file size (this one IS absolute), u32 0, u32 0
  0x10  u32 end of the code and name region, u32 entry count
  0x18  u32 offset of the relocation table, u32 its u32 count
  0x20  the build's timestamp as ASCII, e.g. "2004/ 2/ 4 17:33:18"
  0x40  entry count x (u32 code offset, u32 name offset)

The names are the game's own: m_main_script_check, m_get_xlaser,
m_event_Lucy_house_script_5. They are what makes these files readable --
281 of them in snr_f_01.bin alone.

The relocation table lists every u32 in the file that holds an offset. Its
first entries are the entry table's own fields; the rest sit inside the
bytecode, and those are how a script names another script: the operand is
a pointer to the other script's entry-table record, so `pointer / 8` is
its index. That is also a reliable way to tell a pointer operand from a
number without knowing the opcode.

Two things about the entry table itself: 322 entries across the 50 files
share a code offset with another entry, so a script can have several
names; and 20 entries, all called yobiN ("spare") in snr_t_19.bin, point
into the name region rather than at code. Script.placeholder flags those
-- they are unused slots, not a parsing failure.

The bytecode itself is u16 opcodes with operands whose width varies per
opcode -- floats and pointers both appear. Opcode 0x42 plays a motion
(actor, motion id, flag) and its ids match snr_motion.BIN's 418+ naming.
The commonest opcodes to start a script are 0x08, 0x42, 0x3b and 0xf8.

Recovering the operand widths from the data alone did not work: treating
each script as a word stream that must decode to exactly its own length
leaves far too many consistent readings, even across all 7112 scripts.
They came from the interpreter instead. There is no dispatch loop -- the
interpreter is threaded, so each of the 300 handlers ends by advancing
the pc by its own hardcoded instruction length and jumping through the
handler table at 0x8021c3f4. OPCODE_BYTES below is that set of lengths.

The game also shipped its opcode NAMES: 300 pointers at 0x8021bf44, index
aligned with the handlers, so opcode_names() reads them out of main.dol
the same way motion.py reads motion names. SYS_SET_SCR, CHAR_TALK,
CHAR_MOT_SET, F_CHK_BIT, C_ACT_CHK_BIT and so on.

Instructions are 4-byte aligned: a u16 opcode, a u16 at +2 that is usually
an actor or flag id, then u32 operands -- pointers (which the relocation
table marks) or floats.
"""
import argparse, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from models import FILES

SCENARIO = 'scenario'
BASE = 0x40
OPCODE_COUNT = 300
NAME_TABLE = 0x8021bf44     # 300 char* in main.dol, index = opcode
HANDLER_TABLE = 0x8021c3f4  # 300 handlers, same index

OPCODE_BYTES = (   # index = opcode, value = instruction length in bytes
    8, 4, 4, 8, 16, 4, 4, 4, 4, 4, 8, 4, 20, 4, 4, 4, 20, 4, 4, 4,
    4, 4, 4, 4, 8, 4, 4, 8, 8, 8, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 4, 8, 4, 28, 20, 8, 8, 8, 4, 8,
    16, 12, 20, 12, 16, 8, 8, 4, 8, 4, 4, 4, 8, 8, 4, 4, 8, 12, 4, 32,
    8, 12, 4, 32, 4, 4, 4, 4, 4, 24, 4, 20, 12, 8, 4, 4, 8, 36, 12, 16,
    24, 8, 4, 28, 8, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 36, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 24, 28, 16, 8,
    20, 16, 24, 20, 12, 20, 4, 4, 8, 4, 4, 8, 4, 12, 20, 12, 20, 4, 28, 8,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 20, 24, 20, 8, 8, 8, 8, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 12, 16, 20, 12, 8, 12, 8, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 8, 4, 16, 12, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 12, 8, 8, 8, 8, 8, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 12,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 16, 12, 8, 12, 8, 4, 8, 4, 4, 8, 12,
)

_names = None

def opcode_names():
    """The game's own opcode names, read out of main.dol."""
    global _names
    if _names is None:
        sys.path.insert(0, str(Path(__file__).parent))
        from dol import Dol
        d = Dol()
        ptrs = struct.unpack(f'>{OPCODE_COUNT}I', d.read(NAME_TABLE, 4 * OPCODE_COUNT))
        out = []
        for a in ptrs:
            b = d.read(a, 64)
            out.append(b[:b.index(0)].decode('ascii', 'replace')
                       if b and 0 in b else f'OP_{len(out)}')
        _names = out
    return _names

class Script:
    def __init__(self, index, name, start, end, placeholder=False):
        self.index, self.name, self.start, self.end = index, name, start, end
        self.placeholder = placeholder
    def __repr__(self):
        return f'Script({self.index}, {self.name!r}, {self.start:#x}..{self.end:#x})'

class ScrFile:
    def __init__(self, blob):
        if blob[:4] != b'SCR\0':
            raise ValueError('not a SCR file')
        self.blob = blob
        size, _, _, data_end, count, reloc, nreloc = struct.unpack_from('>7I', blob, 4)
        if size != len(blob):
            raise ValueError(f'header size {size:#x} is not the file size {len(blob):#x}')
        self.size, self.count, self.data_end = size, count, data_end
        self.timestamp = blob[0x20:blob.index(b'\0', 0x20)].decode('ascii', 'replace')
        self.entries = [struct.unpack_from('>2I', blob, BASE + 8 * i) for i in range(count)]
        self.relocs = set(struct.unpack_from(f'>{nreloc}I', blob, BASE + reloc))
        self.names_start = min(n for _, n in self.entries)
        bounds = sorted({a for a, _ in self.entries} | {data_end, self.names_start})
        self.scripts = []
        for i, (code, name) in enumerate(self.entries):
            end = bounds[bounds.index(code) + 1] if bounds.index(code) + 1 < len(bounds) else data_end
            self.scripts.append(Script(i, self.name_at(name), code, end,
                                       placeholder=code >= self.names_start))
    def name_at(self, off):
        at = BASE + off
        return self.blob[at:self.blob.index(b'\0', at)].decode('ascii', 'replace')
    def words(self, script):
        n = (script.end - script.start) // 2
        return struct.unpack_from(f'>{n}H', self.blob, BASE + script.start)
    def pointers(self, script):
        """{offset in the script: referenced entry index} for its pointer operands."""
        out = {}
        for r in self.relocs:
            if script.start <= r < script.end:
                target = struct.unpack_from('>I', self.blob, BASE + r)[0]
                out[r - script.start] = target // 8 if target % 8 == 0 and target // 8 < self.count else None
        return dict(sorted(out.items()))

def disassemble(f, script):
    """[(offset, opcode, operand bytes)] for a script, or None past a bad
    opcode. The offsets are relative to the file's 0x40 base."""
    out, at = [], script.start
    while at < script.end:
        op = struct.unpack_from('>H', f.blob, BASE + at)[0]
        if op >= OPCODE_COUNT:
            return None
        size = OPCODE_BYTES[op]
        if at + size > script.end:
            return None
        out.append((at, op, f.blob[BASE + at + 2:BASE + at + size]))
        at += size
    return out if at == script.end else None

def format_script(f, script, messages=None):
    """A script as readable lines: the game's opcode names, pointer operands
    resolved to the script they name, and CHAR_TALK's message inlined."""
    names = opcode_names()
    ins = disassemble(f, script)
    if ins is None:
        return [f'{script.name}:  <does not disassemble>']
    out = [f'{script.name}:']
    for at, op, operands in ins:
        args, arg0 = [], struct.unpack_from('>H', operands, 0)[0]
        for k in range(2, len(operands), 4):
            word = struct.unpack_from('>I', operands, k)[0]
            if at + 2 + k in f.relocs:
                idx = word // 8
                args.append('-> ' + (f.scripts[idx].name if idx < f.count else f'{word:#x}'))
            elif word and 0x30000000 < word < 0x50000000 or 0xB0000000 < word < 0xD0000000:
                args.append(f'{struct.unpack(">f", struct.pack(">I", word))[0]:g}')
            elif word >= 0x10000:
                # Not a pointer and not a float: these words are two u16s, as
                # CHAR_MOT_SET's (motion id, flag) and CHAR_TALK's (slot, 0).
                args.append(f'{word >> 16},{word & 0xFFFF}')
            else:
                args.append(str(word))
        line = f'  +{at - script.start:04x}  {names[op]:22s} {arg0}'
        if args:
            line += ' ' + ' '.join(args)
        if op == 59 and messages is not None:      # CHAR_TALK
            slot = struct.unpack_from('>H', operands, 2)[0] & 0x7FFF
            text = messages.get(slot)
            if text is not None:
                line += '   ; ' + text.text(escapes=False).replace(chr(10), ' / ')[:70]
        out.append(line)
    return out

def scr_files():
    return sorted((FILES / SCENARIO).glob('*.bin'))

def load(stem):
    p = FILES / SCENARIO / (stem if stem.endswith('.bin') else stem + '.bin')
    return ScrFile(p.read_bytes())

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('stem', nargs='?', help='e.g. snr_f_01')
    ap.add_argument('--names', action='store_true', help='list the entry points')
    ap.add_argument('--code', type=int, metavar='N', help='dump script N as words')
    ap.add_argument('--dis', metavar='N', help='disassemble script N, or a name, or "all"')
    args = ap.parse_args()
    paths = [FILES / SCENARIO / (args.stem + '.bin')] if args.stem else scr_files()
    for p in paths:
        f = ScrFile(p.read_bytes())
        print(f'{p.name}  {f.count:4d} scripts  {len(f.relocs):5d} relocs  built {f.timestamp}')
        if args.names:
            for s in f.scripts:
                print(f'  [{s.index:4d}] {s.name:46s} {s.start:#08x}..{s.end:#08x} '
                      f'{s.end - s.start:6d} bytes')
        if args.dis:
            import bmg
            b = FILES / 'scenario_us' / (p.stem + '.bmg')
            messages = bmg.read_bmg(b.read_bytes())[0] if b.exists() else None
            if args.dis == 'all':
                chosen = [s for s in f.scripts if not s.placeholder]
            elif args.dis.isdigit():
                chosen = [f.scripts[int(args.dis)]]
            else:
                chosen = [s for s in f.scripts if s.name == args.dis]
            for s in chosen:
                print(chr(10).join(format_script(f, s, messages)))
        if args.code is not None:
            s = f.scripts[args.code]
            print(f'  {s.name}  {s.start:#x}..{s.end:#x}')
            ptr = f.pointers(s)
            ws = f.words(s)
            for i in range(0, len(ws), 8):
                tag = ''.join(f' ->{ptr[o]}' for o in ptr if i * 2 <= o < i * 2 + 16)
                print(f'    +{i * 2:04x}  ' + ' '.join(f'{w:04x}' for w in ws[i:i + 8]) + tag)

if __name__ == '__main__':
    main()

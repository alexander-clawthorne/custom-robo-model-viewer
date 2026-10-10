#!/usr/bin/env python3
"""Message text: the game's BMG files and the font that draws them.

    python tools/bmg.py                       # one line per file
    python tools/bmg.py scenario_us/snr_f_01.bmg --text
    python tools/bmg.py --escapes             # tally the escape codes

Nintendo's BMG container, big-endian: 'MESGbmg1', u32 size in 32-byte
units, u32 section count, u32 encoding (3, Shift-JIS), then the sections,
each 'XXXn' + u32 size. Custom Robo uses only two:

  INF1  u16 count, u16 entry size (always 4 here), u32 0, then one u32
        offset into DAT1 per message. Offset 0 means no message: about
        half of every file's slots are empty, so the index is a fixed id
        space the scripts address, not a packed list.
  DAT1  the strings, each NUL-terminated, at those offsets.

Text is Shift-JIS. The US files are mostly ASCII but not entirely: 2155
of their 13233 strings still hold Japanese, which `--japanese` lists.

An escape is 0x1a, a total length byte (counting both), a group byte and
a u16 code, then parameters. **Those parameters routinely contain 0x00**,
so a message cannot be found by scanning for the next NUL -- the scan has
to step over each escape by its length, or strings are cut at their first
escape. Seen here:

  fe 0006  speaker and expression; its two parameters pick the portrait
  fe 0003  end of message / wait for a button
  fe 0007  two parameters, always small; a box or layout option
  fe 0008  no parameters; a pause, and it appears mid-sentence after '...'
  ff 0002  ruby: a flag, padding spaces, then the kana to set above the
           word. 28792 of these in the Japanese files and 1770 in the US
           ones, which is the giveaway that those US strings are untranslated.
  ff 0000  one byte, 1-5: a text colour or size
  ff 0001  a u16, round numbers (0x4b, 0x64, 0x96, 0xc8): a delay
  01 0000  one byte: a sound cue

The names above are read off how they are used, not from the game's code.
"""
import argparse, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from models import FILES

class Message:
    def __init__(self, index, raw):
        self.index, self.raw = index, raw
    def escapes(self):
        """(group, code, parameter bytes) for every escape in the message."""
        out, i = [], 0
        while i < len(self.raw):
            if self.raw[i] == 0x1A:
                n = max(self.raw[i + 1], 2)
                chunk = self.raw[i:i + n]
                code = struct.unpack_from('>H', chunk, 3)[0] if len(chunk) >= 5 else None
                out.append((chunk[2] if n >= 3 else None, code, chunk[5:]))
                i += n
            else:
                i += 1
        return out
    def text(self, escapes=True):
        """Shift-JIS decoded, with escapes shown as {group:code params}."""
        out, i = [], 0
        while i < len(self.raw):
            if self.raw[i] == 0x1A:
                n = max(self.raw[i + 1], 2)
                c = self.raw[i:i + n]
                if escapes:
                    code = struct.unpack_from('>H', c, 3)[0] if len(c) >= 5 else 0
                    out.append('{%02x:%04x%s}' % (c[2], code, ' ' + c[5:].hex() if len(c) > 5 else ''))
                i += n
            else:
                j = i
                while j < len(self.raw) and self.raw[j] != 0x1A:
                    j += 1
                out.append(self.raw[i:j].decode('shift_jis', 'replace'))
                i = j
        return ''.join(out)
    def is_japanese(self):
        return any(b >= 0x80 for b in self._plain())
    def _plain(self):
        out, i = bytearray(), 0
        while i < len(self.raw):
            if self.raw[i] == 0x1A:
                i += max(self.raw[i + 1], 2)
            else:
                out.append(self.raw[i]); i += 1
        return out

def read_bmg(blob):
    """{index: Message} for every non-empty slot, plus the slot count."""
    if blob[:8] != b'MESGbmg1':
        raise ValueError('not a BMG file')
    at, inf, dat = 0x20, None, None
    while at + 8 <= len(blob):
        magic = blob[at:at + 4]
        size = struct.unpack_from('>I', blob, at + 4)[0]
        if size < 8:
            break
        if magic == b'INF1':
            count, entry = struct.unpack_from('>HH', blob, at + 8)
            inf = at + 16
        elif magic == b'DAT1':
            dat, dat_end = at + 8, at + size
        at += size
    if inf is None or dat is None:
        raise ValueError('BMG without INF1 and DAT1')
    out = {}
    for i in range(count):
        off = struct.unpack_from('>I', blob, inf + i * entry)[0]
        if not off:
            continue
        at = dat + off
        while at < dat_end and blob[at] != 0:
            at += max(blob[at + 1], 2) if blob[at] == 0x1A else 1
        out[i] = Message(i, blob[dat + off:at])
    return out, count

def bmg_files():
    return sorted(FILES.rglob('*.bmg'))

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('path', nargs='?', help='a .bmg under data/extracted/files')
    ap.add_argument('--text', action='store_true', help='print every message')
    ap.add_argument('--japanese', action='store_true', help='only untranslated messages')
    ap.add_argument('--escapes', action='store_true', help='tally escape codes')
    args = ap.parse_args()
    paths = [FILES / args.path] if args.path else bmg_files()
    from collections import Counter
    tally = Counter()
    for p in paths:
        msgs, slots = read_bmg(p.read_bytes())
        jp = sum(m.is_japanese() for m in msgs.values())
        if args.escapes:
            for m in msgs.values():
                for g, c, params in m.escapes():
                    tally[(g, c, len(params))] += 1
            continue
        print(f'{p.relative_to(FILES)}  {len(msgs):5d} messages in {slots:5d} slots'
              + (f'  {jp} still Japanese' if jp else ''))
        if args.text or args.japanese:
            for i, m in sorted(msgs.items()):
                if args.japanese and not m.is_japanese():
                    continue
                print(f'  [{i:4d}] {m.text()}')
    if args.escapes:
        for (g, c, n), v in tally.most_common():
            print(f'group {g:#04x} code {c:#06x} {n} param bytes  x{v}')

if __name__ == '__main__':
    main()

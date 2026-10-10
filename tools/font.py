#!/usr/bin/env python3
"""The game's font: message/font.out and message/font.bmc.

    python tools/font.py                     # describe the four banks
    python tools/font.py --sheets build      # write each bank's glyph sheet
    python tools/font.py --render "Hello"    # draw a string with bank 0

font.out uses the same little container as the effect archives: u32 count,
u32 0x20, then count (offset, size) pairs, each pointing at a Nintendo
'FONTbfn1' bank (JUTResFont). Four banks, which together cover Shift-JIS.
A bank's sections:

  INF1  u16 font type, ascent, descent, width, leading
  WID1  u16 first code, u16 last code, then (left, width) bytes per glyph.
        `left` is the left-side bearing inside the 24-wide cell, so a glyph
        is the slice [left, left + width) -- slicing from 0 clips letters
  MAP1  u16 type, u16 first, u16 last, u16 count, then the mapping. Type 0
        is identity, type 1 a 1:1 range, type 2 a u16 glyph index per code
        in the range. A bank carries many of these, one per Shift-JIS run.
  GLY1  u16 first, u16 last, u16 cell width, u16 cell height, u32 sheet
        size, u16 format, u16 columns, u16 rows, u16 sheet width, u16
        sheet height, then the sheet. Columns comes before rows: every bank
        says 32 then its own row count, and 32 cells across is what the
        sheet actually shows.

Every bank here is 24x24 cells in one 768x1024 I4 sheet, 32 cells across,
GameCube-tiled -- gxtex decodes it unchanged. Glyph N sits at
(N % columns, N // columns).

font.bmc is 'MGCLbmc1' with one CLT1 section: u16 count (256), then RGBA8.
Only the first six entries are distinct -- black, red, green, blue, white
and pink -- which is the range the BMG `ff 0000` escape selects from.
"""
import argparse, struct, sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).parent))
import gxtex
from models import FILES

FONT_OUT, FONT_BMC = 'message/font.out', 'message/font.bmc'

def sections(blob, base):
    """(magic, offset, size) for every section of the bank at `base`."""
    count = struct.unpack_from('>I', blob, base + 12)[0]
    at, out = base + 0x20, []
    for _ in range(count):
        magic = blob[at:at + 4]
        size = struct.unpack_from('>I', blob, at + 4)[0]
        out.append((magic, at, size))
        if size < 8:
            break
        at += size
    return out

class Bank:
    def __init__(self, blob, base):
        self.base, self.maps = base, []
        for magic, at, size in sections(blob, base):
            if magic == b'INF1':
                (self.type, self.ascent, self.descent,
                 self.width, self.leading) = struct.unpack_from('>5H', blob, at + 8)
            elif magic == b'WID1':
                first, last = struct.unpack_from('>2H', blob, at + 8)
                self.width_first = first
                self.widths = [struct.unpack_from('>2B', blob, at + 12 + 2 * i)
                               for i in range(last - first + 1)]
            elif magic == b'MAP1':
                kind, first, last, n = struct.unpack_from('>4H', blob, at + 8)
                table = struct.unpack_from(f'>{n}H', blob, at + 16) if kind == 2 else ()
                self.maps.append((kind, first, last, n, table))
            elif magic == b'GLY1':
                (self.first, self.last, self.cell_w, self.cell_h) = struct.unpack_from('>4H', blob, at + 8)
                self.sheet_size = struct.unpack_from('>I', blob, at + 16)[0]
                (self.format, self.columns, self.rows,
                 self.sheet_w, self.sheet_h) = struct.unpack_from('>5H', blob, at + 20)
                self.sheet = blob[at + 0x20:at + size]

    def glyph_index(self, code):
        """Shift-JIS code point -> glyph number in this bank, or None."""
        for kind, first, last, n, table in self.maps:
            if not first <= code <= last:
                continue
            if kind == 2:
                g = table[code - first]
                return None if g == 0xFFFF else g
            return code - first + (0 if kind == 0 else self.first)
        return None

    def image(self):
        """The glyph sheet as (h, w) greyscale."""
        rgba = np.asarray(gxtex.decode(self.sheet, 0, self.sheet_w, self.sheet_h), np.uint8)
        return rgba[..., 0]

    def cell(self, index, sheet=None):
        sheet = self.image() if sheet is None else sheet
        col, row = index % self.columns, index // self.columns
        return sheet[row * self.cell_h:(row + 1) * self.cell_h,
                     col * self.cell_w:(col + 1) * self.cell_w]

    def advance(self, index):
        left, width = self.widths[index - self.width_first]
        return left, width

def read_font(blob=None):
    blob = (FILES / FONT_OUT).read_bytes() if blob is None else blob
    count = struct.unpack_from('>I', blob, 0)[0]
    return [Bank(blob, struct.unpack_from('>2I', blob, 8 + 8 * i)[0]) for i in range(count)]

def palette(blob=None):
    blob = (FILES / FONT_BMC).read_bytes() if blob is None else blob
    n = struct.unpack_from('>H', blob, 0x28)[0]
    return [tuple(blob[0x2C + i * 4:0x30 + i * 4]) for i in range(n)]

def render(text, banks=None, pad=2):
    """Draw a Shift-JIS string with whichever bank holds each character."""
    banks = read_font() if banks is None else banks
    sheets = [b.image() for b in banks]
    cells, height = [], max(b.cell_h for b in banks)
    data = text.encode('shift_jis', 'replace')
    i = 0
    while i < len(data):
        code = data[i]
        if 0x81 <= code <= 0xFE and i + 1 < len(data):     # Shift-JIS lead byte
            code = (code << 8) | data[i + 1]; i += 2
        else:
            i += 1
        for b, sheet in zip(banks, sheets):
            g = b.glyph_index(code)
            if g is not None and g <= b.last:
                left, width = b.advance(g)
                cell = b.cell(g, sheet)
                cells.append(cell[:, left:left + max(width, 1)])
                break
        else:
            cells.append(np.zeros((height, 8), np.uint8))
    out = np.zeros((height, sum(c.shape[1] + pad for c in cells)), np.uint8)
    x = 0
    for c in cells:
        out[:c.shape[0], x:x + c.shape[1]] = c
        x += c.shape[1] + pad
    return out

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--sheets', metavar='DIR', help='write each bank as a PNG')
    ap.add_argument('--render', metavar='TEXT', help='draw a string to build/text.png')
    args = ap.parse_args()
    banks = read_font()
    for i, b in enumerate(banks):
        codes = sum(n for _, _, _, n, _ in b.maps) or (b.last - b.first + 1)
        print(f'bank {i}: glyphs {b.first}-{b.last}  cell {b.cell_w}x{b.cell_h}  '
              f'sheet {b.sheet_w}x{b.sheet_h} fmt {b.format}  {b.columns} columns  '
              f'{len(b.maps)} map sections covering {codes} codes')
    print(f'palette: {len(palette())} colours, first six {palette()[:6]}')
    from PIL import Image
    if args.sheets:
        for i, b in enumerate(banks):
            p = Path(args.sheets) / f'font_bank{i}.png'
            Image.fromarray(b.image()).save(p)
            print('wrote', p)
    if args.render:
        Image.fromarray(render(args.render, banks)).save('build/text.png')
        print('wrote build/text.png')

if __name__ == '__main__':
    main()

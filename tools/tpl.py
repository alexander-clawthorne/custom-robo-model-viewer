#!/usr/bin/env python3
"""Menu and HUD art: the 590 .tpl texture banks.

    python tools/tpl.py                          # one line per file
    python tools/tpl.py sprite/menu.tpl --png build/menu
    python tools/tpl.py --sheet build/tpl.png    # everything as one sheet

TPL is Nintendo's own GameCube texture container, big-endian:

  0x00  u32 0x0020AF30, u32 image count, u32 table offset (0x0c)
  table count x (u32 image header offset, u32 palette header offset;
        the palette offset is 0 for formats that do not use one)
  image header  u16 HEIGHT, u16 WIDTH, u32 format, u32 data offset,
        then wrap, filter, LOD fields
  palette header  u16 entries, u16, u32 format, u32 data offset

Height comes before width. Reading the pair the other way still decodes
without error -- gxtex does not validate -- so it was settled by checking
each image's data against the gap to the next image's data: height-first
accounts for all 758 images in the first 40 files, width-first for 574.

The pixel formats are the ordinary GameCube set that gxtex.py already
decodes, palettes included, so there is nothing new to decode here.
"""
import argparse, struct, sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).parent))
import gxtex
from models import FILES

TPL_MAGIC = 0x0020AF30

class Image_:
    def __init__(self, width, height, fmt, data, tlut):
        self.width, self.height, self.format = width, height, fmt
        self.data, self.tlut = data, tlut
    def rgba(self):
        return np.asarray(gxtex.decode(self.data, self.format, self.width,
                                       self.height, self.tlut), np.uint8)

def read_tpl(blob):
    magic, count, table = struct.unpack_from('>3I', blob, 0)
    if magic != TPL_MAGIC:
        raise ValueError('not a TPL')
    out = []
    for i in range(count):
        ih, ph = struct.unpack_from('>2I', blob, table + 8 * i)
        if not ih:
            continue
        height, width, fmt, data = struct.unpack_from('>HHII', blob, ih)
        tlut = None
        if ph:
            entries, _, pfmt, poff = struct.unpack_from('>HHII', blob, ph)
            tlut = (blob[poff:poff + entries * 2], pfmt, entries)
        out.append(Image_(width, height, fmt, blob[data:], tlut))
    return out

def tpl_files():
    return sorted(FILES.rglob('*.tpl'))

def contact_sheet(images, columns=24, cell=64):
    """Every image shrunk into one grid, for looking at a whole set at once."""
    from PIL import Image as PILImage
    rows = (len(images) + columns - 1) // columns
    sheet = PILImage.new('RGBA', (columns * cell, rows * cell), (20, 24, 32, 255))
    for i, im in enumerate(images):
        try:
            a = im.rgba()
        except Exception:
            continue
        pic = PILImage.fromarray(a)
        pic.thumbnail((cell - 2, cell - 2))
        sheet.paste(pic, ((i % columns) * cell + 1, (i // columns) * cell + 1), pic)
    return sheet

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('path', nargs='?', help='a .tpl under data/extracted/files')
    ap.add_argument('--png', metavar='PREFIX', help='write every image as a PNG')
    ap.add_argument('--sheet', metavar='FILE', help='one contact sheet of everything')
    args = ap.parse_args()
    paths = [FILES / args.path] if args.path else tpl_files()
    from collections import Counter
    formats, total, failed, gathered = Counter(), 0, [], []
    for p in paths:
        try:
            images = read_tpl(p.read_bytes())
        except Exception as e:
            failed.append((p.name, repr(e)[:50]))
            continue
        for im in images:
            formats[im.format] += 1
            total += 1
        if args.sheet:
            gathered += images
        if args.path:
            print(f'{p.relative_to(FILES)}  {len(images)} images')
            for i, im in enumerate(images):
                print(f'  [{i:3d}] {im.width:4d}x{im.height:<4d} format {im.format:2d}'
                      + (f'  palette {im.tlut[2]}' if im.tlut else ''))
        if args.png:
            from PIL import Image as PILImage
            out = Path(args.png)
            out.mkdir(parents=True, exist_ok=True)
            for i, im in enumerate(images):
                PILImage.fromarray(im.rgba()).save(out / f'{p.stem}_{i:03d}.png')
    if not args.path:
        print(f'{len(paths)} TPL files, {total} images')
        print('formats:', dict(sorted(formats.items())))
        if failed:
            print('failed:', failed[:5], len(failed))
    if args.sheet:
        contact_sheet(gathered).save(args.sheet)
        print('wrote', args.sheet, f'({len(gathered)} images)')

if __name__ == '__main__':
    main()

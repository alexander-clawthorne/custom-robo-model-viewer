#!/usr/bin/env python3
"""Battle HUD sprites: the nameplate over each robot, from sprite/us_battle.tpl.

    PYTHONUTF8=1 python tools/hud.py        # -> romfs/hud/battle.bin (src/hud.c)

The plate (image 40, 102x32 at the GameCube's 640x480) has three slots: a
ring on the left for the player tag (43: 1P 2P 3P 4P CPU, 27x14 cells), a
slanted box for HP in the small digits (42: 0-9, 13x14 cells) and a strip
along the bottom that holds the 80x4 bar (41), which is the down gauge.
Image 44 is the plate's dark backing; 47 and 50 are the DOWN and REBIRTH
words. GUESS: which slot shows what is read from the art, not from the
game's HUD code, which has not been traced.

romfs/hud/battle.bin, little-endian:
  'HUD1', u16 atlas width, u16 atlas height, u32 sprite count,
  count x (u16 x, y, w, h) in atlas pixels, top row first,
  then the atlas as a PICA RGBA8 texture (8x8 Morton tiles, top row first).
"""
import struct, sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).parent))
from cook import tile
from models import FILES
from tpl import read_tpl

OUT = Path(__file__).resolve().parents[1] / 'romfs/hud'
# Order is the HUD_* enum in src/hud.h.
SPRITES = [44, 40, 41, 42, 43, 47, 50]
ATLAS_W, ATLAS_H = 256, 128

def pack(images):
    """Shelf packing, one pixel of clear border so bilinear filtering does
    not bleed one sprite into the next."""
    x = y = shelf = 0
    rects = []
    for im in images:
        w, h = im.shape[1], im.shape[0]
        if x + w + 1 > ATLAS_W:
            x, y, shelf = 0, y + shelf + 1, 0
        rects.append((x + 1, y + 1, w, h))
        x += w + 1
        shelf = max(shelf, h + 1)
    if y + shelf > ATLAS_H:
        raise SystemExit('HUD atlas overflow')
    return rects

def main():
    images = read_tpl((FILES / 'sprite/us_battle.tpl').read_bytes())
    sprites = [images[i].rgba() for i in SPRITES]
    rects = pack(sprites)
    atlas = np.zeros((ATLAS_H, ATLAS_W, 4), np.uint8)
    for (x, y, w, h), im in zip(rects, sprites):
        atlas[y:y + h, x:x + w] = im
    r, g, b, a = (atlas[..., i].astype(np.uint32) for i in range(4))
    px = (r << 24 | g << 16 | b << 8 | a).astype('<u4')
    out = bytearray(b'HUD1' + struct.pack('<HHI', ATLAS_W, ATLAS_H, len(rects)))
    for rect in rects:
        out += struct.pack('<4H', *rect)
    out += tile(px).tobytes()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'battle.bin').write_bytes(out)
    print(f'{OUT / "battle.bin"}: {len(rects)} sprites, {len(out)} bytes')

if __name__ == '__main__':
    main()

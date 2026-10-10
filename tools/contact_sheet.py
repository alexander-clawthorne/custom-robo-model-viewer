#!/usr/bin/env python3
"""Tile emulator screenshots into one small image, so a test run can be
checked by looking at a single picture.

    python tools/contact_sheet.py out.png shot_40.png shot_41.png ...
    python tools/contact_sheet.py out.png --dir shots/ --from 40 --to 52

Each shot is scaled to --width pixels (default 200), 6 per row, labelled
with its file name.
"""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw

def main():
    a = argparse.ArgumentParser()
    a.add_argument('out')
    a.add_argument('shots', nargs='*')
    a.add_argument('--dir')
    a.add_argument('--from', dest='start', type=int, default=0)
    a.add_argument('--to', dest='end', type=int, default=10**6)
    a.add_argument('--width', type=int, default=200)
    a.add_argument('--cols', type=int, default=6)
    args = a.parse_args()
    paths = [Path(p) for p in args.shots]
    if args.dir:
        found = sorted(Path(args.dir).glob('*.png'))
        paths += [p for p in found if args.start <= int(''.join(c for c in p.stem if c.isdigit()) or 0) <= args.end]
    tiles = []
    for p in paths:
        im = Image.open(p).convert('RGB')
        im = im.resize((args.width, round(im.height * args.width / im.width)))
        ImageDraw.Draw(im).text((4, 4), p.stem, fill=(255, 255, 0))
        tiles.append(im)
    if not tiles:
        raise SystemExit('no shots')
    th = max(t.height for t in tiles)
    rows = (len(tiles) + args.cols - 1) // args.cols
    sheet = Image.new('RGB', (args.width * min(args.cols, len(tiles)), th * rows))
    for i, t in enumerate(tiles):
        sheet.paste(t, (args.width * (i % args.cols), th * (i // args.cols)))
    sheet.save(args.out)
    print(f'{len(tiles)} shots -> {args.out}')

if __name__ == '__main__':
    main()

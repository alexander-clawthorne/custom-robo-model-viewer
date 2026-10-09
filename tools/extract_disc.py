#!/usr/bin/env python3
"""Extract every file from the user's own Custom Robo (USA, GXCE01) GameCube ISO.

    python tools/extract_disc.py "Custom Robo (USA).iso" data/extracted

GameCube discs are unencrypted: header at 0, FST offset/size at 0x424/0x428.
Nothing from the disc is committed; data/ is ignored by git.
"""
import argparse, json, struct
from pathlib import Path

def walk_fst(fst):
    count = struct.unpack_from('>I', fst, 8)[0]
    names = count * 12
    stack = [('', count)]
    for i in range(1, count):
        while i >= stack[-1][1]:
            stack.pop()
        info, offset, size = struct.unpack_from('>III', fst, i * 12)
        at = names + (info & 0xFFFFFF)
        name = fst[at:fst.index(0, at)].decode('shift_jis')
        if '/' in name or name in ('.', '..'):
            raise ValueError('unsafe FST name: ' + name)
        name = stack[-1][0] + name
        if info >> 24:
            stack.append((name + '/', size))
        else:
            yield name, offset, size

def main():
    a = argparse.ArgumentParser()
    a.add_argument('disc', type=Path)
    a.add_argument('output', type=Path)
    args = a.parse_args()
    with args.disc.open('rb') as f:
        header = f.read(0x440)
        game_id = header[:6].decode('ascii')
        if game_id != 'GXCE01':
            raise SystemExit(f'Expected Custom Robo USA (GXCE01), got {game_id}')
        dol_offset, fst_offset, fst_size = struct.unpack_from('>III', header, 0x420)
        f.seek(fst_offset)
        fst = f.read(fst_size)
        files = list(walk_fst(fst))
        # main.dol: header gives section offsets/sizes; total = max(offset+size).
        f.seek(dol_offset)
        dol_header = f.read(0x100)
        offs = struct.unpack_from('>18I', dol_header, 0)
        sizes = struct.unpack_from('>18I', dol_header, 0x90)
        dol_size = max(o + s for o, s in zip(offs, sizes) if s)
        out = args.output
        (out / 'sys').mkdir(parents=True, exist_ok=True)
        f.seek(dol_offset)
        (out / 'sys/main.dol').write_bytes(f.read(dol_size))
        (out / 'sys/boot.bin').write_bytes(header)
        for name, offset, size in files:
            dest = out / 'files' / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            f.seek(offset)
            dest.write_bytes(f.read(size))
    receipt = {'game_id': game_id, 'revision': header[7], 'files': len(files), 'dol_bytes': dol_size}
    (out / 'extraction-receipt.json').write_text(json.dumps(receipt, indent=1) + '\n', encoding='utf-8')
    print(json.dumps(receipt))

if __name__ == '__main__':
    main()

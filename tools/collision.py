#!/usr/bin/env python3
"""Arena collision from the odd entries of bg_data.BIN.

    python tools/collision.py            # summarise all 40 arenas
    python tools/collision.py 0 --faces  # list arena 0's faces
    python tools/collision.py --maps     # the 94 story maps use it too

Arena N is SFD entry 2N (the HSD scene, see models.py) and entry 2N+1,
which holds collision only -- no HSD header, just offsets from its start:

  u32 count      collision blocks
  u32 0x10       offset of u32[count] block-header offsets
  u32            offset of u32[count] block face-data offsets
  u32            offset of the trailing section, 0 if there is none

Block header, 0x20 bytes:
  f32 centre[3], f32 radius   bounding sphere, used to cull the block
  u32 face_count
  u32 flags                   see below; 0 on 280 of the 414 blocks
  u32 0, u32 0                zero everywhere, and nothing in the DOL reads them

Block flags, from the code that reads them (0x8005d08c, 0x8005d338):
  0x80   the block is skipped by the collision sweeps entirely
  0x40   a hit on it is treated as non-blocking by several callers; the
         sense is inverted in the camera path, so this is read off usage
         rather than named -- treat it as "not solid for some queries"
  0-2    index into the eight floats at 0x802176c8 (1000, 100, 150, 200,
         250, 300, 350, 400), whose use was not traced

A face is a convex polygon with its plane precomputed, stored as
  f32 vert[n][3], f32 normal[3], f32 d, u32 kind
so that normal . vert + d == 0, with the normal pointing into the arena.
n is 4 (68-byte records) in every arena but arena 7, which is 3 (56-byte
records); nothing in the block header distinguishes them, so the stride is
taken from the gap between a block's data and the next block's header.
Blocks are padded to 16 bytes.

`kind` reaches the hit record and callers test bit 0x40 on it; the 0x100 -
0x800 values look like a surface class, but no reader for them was found.

Block 0 is the arena shell: four inward walls and a floor, no ceiling. The
rest are obstacles, each tied to root joint i of the arena scene, so a block
can move or be switched off with its model.

The game tests a robot as a swept sphere: the segment from the old to the
new position against each block's sphere, then against that block's faces,
stopping at the first hit (plane side, then point-in-polygon, then the
edges). Blocks are culling groups, not convex brushes -- the faces are a
one-sided soup, which is why each carries its own plane.

The trailing section is NOT arena data: it is a runtime scratch array of
0x284-byte records that the game rewrites every frame (0x8006b848), left in
the file with its placeholder values. Its size matches -- arena 0's 13536
bytes are 21 records plus 12 bytes of padding -- so read_collision returns
it undecoded and nothing should be read out of it.

Decoded with the game's own code: the loader is FUN_80020a70(file 12,
entry 2*arena+n), the parser 0x80072a58, the per-block init 0x80072064,
and the sweeps 0x8005d08c / 0x8005d338 with the face test at 0x8005e270.
"""
import argparse, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import hsd
from models import FILES

ARENAS = 40

class Face:
    def __init__(self, verts, normal, d, kind):
        self.verts, self.normal, self.d, self.kind = verts, normal, d, kind
    def edges(self):
        v = self.verts
        return [(v[i], v[(i + 1) % len(v)]) for i in range(len(v))]

class Block:
    def __init__(self, centre, radius, flags, faces):
        self.centre, self.radius, self.flags, self.faces = centre, radius, flags, faces

def read_collision(blob):
    """Returns (blocks, tail), tail being the undecoded trailing bytes."""
    count, off_hdrs, off_data, off_tail = struct.unpack_from('>4I', blob, 0)
    hdrs = struct.unpack_from(f'>{count}I', blob, off_hdrs)
    data = struct.unpack_from(f'>{count}I', blob, off_data)
    blocks = []
    for i in range(count):
        cx, cy, cz, radius = struct.unpack_from('>4f', blob, hdrs[i])
        face_count, flags = struct.unpack_from('>2I', blob, hdrs[i] + 16)
        end = hdrs[i + 1] if i + 1 < count else (off_tail or len(blob))
        # 3 verts -> 56-byte records, 4 -> 68; padding to 16 keeps the gap small.
        nverts = 3 if face_count and (end - data[i]) / face_count < 62 else 4
        stride = 12 * nverts + 20
        faces = []
        for k in range(face_count):
            at = data[i] + stride * k
            v = [struct.unpack_from('>3f', blob, at + 12 * j) for j in range(nverts)]
            n = struct.unpack_from('>3f', blob, at + 12 * nverts)
            d = struct.unpack_from('>f', blob, at + 12 * nverts + 12)[0]
            kind = struct.unpack_from('>I', blob, at + 12 * nverts + 16)[0]
            faces.append(Face(v, n, d, kind))
        blocks.append(Block((cx, cy, cz), radius, flags, faces))
    return blocks, (blob[off_tail:] if off_tail else b'')

def story_map(index):
    """The same format again: snr_map.BIN entry N opens with the offset of a
    collision section (0 if the map has none) and the offset of the map's own
    HSD archive, which is where the collision section ends. Unlike an arena,
    a map puts everything in one block."""
    from models import map_sections
    s = map_sections(index)
    if not s['collision']:
        return None
    lo, hi = s['collision']
    return read_collision(s['blob'][lo:hi])

def arena(index):
    blob = (FILES / 'bg_data.BIN').read_bytes()
    off, size = hsd.sfd_entries(blob)[index * 2 + 1]
    return read_collision(blob[off:off + size])

def plane_error(face):
    return max(abs(sum(face.normal[c] * v[c] for c in range(3)) + face.d) for v in face.verts)

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('index', nargs='?', type=int, help='arena 0-39; omit for all')
    ap.add_argument('--faces', action='store_true', help='list every face')
    ap.add_argument('--maps', action='store_true', help='story maps, not arenas')
    args = ap.parse_args()
    if args.maps:
        import hsd as _hsd
        from models import FILES as _F
        n = len(_hsd.sfd_entries((_F / 'scenario_us/snr_map.BIN').read_bytes()))
        for i in ([args.index] if args.index is not None else range(n)):
            got = story_map(i)
            if got is None:
                print(f'map {i:2d}  no collision section')
                continue
            blocks, _ = got
            nf = sum(len(b.faces) for b in blocks)
            err = max((plane_error(f) for b in blocks for f in b.faces), default=0)
            print(f'map {i:2d}  {len(blocks):2d} blocks  {nf:5d} faces  plane err {err:.4f}')
        return
    for i in ([args.index] if args.index is not None else range(ARENAS)):
        blocks, tail = arena(i)
        nf = sum(len(b.faces) for b in blocks)
        err = max((plane_error(f) for b in blocks for f in b.faces), default=0)
        kinds = sorted({f.kind for b in blocks for f in b.faces})
        print(f'arena {i:2d}  {len(blocks):3d} blocks  {nf:5d} faces  '
              f'{len(blocks[0].faces[0].verts)}-gon  plane err {err:.4f}  '
              f'tail {len(tail):5d}B  kinds {[hex(k) for k in kinds]}')
        if args.faces:
            for bi, b in enumerate(blocks):
                print(f'  block {bi:3d} centre {tuple(round(x, 2) for x in b.centre)} '
                      f'r {b.radius:.2f} flags {b.flags:#x} faces {len(b.faces)}')
                for f in b.faces:
                    print('    ', [tuple(round(x, 2) for x in v) for v in f.verts],
                          'n', tuple(round(x, 3) for x in f.normal), 'd', round(f.d, 3),
                          'kind', hex(f.kind))

if __name__ == '__main__':
    main()

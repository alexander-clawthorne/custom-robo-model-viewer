#!/usr/bin/env python3
"""Where a gun's bullets and a bomb launcher's bombs leave the part.

    PYTHONUTF8=1 python tools/muzzle.py [gun|bom] [index ...]

The part loader 0x80073cb8 reads header word 8 of a part file (as
hsd.read_robo sees it): per root one count byte, then that many paths of
02 (child) / 01 (next sibling) bytes ended by 0xff, walked from the root's
joint. The joints found go into one array per robot, across all roots:
S+0x8b8 + 4i, i = 0-3 the gun's, 4-5 the bomb's, 6-13 the legs'. Then
0x80007ce4 picks
  S+0x8f0 = gun entry 0   the bullet origin; bullets fly along its +X
  S+0x8f8 = bomb entry 1  the throw origin and direction (bomb root 1)
and the fire code (0x80042bbc) reads the joint's world matrix right after
FUN_80095a30 refreshes it. Pods have no list (word 8 is -1 in all 57).

A part root's own SRT never reaches the screen (0x80039fac writes its world
matrix as the rig slot's, basis x 0.1), so the muzzle is given relative to
the root: world = slot matrix x scale(0.1) x rel. Every gun and bomb has
the joint. Gun root 0 also carries a 10-frame deploy animation, ramped in
at battle start (S+0x20d4 += 0.025 a frame, 0x80101538); its last frame is
the rest pose for 53 of the 64 guns, and is what this evaluates.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import effects
import hsd
import models

ARCHIVES = {'gun': 'weapon_gun.BIN', 'bom': 'weapon_bom.BIN'}
ENTRY = {'gun': 0, 'bom': 1}
DEPLOYED = {'gun': 10.0, 'bom': None}   # gun deploy animation frame; bombs have none


def spawn_paths(d, h):
    """The effect-spawn list: [(root index, path bytes)] in file order."""
    out, at = [], h[8]
    for r in range(h[3]):
        n = d.u8(at)
        at += 1
        for _ in range(n):
            start = at
            while d.u8(at) != 0xff:
                at += 1
            out.append((r, bytes(d.data[start:at])))
            at += 1
    return out


def _anim_nodes(d, a):
    """Root 0's AnimJoint tree: {path bytes: AObj}, paths as in the spawn list."""
    res = {}

    def visit(a, path):
        k = 0
        while a is not None:
            p = path + bytes([1]) * k
            ao = effects.aobj(d, d.ptr(a + 8))
            if ao:
                res[p] = ao
            c = d.ptr(a)
            if c is not None:
                visit(c, p + bytes([2]))
            a = d.ptr(a + 4)
            k += 1
    visit(a, b'')
    return res


def _relative(d, h, root_index, path, frame):
    """The joint's matrix relative to its root, the root's own SRT left out;
    with root 0's animation at `frame` (None = the stored SRTs). Tracks 1-3
    rotation, 5-7 translation, 8-10 scale."""
    nodes = _anim_nodes(d, d.ptr(h[2] + 4)) if frame is not None and root_index == 0 else {}
    off, m, parent = d.roots[f'jobj{root_index}'], hsd.IDENTITY, hsd.IDENTITY
    for n, step in enumerate(path):
        if step == 2:
            parent = m
            off = d.ptr(off + 8)
        else:
            off = d.ptr(off + 0x0c)
        s, r, t = list(d.vec3(off + 0x20)), list(d.vec3(off + 0x14)), list(d.vec3(off + 0x2c))
        ao = nodes.get(path[:n + 1])
        for fo in (ao[2] if ao else []):
            v = effects.evaluate(effects.fobj_keys(fo), frame)
            if fo.track in (1, 2, 3):
                r[fo.track - 1] = v
            elif fo.track in (5, 6, 7):
                t[fo.track - 5] = v
            elif fo.track in (8, 9, 10):
                s[fo.track - 8] = v
        m = hsd.mat_mul(parent, hsd.srt(s, r, t))
    return m


def muzzle(kind, index):
    """(point, unit +X axis) in the part root's frame, part units; None if
    the part has no such joint."""
    blob = models.entry(ARCHIVES[kind], index)
    h = struct.unpack_from('>16I', blob, 0)
    d = hsd.read_robo(blob)
    paths = spawn_paths(d, h)
    if ENTRY[kind] >= len(paths):
        return None
    r, p = paths[ENTRY[kind]]
    m = _relative(d, h, r, p, DEPLOYED[kind])
    return tuple(m[i][3] for i in range(3)), hsd._norm(tuple(m[i][0] for i in range(3)))


def main():
    args = sys.argv[1:]
    for kind in [args[0]] if args else ['gun', 'bom']:
        n = len(hsd.sfd_entries((models.FILES / ARCHIVES[kind]).read_bytes()))
        for i in [int(a) for a in args[1:]] or range(n):
            got = muzzle(kind, i)
            if got is None:
                print(f'{kind} {i:2d}: no muzzle joint')
                continue
            (tx, ty, tz), (xx, xy, xz) = got
            print(f'{kind} {i:2d}: at ({tx:7.3f} {ty:7.3f} {tz:7.3f})  +X ({xx:6.3f} {xy:6.3f} {xz:6.3f})')


if __name__ == '__main__':
    main()

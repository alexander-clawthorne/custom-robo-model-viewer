#!/usr/bin/env python3
"""Weapon and hit effects: the animation inside *_effect.BIN and efc_common.dbn.

    python tools/effects.py                 # summarise every effect
    python tools/effects.py gunfx 0         # one effect's animation tracks

models.py reads the containers and the models; this reads what moves. Every
ModelSet in an effect scene is

  JObj*, AnimJoint**, MatAnimJoint**, ShapeAnimJoint**

where the last three are NULL-terminated arrays, one entry per model, not
single pointers. Each array entry is the root of a tree that mirrors the
JObj tree, so walking child/next in step with the joints pairs an animation
with the joint it drives.

  AnimJoint     child*, next*, AObj*, RObj*, flags            (0x14)
  MatAnimJoint  child*, next*, MatAnim*                       (0x0c)
  MatAnim       next*, AObj*, TexAnim*, ...                   (material colour)
  TexAnim       next*, GXTexMapID, AObj*, imagetbl**, tluttbl**,
                u16 n_imagetbl, u16 n_tluttbl
  AObj          flags, f32 endFrame, FObj*, objID*            (0x10)
  FObj          next*, u32 length, f32 startFrame, u8 track,
                u8 valueFormat, u8 slopeFormat, u8 pad, u8* data   (0x14)

Every effect animates in three ways at once: joint transforms, the
material's colours and alpha, and the texture's UV transform -- plus a
flipbook on the 57 TexAnims that carry an image table, which is exactly
the set whose AObj has an `image` track.

Track 40 on a joint is the particle trigger. It is the only track that
uses opcode KEY, it always uses it, and every one of its key values is
0.0, so the value carries nothing and the frame is the whole point: these
are timed spawns, not a curve.

`length` is the byte length of the packed keyframe stream at `data`;
fobj_keys() unpacks it. Numbers inside that stream are LITTLE-endian even
though everything around them is big-endian, and waits are LSB-first
varints. The format follows the Melee decomp's fobj.c; the first track
tried decoded to 0, pi/2, 3*pi radians over the AObj's 600 frames, which
is what a spinning bomb should look like.
"""
import argparse, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import hsd
from models import EFFECT_ARCHIVES, FILES, effect_subs

ANIM_JOINT, MAT_ANIM_JOINT, SHAPE_ANIM_JOINT = 1, 2, 3

def archive_count(archive):
    name = EFFECT_ARCHIVES[archive]
    blob = (FILES / name).read_bytes()
    if name.endswith('.dbn'):
        return struct.unpack_from('>I', blob, 0)[0]
    return 2 * len(hsd.sfd_entries(blob))

def scenes(archive, index):
    """The HSD archive for effect `index` of this category."""
    name = EFFECT_ARCHIVES[archive]
    if name.endswith('.dbn'):
        return hsd.read_hsd(effect_subs(name, 0)[index])
    return hsd.read_hsd(effect_subs(name, index // 2)[index % 2])

def model_sets(d):
    """Every ModelSet offset under scene_data."""
    at, out = d.ptr(d.roots['scene_data']), []
    while at is not None and at + 4 <= len(d.data) and d.ptr(at) is not None:
        out.append(d.ptr(at))
        at += 4
    return out

def anim_roots(d, model_set, slot):
    """The NULL-terminated array at ModelSet slot 1/2/3, as a list of roots."""
    arr, out, k = d.ptr(model_set + slot * 4), [], 0
    if arr is None:
        return out
    while d.ptr(arr + k * 4) is not None:
        out.append(d.ptr(arr + k * 4))
        k += 1
    return out

def walk_tree(d, root, payload=8):
    """(depth, node, payload pointer) over a child/next tree, parents first."""
    out = []
    def visit(off, depth):
        while off is not None:
            out.append((depth, off, d.ptr(off + payload)))
            child = d.ptr(off)
            if child is not None:
                visit(child, depth + 1)
            off = d.ptr(off + 4)
    visit(root, 0)
    return out

# Storage formats for a value or slope byte: high three bits pick the type,
# low five are the shift the stored integer is divided by.
VALUE_READERS = {
    0x00: (4, lambda b: struct.unpack('<f', b)[0]),
    0x20: (2, lambda b: struct.unpack('<h', b)[0]),
    0x40: (2, lambda b: struct.unpack('<H', b)[0]),
    0x60: (1, lambda b: struct.unpack('<b', b)[0]),
    0x80: (1, lambda b: b[0]),
}
# Opcodes, from HSD_A_OP_*. SLP carries only a tangent and takes no time.
OP_NONE, OP_CON, OP_LIN, OP_SPL0, OP_SPL, OP_SLP, OP_KEY = range(7)

# Track ids. Which table applies depends on what owns the AObj, so the same
# number means different things on a joint and on a texture.
JOBJ_TRACKS = {1: 'rot x', 2: 'rot y', 3: 'rot z', 4: 'path', 5: 'tra x', 6: 'tra y',
               7: 'tra z', 8: 'scale x', 9: 'scale y', 10: 'scale z', 11: 'node',
               12: 'branch', 40: 'particle'}
MAT_TRACKS = {1: 'ambient r', 2: 'ambient g', 3: 'ambient b', 4: 'diffuse r',
              5: 'diffuse g', 6: 'diffuse b', 7: 'specular r', 8: 'specular g',
              9: 'specular b', 10: 'alpha', 11: 'pe ref0', 12: 'pe ref1',
              13: 'pe dst alpha'}
TEX_TRACKS = {1: 'image', 2: 'tra u', 3: 'tra v', 4: 'scale u', 5: 'scale v',
              6: 'rot x', 7: 'rot y', 8: 'rot z', 9: 'blend', 10: 'tclt',
              11: 'lod bias'}
TRACK_NAMES = {'jobj': JOBJ_TRACKS, 'mat': MAT_TRACKS, 'tex': TEX_TRACKS}

class Key:
    def __init__(self, op, frame, value, tangent):
        self.op, self.frame, self.value, self.tangent = op, frame, value, tangent
    def __repr__(self):
        return f'Key({self.op}, f={self.frame:g}, v={self.value:.4f}, t={self.tangent:.4f})'

def fobj_keys(fobj):
    """The packed stream -> keys at absolute frames. Raises on a short read,
    which is the check that the format is being read correctly."""
    data, at = fobj.data, 0
    def value(fmt):
        nonlocal at
        size, read = VALUE_READERS[fmt & 0xE0]
        if at + size > len(data):
            raise ValueError('ran off the end of the keyframe stream')
        v = read(data[at:at + size])
        at += size
        return v / (1 << (fmt & 0x1F)) if fmt & 0xE0 else v
    def varint():
        nonlocal at
        out, shift = 0, 0
        while True:
            if at >= len(data):
                raise ValueError('ran off the end of a varint')
            b = data[at]; at += 1
            out |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return out
    keys, clock, tangent = [], 0, 0.0
    while at < len(data):
        b = data[at]; at += 1
        op, count, shift = b & 0x0F, ((b >> 4) & 7) + 1, 3
        while b & 0x80:
            b = data[at]; at += 1
            count += (b & 0x7F) << shift
            shift += 7
        if op == OP_NONE:
            break
        for _ in range(count):
            v, wait = None, 0
            if op == OP_SLP:
                tangent = value(fobj.slope_format)
            elif op == OP_SPL:
                v = value(fobj.value_format); tangent = value(fobj.slope_format); wait = varint()
            elif op in (OP_CON, OP_LIN, OP_SPL0):
                v = value(fobj.value_format); wait = varint()
                if op == OP_SPL0:
                    tangent = 0.0
            elif op == OP_KEY:
                # The Melee decomp reads a wait here and HSDLib does not. The
                # decomp is right for this data: without the wait, 76 of the
                # 753 tracks that use this opcode run off the end of their
                # stream and none of the rest land on the AObj's end frame.
                v = value(fobj.value_format); wait = varint()
            else:
                raise ValueError(f'unknown FObj opcode {op}')
            if v is not None:
                keys.append(Key(op, clock - fobj.start, v, tangent))
                clock += wait
    return keys

def hermite(p0, p1, d0, d1, u, span):
    s = u / span
    return (p0 * (2 * s ** 3 - 3 * s ** 2 + 1) + p1 * (-2 * s ** 3 + 3 * s ** 2)
            + d0 * (u - 2 * u * u / span + u ** 3 / (span * span))
            + d1 * (u ** 3 / (span * span) - u * u / span))

def evaluate(keys, frame):
    """The track's value at `frame`. The opcode of the key that starts a
    segment decides how that segment is interpolated."""
    if not keys:
        return 0.0
    if frame <= keys[0].frame:
        return keys[0].value
    for i in range(len(keys) - 1):
        a, b = keys[i], keys[i + 1]
        if not a.frame <= frame < b.frame:
            continue
        span = b.frame - a.frame
        if span <= 0 or a.op == OP_CON:
            return a.value
        u = frame - a.frame
        if a.op == OP_LIN:
            return a.value + (b.value - a.value) / span * u
        return hermite(a.value, b.value, a.tangent, b.tangent, u, span)
    return keys[-1].value

class FObj:
    def __init__(self, d, off):
        self.off = off
        self.length = d.u32(off + 4)
        self.start = d.f32(off + 8)
        self.track = d.data[off + 12]
        self.value_format = d.data[off + 13]
        self.slope_format = d.data[off + 14]
        at = d.ptr(off + 16)
        self.data = d.data[at:at + self.length] if at is not None else b''

def aobj(d, off):
    """(flags, endFrame, [FObj]) for an AObj, or None."""
    if off is None:
        return None
    tracks, f = [], d.ptr(off + 8)
    while f is not None:
        tracks.append(FObj(d, f))
        f = d.ptr(f)
    return d.u32(off), d.f32(off + 4), tracks

def tex_anims(d, mat_anim):
    """Every TexAnim hanging off a MatAnim chain."""
    out = []
    while mat_anim is not None:
        t = d.ptr(mat_anim + 8)
        while t is not None:
            out.append({'off': t, 'tex_map_id': d.u32(t + 4), 'aobj': d.ptr(t + 8),
                        'images': d.u16(t + 20), 'tluts': d.u16(t + 22)})
            t = d.ptr(t)
        mat_anim = d.ptr(mat_anim)
    return out

def tracks(archive, index):
    """Every animation track in one effect, as (owner, name, track id, keys)."""
    d, out = scenes(archive, index), []
    def add(owner, p, label):
        a = aobj(d, p)
        if not a:
            return
        for f in a[2]:
            out.append((owner, label, TRACK_NAMES[owner].get(f.track, f'track {f.track}'),
                        a[1], fobj_keys(f)))
    for mi, ms in enumerate(model_sets(d)):
        for root in anim_roots(d, ms, ANIM_JOINT):
            for depth, node, p in walk_tree(d, root):
                add('jobj', p, f'model {mi} joint@{node:#x}')
        for root in anim_roots(d, ms, MAT_ANIM_JOINT):
            for depth, node, p in walk_tree(d, root):
                if p is None:
                    continue
                add('mat', d.ptr(p + 4), f'model {mi} material@{node:#x}')
                for t in tex_anims(d, p):
                    add('tex', t['aobj'],
                        f"model {mi} texture{t['tex_map_id']}"
                        + (f" ({t['images']} images)" if t['images'] else ''))
    return out

def summarise(archive, index):
    d = scenes(archive, index)
    joints = mats = texs = fobjs = 0
    end = 0.0
    for ms in model_sets(d):
        for root in anim_roots(d, ms, ANIM_JOINT):
            for _, _, p in walk_tree(d, root):
                a = aobj(d, p)
                if a:
                    joints += 1; fobjs += len(a[2]); end = max(end, a[1])
        for root in anim_roots(d, ms, MAT_ANIM_JOINT):
            for _, _, p in walk_tree(d, root):
                if p is None:
                    continue
                mats += 1
                a = aobj(d, d.ptr(p + 4))
                if a:
                    fobjs += len(a[2]); end = max(end, a[1])
                for t in tex_anims(d, p):
                    texs += 1
                    a = aobj(d, t['aobj'])
                    if a:
                        fobjs += len(a[2]); end = max(end, a[1])
    return {'joint_anims': joints, 'mat_anims': mats, 'tex_anims': texs,
            'tracks': fobjs, 'end_frame': end}

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('archive', nargs='?', choices=sorted(EFFECT_ARCHIVES))
    ap.add_argument('index', nargs='?', type=int)
    ap.add_argument('--tracks', action='store_true', help='list every keyframe track')
    args = ap.parse_args()
    archives = [args.archive] if args.archive else sorted(EFFECT_ARCHIVES)
    for a in archives:
        rng = [args.index] if args.index is not None else range(archive_count(a))
        for i in rng:
            print(f'{a}:{i:3d}', summarise(a, i))
            if not args.tracks:
                continue
            for owner, label, name, end, keys in tracks(a, i):
                frames = ' '.join(f'{k.frame:g}:{k.value:.3f}' for k in keys[:6])
                print(f'    {label:34s} {name:11s} end {end:6g}  {len(keys):3d} keys  '
                      f'{frames}{" ..." if len(keys) > 6 else ""}')

if __name__ == '__main__':
    main()

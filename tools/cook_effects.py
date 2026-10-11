#!/usr/bin/env python3
"""Cook HSD effect animations into EFX1 files (romfs/<cat>/anim.efx).

    python tools/cook_effects.py            # all categories
    python tools/cook_effects.py gunfx      # one category

EFX1 layout (little-endian):
  'EFX1', u32 count, u32 offsets[count]
  Per animation:
    f32 duration, u32 joint_track_count, u32 mat_track_count
    joint_track_count * { s16 joint, u8 channel, u8 pad, u16 key_count, u16 pad,
                          key_count * { f32 frame, f32 value, f32 tanIn, f32 tanOut } }
    mat_track_count  * { s16 mat,   u8 channel, u8 pad, u16 key_count, u16 pad,
                          key_count * { f32 frame, f32 value, f32 tanIn, f32 tanOut } }

Joint channel numbers match JOBJ_TRACKS in effects.py (1=rotX..10=scaleZ, 40=particle).
Mat channel 10 = alpha. Other mat channels skipped (not needed for 3DS port).
Rotations are stored in radians (raw from HSD).
"""
import argparse, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from models import EFFECT_ARCHIVES
from effects import (archive_count, scenes, model_sets, anim_roots,
                     walk_tree, aobj, fobj_keys,
                     ANIM_JOINT, MAT_ANIM_JOINT, OP_KEY)

ROOT = Path(__file__).resolve().parents[1]
OUT  = ROOT / 'romfs'

# Joint channels we actually animate on 3DS (rot, tra, scale, particle trigger)
JOBJ_KEEP = {1, 2, 3, 5, 6, 7, 8, 9, 10, 40}
# Mat channel: only alpha
MAT_KEEP = {10}

KEY_FMT = struct.Struct('<4f')

def pack_keys(keys):
    """Pack a key list into binary. For SPL/SPL0 keys, tangent is stored.
    For non-Hermite, tanIn=tanOut=0."""
    out = bytearray()
    for i, k in enumerate(keys):
        tan_in = k.tangent
        # tanOut: use next key's tangent if available, else same as tanIn
        tan_out = keys[i + 1].tangent if i + 1 < len(keys) else k.tangent
        out += KEY_FMT.pack(k.frame, k.value, tan_in, tan_out)
    return bytes(out)

def cook_anim(archive, index):
    """Returns (duration, joint_tracks, mat_tracks) where tracks are byte blobs."""
    d = scenes(archive, index)
    duration = 0.0
    joint_blobs = []  # list of (joint_index, channel, blob)
    mat_blobs   = []  # list of (mat_index, channel, blob)

    for ms in model_sets(d):
        for root in anim_roots(d, ms, ANIM_JOINT):
            for _depth, _node, p in walk_tree(d, root, payload=8):
                a = aobj(d, p)
                if not a:
                    continue
                flags, end_frame, fobjs = a
                duration = max(duration, end_frame)
                # Recover which joint this AnimJoint maps to.
                # walk_tree visits nodes in the same DFS order as walk_joints
                # in hsd.py, so the DFS index == position in the joints list.
                # We use _node (the AnimJoint HSD offset) as the key -- but we
                # need the JOBJ index, not the AnimJoint offset. The AnimJoint
                # tree mirrors the JObj tree in order, so we track position.
                # Simpler: we rely on the cook step numbering joints 0..N-1
                # in DFS order, and the AnimJoint tree also visits them in DFS
                # order. We use a counter maintained below.
                for f in fobjs:
                    if f.track not in JOBJ_KEEP:
                        continue
                    try:
                        keys = fobj_keys(f)
                    except Exception:
                        continue
                    if not keys:
                        continue
                    joint_blobs.append((f.track, keys))

        # Material alpha tracks
        for root in anim_roots(d, ms, MAT_ANIM_JOINT):
            mat_idx = 0
            for _depth, _node, p in walk_tree(d, root, payload=8):
                if p is None:
                    mat_idx += 1
                    continue
                mat_aobj = d.ptr(p + 4)  # MatAnim.AObj
                a = aobj(d, mat_aobj)
                if a:
                    _, end_frame, fobjs = a
                    duration = max(duration, end_frame)
                    for f in fobjs:
                        if f.track not in MAT_KEEP:
                            continue
                        try:
                            keys = fobj_keys(f)
                        except Exception:
                            continue
                        if keys:
                            mat_blobs.append((mat_idx, f.track, keys))
                mat_idx += 1

    return duration, joint_blobs, mat_blobs


def cook_anim_v2(archive, index):
    """Walk AnimJoint tree in parallel with the JObj DFS to get joint indices."""
    from models import load_model
    d_hsd, joints, _meshes = load_model(archive, index)
    d = scenes(archive, index)

    # Build offset -> joint_index map from the HSD JObj tree
    by_off = {}
    try:
        jobj_root = list(d_hsd.roots.values())[0]
    except (IndexError, AttributeError):
        jobj_root = None

    if jobj_root is not None:
        # walk_joints DFS: same order as joints list
        def walk_jobj(off, idx_counter=[0]):
            if off is None:
                return
            by_off[off] = idx_counter[0]
            idx_counter[0] += 1
            child = d_hsd.ptr(off + 0x08)
            walk_jobj(child, idx_counter)
            nxt = d_hsd.ptr(off + 0x0C)
            walk_jobj(nxt, idx_counter)
        try:
            walk_jobj(jobj_root)
        except Exception:
            pass

    duration = 0.0
    joint_blobs = []
    mat_blobs = []

    for ms in model_sets(d):
        for root in anim_roots(d, ms, ANIM_JOINT):
            # Walk AnimJoint tree; each node has child@0, next@4, AObj@8
            def visit_anim(aj_off, jobj_off):
                if aj_off is None or jobj_off is None:
                    return
                joint_idx = by_off.get(jobj_off, -1)
                p = d.ptr(aj_off + 8)  # AObj pointer
                a = aobj(d, p)
                if a and joint_idx >= 0:
                    _, end_frame, fobjs = a
                    nonlocal duration
                    duration = max(duration, end_frame)
                    for f in fobjs:
                        if f.track not in JOBJ_KEEP:
                            continue
                        try:
                            keys = fobj_keys(f)
                        except Exception:
                            continue
                        if keys:
                            joint_blobs.append((joint_idx, f.track, keys))
                # Recurse child
                aj_child = d.ptr(aj_off)
                jobj_child = d_hsd.ptr(jobj_off + 0x08)
                visit_anim(aj_child, jobj_child)
                # Recurse next
                aj_next = d.ptr(aj_off + 4)
                jobj_next = d_hsd.ptr(jobj_off + 0x0C)
                visit_anim(aj_next, jobj_next)

            # Find the first JObj root that corresponds to this model set
            jobj_ms_root = list(d_hsd.roots.values())[0] if d_hsd.roots else None
            visit_anim(root, jobj_ms_root)

        # Material alpha
        for root in anim_roots(d, ms, MAT_ANIM_JOINT):
            mat_idx = 0
            for _depth, _node, p in walk_tree(d, root, payload=8):
                if p is None:
                    mat_idx += 1
                    continue
                mat_aobj_off = d.ptr(p + 4)
                a = aobj(d, mat_aobj_off)
                if a:
                    _, end_frame, fobjs = a
                    duration = max(duration, end_frame)
                    for f in fobjs:
                        if f.track not in MAT_KEEP:
                            continue
                        try:
                            keys = fobj_keys(f)
                        except Exception:
                            continue
                        if keys:
                            mat_blobs.append((mat_idx, f.track, keys))
                mat_idx += 1

    return duration, joint_blobs, mat_blobs


def pack_anim(duration, joint_blobs, mat_blobs):
    """Serialise one animation to bytes."""
    out = bytearray(struct.pack('<f2I', duration, len(joint_blobs), len(mat_blobs)))
    for joint_idx, channel, keys in joint_blobs:
        blob = pack_keys(keys)
        out += struct.pack('<hBBHH', joint_idx, channel, 0, len(keys), 0)
        out += blob
    for mat_idx, channel, keys in mat_blobs:
        blob = pack_keys(keys)
        out += struct.pack('<hBBHH', mat_idx, channel, 0, len(keys), 0)
        out += blob
    return bytes(out)


def cook_category(archive):
    count = archive_count(archive)
    anims = []
    skipped = 0
    for i in range(count):
        try:
            duration, jblobs, mblobs = cook_anim_v2(archive, i)
            anims.append(pack_anim(duration, jblobs, mblobs))
        except Exception as e:
            anims.append(pack_anim(0.0, [], []))
            skipped += 1

    # Build EFX1 file
    header_size = 8 + 4 * count   # 'EFX1' + u32 count + u32 offsets[count]
    offsets = []
    pos = header_size
    for a in anims:
        offsets.append(pos)
        pos += len(a)

    out = bytearray(b'EFX1' + struct.pack('<I', count))
    for off in offsets:
        out += struct.pack('<I', off)
    for a in anims:
        out += a
    return bytes(out), count, skipped


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('archive', nargs='*', choices=sorted(EFFECT_ARCHIVES),
                    help='categories to cook (default: all)')
    args = ap.parse_args()
    cats = args.archive or sorted(EFFECT_ARCHIVES)
    for cat in cats:
        folder = OUT / cat
        folder.mkdir(parents=True, exist_ok=True)
        data, count, skipped = cook_category(cat)
        (folder / 'anim.efx').write_bytes(data)
        print(f'{cat}: {count} anims, {skipped} skipped, {len(data)} bytes -> romfs/{cat}/anim.efx')


if __name__ == '__main__':
    main()

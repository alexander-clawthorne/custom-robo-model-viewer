#!/usr/bin/env python3
"""Assemble a robot: body plus gun, bomb, pod and legs on their mount joints.

A part root joint stores an SRT that looks like a mount rotation and is not.
The draw code (0x80039fac in main.dol) copies the robot's solved matrix out[k]
straight into the part root's world matrix and sets JOBJ_USER_DEF_MTX |
MTX_INDEPEND_PARENT | MTX_INDEPEND_SRT, so that stored SRT never reaches the
screen; the only thing applied is a uniform scale of the basis. Rig slot r is
driven by out[r+1], so each part sits at

    slot_world * scale(models.PART_SCALE)

and the part's geometry is already in the mount joint's frame. The same
placement runs on the 3DS in src/main.c, so the two are a check on each other.

    python tools/assemble.py 0 --out build/preview/robot0.png
    python tools/assemble.py 0 --motion sf_weapon_ta_kamae
"""
import argparse, math, sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).parent))
import hsd, models
from models import ATTACH, PART_SCALE, load_model
from pose import default_skeleton, find_motion, posed_meshes, rig, solve

def slot_matrices(archive, index, motion=None, frame=0.0, mirror=False):
    """World matrix per rig slot: the bind pose, or the solved pose of a motion."""
    d, joints, meshes = load_model(archive, index)
    dims, paths = rig(archive, index)
    by_off = {j.off: j for j in joints}
    root = list(d.roots.values())[0]
    slot_joint = []
    for p in paths:
        off = root
        for c in p:
            off = d.ptr(off + 8) if c == 2 else d.ptr(off + 12)
        slot_joint.append(by_off[off])
    if motion is None:
        return [np.array(j.world) for j in slot_joint]
    out = solve(motion.channels(frame), dims, default_skeleton(), 1.0, mirror)
    return [np.array(out[r + 1]) for r in range(19)]

def assemble(body, picks, motion=None, frame=0.0, mirror=False):
    """(data, meshes) for the whole robot, vertices in world space."""
    if motion is None:
        d, meshes = load_model('chara', body)[0], load_model('chara', body)[2]
    else:
        d, meshes = posed_meshes('chara', body, motion, frame, mirror=mirror)
    slots = slot_matrices('chara', body, motion, frame, mirror)
    S = np.diag([PART_SCALE, PART_SCALE, PART_SCALE, 1.0])
    out = list(meshes)
    for archive, root, slot in ATTACH:
        index = picks[archive]
        M = slots[slot] @ S
        pd, _, pm = load_model(archive, index)
        for m in pm:
            if m['model'] != root:
                continue
            tris = [[{**v, 'pos': tuple((M @ np.array([*v['pos'], 1.0]))[:3])} for v in t]
                    for t in m['tris']]
            # 'data' tells the renderer which archive this mesh's textures live in.
            out.append({**m, 'tris': tris, 'data': pd})
    return d, out

def main():
    from PIL import Image
    from preview import render
    a = argparse.ArgumentParser()
    a.add_argument('body', type=int)
    a.add_argument('--gun', type=int, default=0)
    a.add_argument('--bom', type=int, default=0)
    a.add_argument('--pod', type=int, default=0)
    a.add_argument('--leg', type=int, default=0)
    a.add_argument('--motion')
    a.add_argument('--size', type=int, default=320)
    a.add_argument('--out', default='build/preview/assemble.png')
    args = a.parse_args()
    picks = {'gun': args.gun, 'bom': args.bom, 'pod': args.pod, 'leg': args.leg}
    mo = find_motion(args.motion) if args.motion else None
    frames = [round(mo.length * k / 4) for k in range(4)] if mo else [0.0]
    rows = []
    for f in frames:
        d, meshes = assemble(args.body, picks, mo, f)
        rows.append(np.concatenate([render(d, meshes, y, args.size, 0.15)
                                    for y in (0.0, math.pi / 2, math.pi)], 1))
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.concatenate(rows, 0)).save(out)
    print('->', out)

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Pose a robot from a motion, following the game's own code (main.dol):

  keyframes   0x8001eb48  cubic Hermite per channel (tags pick the tangents)
  pose solve  0x8005aee8  two passes: IK on the built-in default skeleton
                          (table 0x801f9b00), then the robot rebuilt from the
                          same directions and bend angles with its own limb
                          lengths (the 32 floats in its model file)

Full notes with instruction addresses: docs/re/motion_RE_notes.md. The root
nodes 23-25 and mirroring follow the game
(prepare); blending between motions is in src/anim.c only.

    python tools/pose.py chara 0 sf_normal_walk build/preview/walk.png [mirror]
    python tools/pose.py snrchara 0 m_walk build/preview/story_walk.png
"""
import math, struct, sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).parent))
import hsd
from models import ROBO_ARCHIVES, entry, load_model

# ---------------------------------------------------------------- keyframes
def eval_track(track, table_vals, frame):
    """track = (times, codes) with raw u16 codes; table_vals(i) -> float."""
    times, codes = track
    def value(c):
        return 0.0 if c >> 14 == 1 else table_vals(c & 0x3FFF)
    def tan_in(c):   # tangent when the key ends a segment
        tag = c >> 14
        return table_vals((c & 0x3FFF) + 1) if tag in (0, 3) else 0.0
    def tan_out(c):  # tangent when the key starts a segment
        tag = c >> 14
        return table_vals((c & 0x3FFF) + (2 if tag == 0 else 1)) if tag in (0, 3) else 0.0
    i = next((k for k, t in enumerate(times) if frame <= t), None)
    if i is None:
        return value(codes[-1])
    if i == 0 or times[i] == frame:
        return value(codes[i])
    t0, t1 = times[i - 1], times[i]
    dt = (t1 - t0) / 30.0
    s = (frame - t0) / (t1 - t0)
    v0, v1 = value(codes[i - 1]), value(codes[i])
    m0, m1 = tan_out(codes[i - 1]), tan_in(codes[i])
    return (v0 * (1 - s) ** 2 * (1 + 2 * s) + v1 * s * s * (3 - 2 * s)
            + dt * (m0 * s * (1 - s) ** 2 - m1 * s * s * (1 - s)))

class RawMotion:
    """One motion with raw codes, for exact evaluation."""
    def __init__(self, blob, bank_base, index, name=''):
        a, x, b = struct.unpack_from('>3I', blob, bank_base + 4 + 12 * index)
        self.name = name
        self.blob, self.table = blob, bank_base + x
        self.tracks = []
        for c in range(78):
            k = blob[bank_base + b + c]
            if not k:
                self.tracks.append(None)
                continue
            tp, vp = struct.unpack_from('>2I', blob, bank_base + a + 8 * c)
            self.tracks.append((struct.unpack_from(f'>{k}H', blob, bank_base + tp),
                                struct.unpack_from(f'>{k}H', blob, bank_base + vp)))
        self.length = max((t[0][-1] for t in self.tracks if t), default=0)

    def channels(self, frame):
        tv = lambda i: struct.unpack_from('>f', self.blob, self.table + 4 * i)[0]
        return [eval_track(t, tv, frame) if t else 0.0 for t in self.tracks]

def find_motion(name):
    """A robot or story motion by its game name."""
    import motion as mo
    names = mo.motion_names()
    dbn = (mo.FILES / 'motion.DBN').read_bytes()
    sources = [(dbn, mo.dbn_banks(dbn)[b]) for b in mo.ROBOT_BANKS] + mo.story_sources()
    for blob, base in sources:
        for i, ident in enumerate(mo.bank_ids(blob, base)):
            if names.get(ident & 0x7FFF) == name:
                return RawMotion(blob, base, i, name)
    raise KeyError(name)

# ---------------------------------------------------------------- matrices
def T(x, y, z):
    m = np.eye(4); m[:3, 3] = (x, y, z); return m
def Tx(x): return T(x, 0, 0)
def Rx(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[1, 0, 0, 0], [0, c, -s, 0], [0, s, c, 0], [0, 0, 0, 1]])
def Ry(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]])
def Rz(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[c, -s, 0, 0], [s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]])
def M(r):
    """Node rotation in degrees: Rz * Ry * Rx (0x80053da8)."""
    return Rz(r[2]) @ Ry(r[1]) @ Rx(r[0])
def aim(v):
    """Turn the local X axis toward v (0x8005b2ac)."""
    return Rz(math.degrees(math.atan2(v[1], v[0]))) @ Ry(math.degrees(math.atan2(-v[2], math.hypot(v[0], v[1]))))
def local(G, p):
    R, t = G[:3, :3], G[:3, 3]
    return R.T @ (np.asarray(p) - t)
def bend(a, b, d):
    """Law-of-cosines angles A (at the root joint) and B (at the middle joint), degrees."""
    d = max(d, 1e-6)
    ca = max(-1.0, min(1.0, (a * a + d * d - b * b) / (2 * a * d)))
    cb = max(-1.0, min(1.0, (a * a + b * b - d * d) / (2 * a * b)))
    return math.degrees(math.acos(ca)), math.degrees(math.acos(cb))

# Default skeleton (0x801f9b00), read once from the DOL so nothing is hard-coded.
def default_skeleton():
    from dol import Dol
    dol = Dol()
    rec = [struct.unpack('>3f', dol.read(0x801f9b00 + 0x14 * i + 4, 12)) for i in range(26)]
    return {'spine': rec[2][0], 'neck': rec[3][0],
            'arm': [(rec[5][0], rec[6], rec[7][0], rec[8][0]), (rec[10][0], rec[11], rec[12][0], rec[13][0])],
            'hip': rec[15][0],
            'leg': [(rec[16], rec[17][0], rec[18][0]), (rec[21], rec[22][0], rec[23][0])]}

ROT_NODES = [1, 2, 4, 6, 7, 9, 10, 11, 13, 14, 15, 17, 18, 19, 21, 22]
POS_NODES = [0, 3, 5, 8, 12, 16, 20]

# Mirroring (robot flag 0x40): positions are read with X negated and the
# left/right targets swapped (0x80054238); rotations are then flipped by
# 0x80053a20. Per rotation node: its partner and which axes become
# negated (-1) or 180 - angle (2; the game's constant is 32768 = 180 deg).
MIRROR_ROT = {1: (1, (1, -1, -1)), 2: (2, (1, 2, 1)), 4: (4, (1, 2, 1)), 14: (14, (-1, 2, -1)),
              6: (10, (-1, -1, 1)), 7: (11, (-1, -1, 1)), 9: (13, (-1, -1, 1)), 15: (19, (-1, -1, 1)),
              17: (21, (1, -1, -1)), 18: (22, (1, -1, -1))}
for _a, (_b, _f) in list(MIRROR_ROT.items()):
    MIRROR_ROT[_b] = (_a, _f)
MIRROR_POS = {8: 12, 12: 8, 16: 20, 20: 16}

def prepare(ch, mirror=False):
    """Channels -> (positions by node, rotations by node, root rotation R24),
    applying the root nodes 23-25 (0x80056104) and mirroring. Every motion in
    both motion files has the root-node flag set (bank ID bit 15, which the
    loader at 0x80074dcc copies into the gate table 0x8032a8c8)."""
    node = lambda n: list(ch[3 * n:3 * n + 3])
    pos = {n: node(MIRROR_POS.get(n, n) if mirror else n) for n in POS_NODES + [23, 25]}
    if mirror:
        for v in pos.values():
            v[0] = -v[0]
    r24 = node(24)
    R = M(r24)
    t = R[:3, :3] @ pos[25] + pos[23]
    pos = {n: R[:3, :3] @ np.array(pos[n]) + t for n in POS_NODES}
    rot = {n: node(n) for n in ROT_NODES}
    rot[1] = [a + b for a, b in zip(rot[1], r24)]
    if mirror:
        flipped = {}
        for n in ROT_NODES:
            src, f = MIRROR_ROT[n]
            flipped[n] = [180 - v if k == 2 else k * v for v, k in zip(rot[src], f)]
        rot = flipped
    return pos, rot, R

def solve(ch, D, ds, s=1.0, mirror=False):
    """20 world matrices (out[0] root, out[r+1] rig slot r) for one frame."""
    pos, rot, R24 = prepare(ch, mirror)
    return solve_prepared(pos, [M(rot[n]) for n in ROT_NODES], R24, D, ds, s)

def solve_prepared(pos, Mk, R24, D, ds, s=1.0):
    node = lambda n: pos[n]
    root_pos = node(0)
    out = [None] * 20
    out[0] = T(*root_pos) @ Mk[0]
    G = out[0] @ Mk[1]
    G = G @ aim(local(G, node(3)))
    out[1] = spine = G
    # pass 1 on the default skeleton: head direction, arm directions and bends
    H = spine @ Tx(ds['spine']) @ Tx(ds['neck']) @ Mk[2]
    head_dir = local(H, node(5))
    arm_solve = []
    for side, (shoulder, j1, hand, target) in enumerate(((3, 4, 5, 8), (6, 7, 8, 12))):
        base_x, j1off, upper, fore = ds['arm'][side]
        G = spine @ Tx(ds['spine']) @ Tx(base_x) @ Mk[shoulder] @ T(j1off[0], 0, j1off[2]) @ Mk[j1]
        v = local(G, node(target))
        arm_solve.append((v, *bend(upper, fore, float(np.linalg.norm(v)))))
    # pass 2 on the robot's own dimensions
    chest = spine @ Tx(D[0] * s)
    out[2] = chest @ Tx(D[1] * s) @ Mk[2] @ aim(head_dir)
    for side, (shoulder, j1, hand) in enumerate(((3, 4, 5), (6, 7, 8))):
        d = D[2 + 6 * side: 8 + 6 * side]
        v, A, B = arm_solve[side]
        base = chest @ Tx(d[0] * s) @ Mk[shoulder]
        J1 = base @ T(d[1] * s, 0, d[2] * s) @ Mk[j1] @ aim(v) @ Rz(-A)
        J2 = J1 @ Tx(d[3] * s) @ Rz(180 - B)
        eff = J2 @ Tx(d[4] * s) @ T(0, 0, d[5] * s) @ Mk[hand]
        out[3 + 4 * side:7 + 4 * side] = [base, J1, J2, eff]
    hips = T(*root_pos) @ Mk[0] @ Mk[9] @ Rz(-90)
    out[11] = hips
    for side, (hip, foot, toe, target) in enumerate(((10, 11, 12, 16), (13, 14, 15, 20))):
        j1off, thigh, shin = ds['leg'][side]
        G = hips @ Tx(ds['hip']) @ T(j1off[0], 0, j1off[2]) @ Mk[hip]
        v = local(G, node(target))
        A, B = bend(thigh, shin, float(np.linalg.norm(v)))
        d = D[15 + 6 * side: 21 + 6 * side]
        J1 = hips @ Tx(D[14] * s) @ T(d[0] * s, 0, d[1] * s) @ Mk[hip] @ aim(v) @ Rz(A)
        J2 = J1 @ Tx(d[2] * s) @ Rz(B - 180)
        F = J2 @ Tx(d[3] * s)
        F[:3, :3] = (R24 @ Mk[foot])[:3, :3]  # absolute foot orientation (0x80059cc0)
        toe_m = F @ T(0, d[4] * s, d[5] * s) @ Mk[toe]
        out[12 + 4 * side:16 + 4 * side] = [J1, J2, F, toe_m]
    return out

# ---------------------------------------------------------------- the robot
def rig(archive, index):
    """Rig slot joint offsets and the 32 dimension floats from the model file."""
    blob = entry(ROBO_ARCHIVES[archive], index)
    h = struct.unpack_from('>12I', blob, 0)
    dims = struct.unpack_from('>32f', blob, h[3])
    paths = blob[h[8]:].split(b'\xff')[:19]
    return dims, paths

def posed_meshes(archive, index, motion, frame, scale=1.0, mirror=False):
    d, joints, meshes = load_model(archive, index)
    dims, paths = rig(archive, index)
    by_off = {j.off: j for j in joints}
    root = list(d.roots.values())[0]
    slot_joint = []
    for p in paths:
        off = root
        for c in p:
            off = d.ptr(off + 8) if c == 2 else d.ptr(off + 12)
        slot_joint.append(by_off[off].index)
    out = solve(motion.channels(frame), dims, default_skeleton(), scale, mirror)
    fixed = {slot_joint[r]: out[r + 1] for r in range(19)}
    world = [None] * len(joints)
    for j in joints:  # depth-first order: parents first
        if j.index in fixed:
            world[j.index] = fixed[j.index]
            continue
        parent = world[j.parent] if j.parent >= 0 else np.eye(4)
        m = parent @ np.array(hsd.local(d, j.off))
        if j.ik == hsd.EFFECTOR and j.parent >= 0 and joints[j.parent].bone_length:
            x = parent[:3, 0] / (np.linalg.norm(parent[:3, 0]) or 1)
            m[:3, 3] = parent[:3, 3] + x * joints[j.parent].bone_length
        world[j.index] = m
    bind = [np.array(j.world) for j in joints]
    inv_bind = [np.linalg.inv(b) for b in bind]
    posed = []
    for mesh in meshes:
        tris = []
        for tri in mesh['tris']:
            vs = []
            for v in tri:
                kind, b = v['bind']
                lp = np.array([*v['lpos'], 1.0]); ln = np.array([*v['lnrm'], 0.0])
                if kind == 'joint':
                    m = world[b]
                else:
                    m = sum(w * (world[jx] @ inv_bind[jx]) for jx, w in b)
                vs.append({**v, 'pos': tuple((m @ lp)[:3]), 'nrm': tuple((m @ ln)[:3])})
            tris.append(vs)
        posed.append({**mesh, 'tris': tris})
    return d, posed

if __name__ == '__main__':
    from PIL import Image
    from preview import render
    archive, index, name, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], Path(sys.argv[4])
    mirror = 'mirror' in sys.argv[5:]
    mo = find_motion(name)
    frames = [round(mo.length * k / 6) for k in range(6)]
    tiles = []
    for f in frames:
        d, posed = posed_meshes(archive, index, mo, f, mirror=mirror)
        tiles.append(render(d, posed, math.pi / 2, 256, 0.15))
    out.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.concatenate(tiles, 1)).save(out)
    print(name, mo.length, 'frames ->', out, 'frames', frames)

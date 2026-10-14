# Custom Robo motion evaluation - reverse-engineering notes

Source: main.dol, read with tools/dol.py. Every claim cites instruction addresses.
"INFERENCE" marks conclusions not directly proven by the code that was read.

Register bases: r2 = 0x804581a0, r13 = 0x804543c0 (set at 0x8000332c/0x80003334).
Constants used below: r2-0x7dd0 = 0.0, r2-0x7d98 = 1.0, r2-0x7d80 = 2.0, r2-0x7d7c = 3.0,
r2-0x7d78 = 1/30, r2-0x7964 = 65536.0, r2-0x7960 = 360.0, r2-0x79c0 = 1.0, r2-0x795c = -1.0.

Global "G" = current 3x4 matrix at 0x8031fe68 (`lis -0x7fce; addi -0x198`). Helpers:

| addr | does |
|---|---|
| 0x8001daf8 | r3 := identity |
| 0x8001df6c | G := *r3 |
| 0x8001e054 / 0x8001e0d0 | *r3 := G |
| 0x8001e134 | G := G * (*r3) (post-multiply) |
| 0x8001de6c | *r3 := G * (*r4) (point, with translation) |
| 0x8001dd1c(f1) | G.t += G.col0 * f1 (translate along local X) |
| 0x8001db5c(f1,f2) | G.t += G.col0*f2 + G.col2*f1 (local (f2,0,f1)) |
| 0x8001dbf0(f1,f2) | G.t += G.col1*f1 + G.col2*f2 (local (0,f1,f2)) |
| 0x8001dc84(f1) | G.t += G.col2*f1 (local Z) |
| 0x8001d814 / 0x8001d694 | push G / pop G (stack ptr r13-0x4fe0) |
| 0x8001f350(a, *sin, *cos) | s16 binary angle (65536 = 360 deg) -> sin to r4, cos to r5 (checked a=0 -> sin 0, cos table[0x400]) |
| 0x8001f4c0 / 0x8001f428 | sin(a) / cos(a), s16 angle |
| 0x8001f060 | atan2 -> s16 angle |
| 0x8001e424 | length of vec3 |
| 0x8001e684 | swap two floats |
| 0x80169424 | sqrt |

Rotation multiply idiom (e.g. 0x80056660-0x800566e4, 0x80053f44-0x80054160): with (s,c) from 0x8001f350,
Z: col0' = col0*c + col1*s, col1' = col1*c - col0*s  -> G = G*Rz
Y: col0' = col0*c - col2*s, col2' = col0*s + col2*c  -> G = G*Ry
X: col1' = col1*c + col2*s, col2' = col2*c - col1*s  -> G = G*Rx
(standard right-handed Rx/Ry/Rz).

---------------------------------------------------------------------------

## 1. Keyframe evaluator 0x8001eb48

Signature: `float eval(u16 *times /*r3*/, u16 *codes /*r4*/, int nkeys /*r5*/)`, returns f1.
Implicit inputs: float table ptr at r13-0x4fdc (0x8044f3e4) and current frame (float) at r13-0x4fd8.
Both set by 0x80056104 at 0x8005620c/0x8005621c from the motion player struct: player+0 = table
(bank+X), player+0xc = current frame, player+8 = A (channel pointer pairs, -> r13-0x4e44),
player+4 = B (key counts, -> r13-0x4e48).

Key search (0x8001ebc0-0x8001ef58): for i = 0..nkeys-1, first i with `times[i] >= t`
(fcmpo + cror gt|eq at 0x8001ebe8/0x8001ebec).
* If `times[i] == t` or i == 0 (0x8001ec14-0x8001ec20): return value(code[i]) (no interpolation).
  So t <= times[0] clamps to the first key.
* Else Hermite between key i-1 (start) and key i (end) (0x8001ecb0-0x8001ef40).
* If no key satisfies it (t > last time) the code reads codes[nkeys] (0x8001ef5c), one past the
  array (verified: the times array has exactly nkeys entries for 11644 of the tracks). Treat as
  "clamp to last key"; in-game the frame presumably never exceeds the last key time (INFERENCE).
* No looping inside the evaluator; looping/wrapping must happen in the caller's frame counter (not traced).

Tags (code >> 14), idx = code & 0x3FFF, T = float table:

| tag | value | in-tangent (used when key is segment END) | out-tangent (used when key is segment START) | evidence |
|---|---|---|---|---|
| 1 (0x4000) | 0.0 | 0 | 0 | 0x8001ed00, 0x8001eddc, 0x8001ec6c |
| 2 (0x8000) | T[idx] | 0 | 0 | 0x8001ed0c, 0x8001ede8 |
| 3 (0xC000) | T[idx] | T[idx+1] | T[idx+1] | 0x8001ed20-0x8001ed3c, 0x8001edfc-0x8001ee1c |
| 0 | T[idx] | T[idx+1] | T[idx+2] | 0x8001ed44-0x8001ed60, 0x8001ee28-0x8001ee48 |

Interpolation (0x8001ee58-0x8001ef40), t0/v0/m0 = start key, t1/v1/m1 = end key:

    dt = (t1 - t0) / 30            # f31 = (1/30)*(t1-t0)
    s  = (t - t0) / (t1 - t0)      # f28 (1/30 cancels)
    v  = v0*(1-s)^2*(1+2s) + v1*s^2*(3-2s) + dt*( m0*s*(1-s)^2 - m1*s^2*(1-s) )

i.e. cubic Hermite; tangents are in units per 30 frames. Verified against data: tangents of
tag-3 keys equal (finite-difference slope)*30 (e.g. bank0 anim0 ch26: tangent -1.95/-2.1,
slope*30 -2.02/-2.06). Tag 2 = flat tangent (ease), tag 1 = zero key, tag 0 = split tangents.
"Step/constant" interpolation does not exist.

## 2. Callers: which channel, where it goes

Channel c of a node n: c = 3n + axis (x,y,z). Count byte = B[c] (r13-0x4e48 + c);
pointers = A + 8c (r13-0x4e44 + 8c). A zero count gives 0.0 (r2-0x79b0) without calling the evaluator.
Mirror flag (r13-0x4e40 bit 2, from robot+0x168 bit 0x40, 0x8005af74) negates the X of positions and
swaps left/right nodes; ignore for a non-mirrored viewer. "obj" below = motion player (r13-0x4e60 =
robotBase+0x6dc, 0x80050ec8).

### 0x80056104 (root nodes 23,24,25 and pelvis node 0)
Gate: byte table 0x8032a8c8[robotState+0x6a] (0x80056248-0x80056264). If nonzero:
* node 24 (B+0x48 / A+0x240, 0x80056268): each axis * 65536/360 -> obj+0x180/184/188 (s16 angle units). ROTATION in degrees (tools/motion.py NODE_KIND calls 24 "pos": wrong).
* node 23 (B+0x45 / A+0x228, 0x80056350): x negated if mirror -> obj+0x18c..0x194 (translation).
* node 25 (B+0x4b / A+0x258, 0x8005642c) -> obj+0x198..0x1a0.
* Root matrix (0x800564f8-0x80056878): G = Rz(n24.z)*Ry(n24.y)*Rx(n24.x); G.t = n23; then G.t = G*n25
  = R*n25 + n23; stored to obj+0x1a4 (0x80056878).
* node 0 (B+0 / A+0, 0x8005687c): X negated if mirror; transformed by that matrix -> obj+0x1d4..0x1dc (0x8005695c).
Else (0x80056988): obj+0x180..0x1a0 = 0, obj+0x1a4 = identity, node 0 -> obj+0x1d4 untransformed.

### 0x80054238 (target positions + all rotations)
Positions: value (+ mirror X negate) -> G=obj+0x1a4 (0x80054534) -> `p_local` written to robot table
r13-0x4e70 + 0x300.. and a world copy into obj:

| order | node (non-mirrored) | channel base (B/A off) | evaluator calls | local copy | world copy |
|---|---|---|---|---|---|
| root | 0 (from obj+0x1d4) | - | - | +0x300 (0x80054540) | obj+0xe4 |
| chest target | 3, plus obj+0x2c4..0x2cc | 9 (0x9/0x48) 0x8005467c.. | 3 | +0x310 | obj+0xf0 |
| head target | 5 | 15 (0xf/0x78) 0x800549dc.. | 3 | +0x320 | obj+0xfc |
| arm1 hand target | 8 (12 if mirror) | 24 (0x18/0xc0) 0x80054e2c.. ; mirror 36 at 0x80054d30 | 3 | +0x330 | obj+0x108 |
| arm2 hand target | 12 (8 if mirror) | 36 (0x24/0x120) 0x8005528c.. ; mirror 24 at 0x80055194 | 3 | +0x340 | obj+0x114 |
| leg1 foot target | 16 (20 if mirror) | 48 (0x30/0x180) 0x80055730.. ; mirror 60 at 0x80055638 | 3 | +0x350 | obj+0x120 |
| leg2 foot target | 20 (16 if mirror) | 60 (0x3c/0x1e0) 0x80055b94.. ; mirror 48 at 0x80055a98 | 3 | +0x360 | obj+0x144 |

(30 calls; the remaining 3 x 16 = 48 rotation calls in source are the single loop below, 3 call sites.)
World copy (e.g. 0x800545e4-0x80054648): d = p_local (or p - root if flag 1; only z relative if flag 8);
world = base + (d.x*c + d.z*s, d.y, -d.x*s + d.z*c), base = *(r13-0x4e84) (robot world pos),
(s,c) = upper-body yaw (robot+0x54+4) for root/chest/head/hands, lower-body facing yaw (r31) for feet
(recomputed at 0x800555b0). If flag 1 is clear, base.y += root.y*(k*scale - 1) (0x8005445c-0x800544a4),
k = r13-0x4e80 = robot+0xe14 = per-robot-type float from table 0x8021b080 (set at 0x800741b4;
robot 0 = 0.931), scale = robotState+0x154. => world root height = root.y*k*scale; all targets are
shifted by the same amount, so target-minus-joint vectors are unchanged.

Rotations (loop 0x80055ebc-0x80056034, i = 0..25): if (table 0x801f9b00[i].byte0 & 2) the channel
base is byte table 0x801f9d94[i]; each axis * 65536/360 (0x80055f54-0x80055f60) -> obj+0x24 + 12k
(k counts rotation entries). Then obj+0x24..0x2c += obj+0x180..0x188 (node 24 added to node 1, 0x80056038).
Matrices: 0x80053da8 builds, for k = 0..15, M[k] = Rz(z)*Ry(y)*Rx(x) (Z at 0x80053f28, Y at 0x80053fe8,
X at 0x800540b0; angles truncated with fctiwz) and stores to r13-0x4e70 + 0x30k (0x8005418c).
Angles: DEGREES -> 16-bit binary angle (deg*65536/360), not radians. Order: R = Rz*Ry*Rx (X applied first).

| k | node | used as |
|---|---|---|
| 0 | 1 (+node 24) | pelvis/root rotation |
| 1 | 2 | spine (pre-aim) |
| 2 | 4 | neck (pre-aim) |
| 3 | 6 | arm1 shoulder base |
| 4 | 7 | arm1 J1 pre-aim |
| 5 | 9 | arm1 hand |
| 6 | 10 | arm2 shoulder base |
| 7 | 11 | arm2 J1 pre-aim |
| 8 | 13 | arm2 hand |
| 9 | 14 | hips / lower-body |
| 10 | 15 | leg1 hip (pre-aim) |
| 11 | 17 | leg1 foot |
| 12 | 18 | leg1 toe |
| 13 | 19 | leg2 hip (pre-aim) |
| 14 | 21 | leg2 foot |
| 15 | 22 | leg2 toe |

Mirror pairs (table 0x801f9d84 and 0x80053a20, which also swaps/negates y,z of rotations):
(6,10) (7,11) (8,12) (9,13) (15,19) (16,20) (17,21) (18,22).

### Skeleton solve 0x8005aee8 (calls 0x80054238 at 0x8005b034, 0x80053da8 at 0x8005b048)
Two passes. Pass 1 solves on the game's built-in default skeleton (table 0x801f9b00, 26 x 0x14
records, +4/+8/+0xc = x/y/z offset) and keeps only local directions and IK angles. Pass 2 rebuilds the
real robot from the same directions/angles with the robot's dims (ptr r13-0x4e7c, read from robot data at
0x80050ee4) times `scale` (robotState+0x154). Targets themselves are never scaled; per-robot lengths
change the pose by angle-retargeting (hands/feet will not land exactly on the targets).
Output: 20 matrices at r13-0x4e74 (robotBase+0x154c), 0x30 each.

Default skeleton lengths used in pass 1: spine 0.38 (0x801f9b2c), neck 0.02 (0x801f9b40), arm1 base x -0.08 (rec i5),
arm1 J1 offset (x 0.03, z -0.18) (rec i6), upper arm 0.28, forearm 0.26 (rec i7/i8 +4); arm2 the same with z +0.18
(recs i10..i13); hip offset 0.22 (0x801f9c30); leg1 J1 offset (x -0.03, z -0.09) (rec i16), thigh 0.39, shin 0.44
(recs i17/i18); leg2 z +0.09 (recs i21..).

Algorithm (model space: base = 0, yaws = 0):

    Aim(v): G = G * Rz(atan2(v.y, v.x)) * Ry(atan2(-v.z, hypot(v.x, v.y)))   # X axis -> v
            (0x8005b2ac-0x8005b4ec, same code in 0x8005a190.., 0x8005b7ac..)
    local(p) = inverse(G) * p   (0x8005b14c-0x8005b268: R^T (p - G.t))

    root   = T(root_pos) * M[0]                       -> out[0]   (0x8005b05c-0x8005b0e8)
    # pass 1 (default skeleton)
    G = root * M[1];  dirSpine = local(chestTarget); G *= Aim(dirSpine)  -> out[1] (0x8005b4f8)
    push; G *= Tx(0.38); push; G *= Tx(0.02) * M[2]; dirHead = local(headTarget); pop
    arm pass1 0x8005aaa8 (x2, from G = spine*Tx(0.38)):
        G *= Tx(base.x) * M[shoulder] * T(j1.x,0,j1.z) * M[j1pre]
        v = local(handTarget);  d = |v|;  a = upper, b = fore (default)
        cosA = clamp((a^2 + d^2 - b^2)/(2ad)),  cosB = clamp((a^2 + b^2 - d^2)/(2ab))
        store v, (-sinA, cosA, sinB, -cosB)   (0x8005ad9c-0x8005ae60)
    pop -> G = spine (out[1])
    # pass 2 (robot dims D[], s = scale)
    G *= Tx(D[0]*s); push; G *= Tx(D[1]*s) * M[2] * Aim(dirHead)  -> out[2] (0x8005ba24); pop
    arm pass2 0x8005a09c (arm1 D[2..7] -> out[3..6], M[3,4,5]; arm2 D[8..13] -> out[7..10], M[6,7,8]):
        G *= Tx(D0*s) * M[shoulder]                      -> base   (0x8005a158)
        G *= T(D1*s, 0, D2*s) * M[j1pre] * Aim(v) * Rz(-A)  -> J1   (0x8005a48c)
        G *= Tx(D3*s) * Rz(180deg - B)                  -> J2 elbow (0x8005a568)
        G *= Tx(D4*s); handPos = G.t -> obj+0x168/0x174; G *= Tz(D5*s) * M[hand] -> EFF (0x8005a5e0)
    hips = T(root_pos) * Ry(lowerYaw) * M[0] * M[9] * Rz(-90deg)  -> out[11] (0x8005baac-0x8005bbec)
        (with flag 0x40 the Ry*M[0] is skipped: hips = root * M[9] * Rz(-90))
    leg pass1 0x8005a668 (from hips*Tx(0.22)): G *= T(j1.x,0,j1.z) * M[hip]; v = local(footTarget);
        a = thigh, b = shin; store v, (+sinA, cosA, -sinB, -cosB)
    leg pass2 0x80059790 (from hips*Tx(D[14]*s); leg1 D[15..20] -> out[12..15], M[10,11,12];
        leg2 D[21..26] -> out[16..19], M[13,14,15]):
        G *= T(D0*s, 0, D1*s) * M[hip] * Aim(v) * Rz(+A)  -> thigh J1
        G *= Tx(D2*s) * Rz(B - 180deg)                    -> knee J2
        G *= Tx(D3*s); G.rot = Ry(lowerYaw) * Rz(n24.z)Ry(n24.y)Rx(n24.x) * M[foot]  -> foot (0x80059cc0-0x80059fd0)
             (foot orientation is absolute: NOT chained through the leg)
        G *= T(0, D4*s, D5*s) * M[toe]                    -> toe (0x80059fd4-0x8005a014)

Spine/head/shoulder bend plane is the local XY plane after Aim (Rz rotations).

### Node -> joint table

| node | kind | drives |
|---|---|---|
| 0 | pos | pelvis/root position (through root matrix of 23/24/25; y scaled by k*scale) |
| 1 | rot | root/pelvis rotation M[0] (node 24 added) |
| 2 | rot | spine pre-aim rotation |
| 3 | pos | spine aim target (chest) -> rig slot 0 |
| 4 | rot | neck pre-aim rotation |
| 5 | pos | head aim target -> rig slot 1 |
| 6 / 10 | rot | arm1 / arm2 shoulder base -> slots 2 / 6 |
| 7 / 11 | rot | arm1 / arm2 J1 rotation before aim |
| 8 / 12 | pos | arm1 / arm2 hand IK target -> slots 3-4 |
| 9 / 13 | rot | arm1 / arm2 hand (EFFECTOR) -> slots 5 / 9 |
| 14 | rot | hips / lower body -> slot 10 |
| 15 / 19 | rot | leg1 / leg2 hip rotation before aim |
| 16 / 20 | pos | leg1 / leg2 foot IK target -> slots 11-12 / 15-16 |
| 17 / 21 | rot | leg1 / leg2 foot (absolute) -> slots 13 / 17 |
| 18 / 22 | rot | leg1 / leg2 toe -> slots 14 / 18 |
| 23 | pos | root translation (gated) |
| 24 | rot | root rotation (gated), added to node 1, also used for feet |
| 25 | pos | root pivot offset (gated) |

Side: arm1 = node 6..9 uses dims D[2..7] whose shoulder z is -0.158 and default z -0.18; arm2 positive z.
Leg1 = nodes 15..18, z -0.07 / -0.09. INFERENCE that arm1/leg1 are the user's "R" side, by sign of z
matching the user's dimension labels.

INFERENCE: rig slot r (of the 19 model tree paths) = out[r+1], i.e. out+0x30*(r+1); out[0] = root.
Supported by the order/count (spine, head, 4 arm1, 4 arm2, hips, 4 leg1, 4 leg2 = 19) and by
0x8009f844 reading translations of out+0x120 / +0x1e0 / +0x2a0 (hand, hand, foot). The code that binds
out[] to model joints was not traced.

INFERENCE: r13-0x4e7c points at the 32 per-robot floats found in the model file: indices used are
[0] spine top (0.2123 vs default 0.38), [1] neck (0.02 = default 0.02), [2..7] arm1, [8..13] arm2,
[14] hip drop (0.1588 vs 0.22), [15..20] leg1, [21..26] leg2; [27..31] unused here.

## 3. Unknowns

* Runtime value of scale = robotState+0x154 (robotBase+0x3c0) not determined (probably 1.0, not verified).
* obj+0x2c4..0x2cc (offset added to the chest target) source not traced.
* Gate table 0x8032a8c8 (whether nodes 23-25 apply) indexed by robotState+0x6a: meaning not traced.
* The blend path (flag 4, 0x8001e3b8 and 0x80050f90) is a transition blend; not analysed.
* 0x8003c4f0 post-adjusts out[] translations; not analysed.
* Frame-counter looping lives outside the evaluator; not traced.

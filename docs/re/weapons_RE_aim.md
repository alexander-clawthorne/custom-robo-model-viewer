# Weapons RE: muzzle, gun aim, arena edge, gun special ids

Source: main.dol (US) via tools/dol.py and Ghidra (build/ghidra/aim_a.c = FUN_80095a30, FUN_80010220, FUN_80044174;
aim_b.c = HSD helpers; aim_c.c = collision parser). Annotated copies: build/aim/move_ann.c, build/aim/w_acts_ann.c.
S = robot struct (0x8024e120 + n*0x33520), S_t = opponent (*(S+0x238)). Matrices are 0x30-byte, translation at +0xc/+0x1c/+0x2c.
Helper: `PYTHONUTF8=1 python build/aim/muzzle.py [--frame F] [gun|bom] [idx...]`.

"Verified" = read straight from instructions. "Inferred" = my reading of decompiled code, not run.

## 1. MUZZLE

### 1.1 Which joint (verified)
Part loader 0x80073cb8 (build/ghidra/loader.c). Per part file it walks the effect-spawn list (header word 8 =
`piVar10[8]`, a data-block offset): for EACH ROOT in order, one count byte n, then n paths; a path is bytes 02 (child) / 01
(next sibling) terminated by 0xff, walked from that root's JObj (a bare 0xff = NULL). The resulting joints are stored
CONSECUTIVELY ACROSS ALL ROOTS (one running pointer `r18`), into one array per part type:

    type (loader var)   root array (S+0x90c+4i)      spawn-joint array (S+0x8b8+4i)
    0 gun   0x80377c60+p*0x80  i=0..1                0x80377ca8  i=0..3   (4 entries)
    1 bomb  0x80377c68         i=2..3                0x80377cb8  i=4..5   (2 entries)
    2 pod   0x80377c70         i=4..7                none (loader leaves r18 = 0; all 57 weapon_pod.BIN have word 8 = -1)
    3 leg   0x80377c80         i=8..13               0x80377cc0  i=6..13  (8 entries)

0x80007ce4 (build/ghidra/parts.c) copies these into S:
- S+0x944 = S+0x90c  = gun root 0 (the drawn gun root; FUN_80095a30 target)
- S+0x948 = S+0x918  = bomb root 1 (the drawn bomb root)
- S+0x8f0 = S+0x8b8  = GUN spawn entry 0   -> bullet origin and direction joint
- S+0x8f4 = S+0x8bc  = gun spawn entry 1   (a second path on root 0; no reader of S+0x8f4 found)
- S+0x8f8 = S+0x8cc  = i=5 = BOMB spawn entry 1 -> bomb throw origin/direction joint
- S+0x8fc..0x908 = S+0x8e0..0x8ec = LEG spawn entries 3..? (dash/boost effect joints; dash.c reads S+0x8fc/0x904).
- Pods: no joint list. Pod launch origin is the robot matrix S+0x10d4 (weapons_RE_bomb_pod.md), not a part joint.

File facts (all 64 gun / 52 bomb files, checked by build/aim/muzzle.py):
- Gun: 2 roots, 4 paths = root0 {entry0, entry1}, root1 {entry2, entry3}. Muzzle = entry 0 = FIRST path of root 0
  (root 0 is the drawn root, mounted at rig slot 8 = out[9], CLAUDE.md "Robot assembly"). Path depth 6-20 child/next steps.
- Bomb: 2 roots, 2 paths = root0 {entry0}, root1 {entry1}. Throw joint = entry 1 = the path "02020202" (4 children
  deep, identical in all 52 files) from root 1 (drawn root, slot 4 = out[5]). Entry 0 hangs on the undrawn root 0.
- All 64 guns and all 52 bombs have the joint (muzzle.py: 64/64, 52/52).

### 1.2 Rest-pose transform relative to the root joint (part units; world = slot_matrix * 0.1 * rel)
rel = product of local SRTs (T*Rz*Ry*Rx*S) of the joints below the root; the root's own SRT is never used (0x80039fac).
"+X" is rel's first column normalised, in the root's frame.

    gun   path (hex)                       translation (x,y,z)          +X axis
    0     020202020102020101010102020202   (5.415, 1.050, 0.000)        (1,0,0)
    1     0202020202020101010202           (3.500, 0.840, 0.000)        (1,0,0)
    2     0202020202010101020102           (5.460, 0.000, 0.910)        (1,0,0)
    3     020202020201020102               (5.194, 0.566, 1.134)        (1,0,0)
    4     02020202010202020101010101010202 (6.048, 0.784, 0.000)        (1,0,0)
    bomb  (root 1, path 02020202)
    0     (3.850, 1.120, 0.000)  (1,0,0)      1  same as 0      2  same as 0

So in robot units the gun-0 muzzle is 0.54 forward and 0.105 to the side of the elbow matrix, bomb 0.385 / 0.112.
Muzzle axis is NOT +X of the root for some parts: guns 19 (0.707,0.707,0), 30 (0.5,0.866,0), 32 (0.866,0.5,0),
36 (0.945,0.327,0), 41 (0.5,0.866,0) tilt up in the root frame; bombs 4,6,19 point -Z, bombs 15,20,21,23,34 point -Y
(the throw direction used by the game is still this joint's +X, so those bombs leave sideways/down).

### 1.3 Does the rest pose suffice? (partly inferred)
- FUN_80095a30 (0x80095a30, aim_a.c) is NOT an animator. It walks the JObj tree (5 levels unrolled) and, for every joint
  with flag 0x40 (matrix dirty) and not 0x800000, calls FUN_8019a7e8 (HSD_JObjSetupMatrixSub), i.e. recomputes world
  matrices from the joints' stored SRT with the root's world matrix as parent. Called with S+0x944 right before the muzzle
  matrix is read (_scr_f.c:1907 / move1.c:674), then again with S+0x8f0 for the direction.
- The root matrix is the solved arm matrix written at draw time (0x80039fac: matrix copy, flags |= 0x3800000, scale 0.1).
- Gun root 0 carries a 10-frame AnimJoint (root record word 1 is non-null in all 64 gun files; bomb root 1 has none in all
  52). The game plays it with FUN_80095700(10 * S+0x20d4, root): S+0x20d4 ramps 0 -> 1.0 at +0.025/frame
  (0x80101538, function 0x80101284, gated by byte 0x4867(r31)) and is reset to 0.0 at 0x8010dcd0; so the gun
  "unfolds" over 40 frames to frame 10. 0x80095700 applies JObjAnim then requests the frame (apply lags one call), so the
  pose left behind is frame ~9.75 (assumption: AObj interpretation order read from 0x8018cdec/0x8018d20c).
- Result (muzzle.py --frame 10, FObj tracks 1-3 rot, 5-7 tra): frame-10 pose == stored rest pose for 53 of 64 guns including
  guns 0-4. Differing guns (|delta| = max over translation/axis components): 8 (0.92), 10 (0.21), 11 (0.21), 16 (0.77),
  31 (1.61), 36 (0.84), 41 (1.61), 43 (0.77), 45 (0.92), 47 (0.21), 48 (0.21). For those use `muzzle.py --frame 10`.
  Frame 0 (folded) differs more (gun 1: x 3.5 -> 2.45).
- Bombs: no animation on the muzzle root, rest pose is exact.

## 2. GUN AIM

### 2.1 Pipeline (verified structure, formulas inferred from decompile)
Per frame, battle robot update 0x8003b528: first loop runs FUN_8005aee8 (animation pose solve, writes out[] = S+0x1074);
second loop (0x8003c3a0) calls FUN_8003c4f0 per robot (attach.c) = AIM STAGE, which ends in FUN_80058ad0 (re-solve of
spine/head/arms from modified targets). Context set by 0x80050f08: obj = S+0x53c, R = S+0x70 (R+0x168 = S+0x1d8 flags,
R+0xe8..0xf0 = S+0x158..0x160 = aim vector V), out = S+0x1074.
Runs for every robot every frame in battle; it only DOES something when S+0x1d8 has aim bits.

### 2.2 Aim flags (S+0x1d8) and the desired direction D (0x8004b0a0, move.c 1040-1100)
Before the action handler (table 0x801f8eb8 by S+0x528): target point T (S+0x14c) = translation of opponent matrix
S_t+0x10a4 (opponent out[1], spine/chest) unless S+0x1d8 & 2, then T = V. Unit vectors, z negated:
`DAT_8032a5b4` = normalize(T - own S+0x10a4) (own spine -> target), `DAT_8032a5a8` = from own hips S+0x1284.
The handler sets global r13-0x4f18: 1 -> S+0x1d8 |= 0x2000000 (GUN arm aims, arm2), 2 -> |= 0x1000000 (BOMB arm aims,
arm1). (Mask 0xf4ffffff clears 0x0b000000 first; actions 9 and 10 then also set 0x8000000 = alternate target set, no torso lean.)
Writers of 1: action 3 (0x8004949c, ground gun, every branch; sets D = DAT_8032a5b4, i.e. toward the opponent), action 9
(0x80047ac0), 10 (0x8004752c), 8 (0x80048050), and action 0 (0x80049924) while firing/locked on (S+0x1f8 bit0 etc.).
Writer of 2: action 4 (0x800490c8, bomb; also sets S+0x164 = normalize(bomb cursor S+0x28ed4 - S+0x20), arm1 aims there).
S+0x1d8 bit 2 ("no look-at") is set once at 0x8003ac98 when the robot's opponent index S+0x228 is not in the active-player
mask r13-0x4ecc (opponent gone); not cleared there.
Smoothing (move.c ~1830, per frame, 0x8004b0a0 end): V = 0.5*V + 0.5*D where D = DAT_8032a5cc..d4 after the handler
(normalised by 0x8001ca84 first; V itself is not re-normalised). So V converges in ~5 frames, no angular rate limit.
Also S+0x170 and S+0x17c are 0.5/0.5 smoothed copies of DAT_8032a5c0 / DAT_8032a5d8 (arm-1 / legs-facing vectors);
S+0x64 = -atan2(S+0x17c, S+0x184) is the lower-body yaw (s16, 65536 = 360 deg).

### 2.3 What gets turned (inferred)
1. Upper-body yaw: 0x80054238 (motion.c 249-262): if (r13-0x4e40 & 0x30) (flags 0x1000000/0x2000000 mapped by 0x8005aee8)
   obj+0x1e4/0x1e8 = sin/cos(-atan2(V.x, V.z_raw)) instead of sin/cos(S+0x64). obj+0x1c/0x20 likewise. The root/spine
   frame is built from these, and chest/head/hand targets are rotated by this yaw (world copy). NO yaw limit: the torso
   faces the horizontal aim direction fully. The hips/legs keep the movement yaw (lowerYaw).
2. Torso pitch (0x8003c4f0, only if flags & 0x3000000 and not 0x8000000): p = atan2(V.y, hypot(V.x, V.z)) in s16 units,
   CLAMPED to [-0x238e, +0xe38] = [-50.0 deg, +20.0 deg] (0x8003c52c..). With w = atan2(-V.z, V.x) the chest target is
   replaced by root + |chest-root| * u, u = (-sin p * cos w, 1, -sin p * sin w) / sqrt(1 + sin^2 p), i.e. the spine
   leans AWAY from the aim direction when aiming up (max ~19 deg) and TOWARD it when aiming down (max ~37 deg).
   The same delta (new - old chest target) is added to head/hand targets (obj+0xf0..0x11c) and to out[1..10]
   translations (0x3c..0x20c), then FUN_80058ad0 re-solves spine/head/arms (Aim of the spine X axis to the new chest target,
   head Aim, arm two-pass IK of motion_RE_notes.md).
3. Gun arm (arm2 = nodes 10-13, out[7..10]): when S+0x1d8 & 0x2000000 and S+0x238 != 0 the arm2 hand IK target
   obj+0x114..0x11c (alt obj+0x2d4 when 0x8000000) is set to the opponent's matrix translation S_t+0x1080/0x1090/0x10a0
   = opponent out[0] = ROOT/PELVIS (attach.c 611-630; not the chest). Otherwise it is the animated hand target.
   Bomb arm (arm1) when 0x1000000: target = out[4].t + 2.0 * (S+0x164, S+0x168, -S+0x16c) (shoulder + 2 * cursor direction).
4. Head: shifted by the same delta as above, so it follows the torso; no separate head look-at limit found.
Limits: arm IK has none except reach (pass 1 uses cosA/cosB clamped to [-1,1], default lengths upper 0.28 / fore 0.26).
Opponent distance in an arena is >> 0.54, so the arm is fully extended: A = 0, B = 180 deg, the forearm +X axis equals the
Aim(v) axis.

### 2.4 Does the bullet direction point at the opponent? Formula
Gun root = elbow matrix out[9] (slot 8). Muzzle joint +X (rest) = (1,0,0) in that frame for guns 0-4, i.e. the forearm axis.
Bullet (0x80042ab8, fire code _scr_f.c 1895-1940, asm 0x80042bbc-0x80042d40):
    FUN_80095a30(S+0x944); muzzle pos P = translation(S+0x8f0 matrix + 0x44)
    if (S+0x1d8 & 2) == 0:   // opponent present
        id != 0x18 : FUN_80095a30(S+0x8f0); dir = normalize(+X column of the S+0x8f0 world matrix)
        id == 0x18 : dir = normalize(S_t+0x10d4 translation - P)      (see 4.)
    else (no look-at): if S+0x238 == 0: dir = V (S+0x158..); else same two cases as above.
With a straight arm, muzzle +X = unit(opp_root - shoulder_J1) for rest-pose-aligned muzzles (every gun except the tilted
ones above, which fire at +angle in the forearm frame). shoulder_J1 is the DEFAULT-skeleton arm origin of pass 1 (a few
tenths of a unit from the real one), and the origin of the bullet is P, about 0.1 off the shoulder->target line, so the
ray misses the aim point by ~0.1 (homing and the 0.01*radius hit capsules cover that).
If the opponent is "outside the limits": there are no yaw/arm limits; only torso pitch saturates ([-50,+20] deg of V's
pitch), the arm keeps pointing at the opponent root. So dir stays toward the opponent root; only the muzzle position
moves with the leaned torso. With S+0x1d8 bit 2 and no opponent, dir = V = last smoothed aim vector (forward).
Port formula: after the normal pose solve, (a) V <- 0.5 V + 0.5 normalize(opp.out[1].t - own.out[1].t) (z flipped as the game
does), (b) yaw of root/spine = atan2(V) around Y, (c) spine lean u above with p clamped, (d) hand target of arm2 = opp.out[0].t,
(e) re-solve; bullet dir = world +X of muzzle joint (rest rel * out[9] * 0.1).
Not verified at run time (no emulator trace): sign conventions of the z-negation and the exact lean direction.

## 3. ARENA EDGE (verified)
FUN_80010220 (aim_a.c 164-169), run once at battle init (guard _DAT_80321dac == 0), after the arena collision is parsed:

    0x8024e100 = *(f32*)(blk0 + 0)        centre x
    0x8024e104 = *(f32*)(blk0 + 8)        centre z
    0x8024e108 = (1.5 * *(f32*)(blk0 + 0xc))^2     (r2-0x7e80 = 1.5)
    0x8024e10c = 100.0f                   (r2-0x7eb8; no reader looked at)

blk0 = **(u32**)0x803756a8 = first block header pointer of the loaded collision (parser 0x80072a58 stores the header-offset
array at 0x8037569c+0xc = 0x803756a8; header layout in tools/collision.py: centre[3], radius, face_count, flags). Block 0 is
the arena shell (4 walls + floor), so the edge is its BOUNDING SPHERE: centre (x, z) and 1.5 x radius. Not a per-arena
table; computed from the collision file (bg_data.BIN odd entry 2N+1) by tools/collision.py:

    arena  centre (x,z)   radius   R=1.5*radius   R^2
    0      (0, 0)         18.788   28.182          794.2
    10     (0, 0)         18.788   28.182          794.2
    20     (0, 0)         17.944   26.917          724.5
    30     (0, 0)         20.629   30.944          957.5
All 40: R^2 = 794.2 for 0,2,5,6,8,10,11,14-18,21,27,29,32,34,36,38,39 (r 18.788); others: 1 793.9, 3 862.7, 4 836.6 (centre z 2.5),
7 310.5 (centre z 3.0), 9 650.4 (centre -1.2,-0.24), 12 734.9 (centre x 0.1), 13 794.2 (centre 0.05,-0.51), 19 787.1, 22 759.4,
23 1154.3, 24 1163.3 (centre 2.0,-2.0), 25 618.8, 26 1154.3, 28 668.3, 30 957.5, 31 650.3, 33 870.8, 35 830.3, 37 724.5.
(centre values listed as x,z; the stored y is ~15.) Reproduce:
`python -c "import sys;sys.path.insert(0,'tools');import collision;b=collision.arena(0).blocks[0];print(b.centre,b.radius)"`.
Bullets, bombs, pods then die when (x-cx)^2+(z-cz)^2 > R^2 (gun notes section 3 step 5).

## 4. GUN SPECIAL IDS
Init 0x80044174 (st3.c 366-372; aim_a.c 671+): `if (id == 0x1a || id == 0x18) G+0x26f00 = 1; if (id == 0x18) G+0x26f01 = 1;
if (id == 0x1e) G+0x26f02 = 1;` with G = S+0x1e20, so S+0x28d20/21/22. Gun id = S+0x1ce. Names (record +0x20; the US
build left these three in Japanese): 0x18 = 24 = Giant Gun (Japanese text 'Giant Gun'), 0x1a = 26 = Knuckle Gun (Japanese text 'Knuckle Gun'),
0x1e = 30 = Fang Gun (Japanese text 'Fang Gun').
Readers (all instructions with displacement 0x6f00/0x6f01/0x6f02 off G+0x20000):
- G+0x26f00 (ids 0x18, 0x1a): 0x80041810 in the hit test FUN_80041358 (move1.c 1552): after a candidate hit, FUN_8005d08c
  (radius 0.01, r2-0x7af8) sweeps the segment from the VICTIM centre (S_t+0x1dc area) to the bullet; if it hits arena geometry
  the hit is discarded. So these two cannot hit through walls/obstacles; all other guns can.
- G+0x26f01 (id 0x18 only): 0x80042bf0 and 0x80042c94 in the fire code: bullet direction = normalize(opponent matrix
  S_t+0x10d4 translation - muzzle pos) instead of the muzzle joint +X, i.e. perfectly aimed at the opponent's torso joint
  (S_t+0x10d4 = opponent out[2], rig slot 1 = neck/head joint per motion_RE_notes.md; the same matrix pods launch from), in both the normal and the no-look-at branch; the
  zero-vector case keeps the unnormalised value.
- G+0x26f02 (id 0x1e): the only read is 0x80043648 in the bullet update, immediately overwritten (no branch uses it, the
  value is dead). So the flag has NO effect. Gun 0x1e's special behaviour is a direct id test elsewhere: 0x80042ec8-0x80042f24
  (bullet spawn): for ids 0x17 (23) and 0x1e (30) the slot's effect handle (slot+0x310) is reset (FUN_80196c50(0) +
  FUN_80094dd8(0)) only for the FIRST bullet of a volley (G+0x2a6 == 0); all other guns reset it for every bullet.
  Cosmetic (restart of a shared bullet effect animation).

## Not settled
- Run-time confirmation of the torso lean/yaw formulas (signs of z negation, atan2 argument order of FUN_8001f060(y, x));
  they come from reading decompiles, not a trace.
- Whether the final frame (10) of the gun deploy animation is ever applied (apply-then-request order); matters only for the
  11 guns listed in 1.3 (use frame 9.75 .. 10).
- Meaning of 0x8024e10c (= 100.0) and of S+0x8f4 (gun spawn entry 1).
- What makes actions 9/10 set 0x8000000 beyond "alternate hand target set / no torso lean" (sky-dash shot).

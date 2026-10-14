# Custom Robo main.dol -- sign conventions for guns/pods, and robot-vs-robot push

World frame used below: the game's world (positions at S+0x20, matrices, bullet/pod positions), right-handed, +y up,
the battle camera looks down -Z (so +x is screen right). Facing yaw psi means direction (sin psi, 0, cos psi).
For a shooter with forward f (horizontal): LEFT = y x f = (f.z, 0, -f.x), RIGHT = -LEFT.
"Positive turn about +y by a" = port's rotY(v, +a) = yaw' = yaw + a = counter-clockwise seen from above = toward the shooter's LEFT.

## 0. The rotation helpers (decompiles: build/gunre/gun_a.c, build/ghidra/bp_a_rot.c)

* FUN_8001e60c(out, a, b) = a x b  (out.x = a.y*b.z - a.z*b.y ...). Right-handed cross product.
* FUN_8001b6ec(angle_u16_as_float, v, axis) and FUN_8001bf04(angle_radians, v, axis):
  identical maths, only the angle unit differs (b6ec: int(angle) & 0xffff, 65536 = 360 deg; bf04: radians * 65536/6.28319).
  They normalise the axis a, then v' = a (a.v) + cos(t) v_perp + sin(t) (a x v) -- the standard right-handed Rodrigues rotation:
  positive t turns v counter-clockwise looking down the axis toward the origin (from the axis tip), i.e. v -> a x v.
  The table at 0x801e6580 is a rising quarter sine (0, 0, 0.001536, ... 1.0 at index 1024); fVar1 = sin multiplies (a x v),
  fVar4 = cos multiplies v_perp; the bit 0x4000/0x8000 logic gives the right signs in all four quadrants.
* FUN_8001bd54(angle_radians, v): rotation about +Y only: x' = x cos + z sin ; z' = z cos - x sin.
  This is exactly the port's rotY(v, a).
* FUN_801690c8 -> 0x80165f58 = atan2(y = f1, x = f2) (fdiv f1/f2 at 0x80166158).
* FUN_8001b1a0(a, b, out) = b - a.   FUN_8001b1d4(in, out) = normalise.   FUN_8001e488(a, b) = |a - b|.
* r2 = 0x804581a0 constants used: r2-0x7b08 = 0.001, r2-0x7b4c = 1.0, r2-0x7b68 = 0.0, r2-0x7748 = 0.5.

## 1. Gun fan (FUN_80042ab8 slot loop, build/gunre/move1.c lines ~918-952)

At the first frame of phase P[8], for bullet index i of the volley (slot+0xa), with d = bullet dir (slot+0x1c):
  d = (0,0,1) if |d| == 0
  Lft = normalise(up x d)   [up x d = (d.z, 0, -d.x): horizontal LEFT of d]                              (auStack_16c)
  Dwn = Lft x d             [= (sin(phi)*hx, -cos(phi), sin(phi)*hz) for d at elevation phi, h = horizontal dir:
                             the bullet's own "down" axis; for horizontal d it is (0,-1,0)]              (auStack_178)
  ang = (s16)P[0xa] - (s16)P[0xc] * i                  (u16 units, 65536 = 360 deg)
  if P[3] == 0: FUN_8001b6ec(ang, d, Lft)  else  FUN_8001b6ec(ang, d, Dwn)
  then the cone: FUN_8001b6ec(P[6] * ((rand>>2 & 0x7fff)/16384 - 1), d, Lft), then the same about Dwn, with the
  SAME stale axes Lft/Dwn (computed before the fan); each jitter is uniform in [-P[6], +P[6]) so its sign is irrelevant.

World-space answers (derived from the code):
* P[3] == 0 (PITCH fan): rotation about LEFT by +ang:  d' = d cos(a) + (Lft x d) sin(a) = d cos(a) + Dwn sin(a).
  POSITIVE angle = bullet turns DOWN (elevation' = elevation - ang). NEGATIVE = up.
* P[3] != 0 (YAW fan): rotation about Dwn by +ang:  d' = d cos(a) - Lft sin(a).
  POSITIVE angle = bullet turns RIGHT = clockwise seen from above (for horizontal d: yaw' = yaw - ang, i.e. rotY(d, -ang)).
  NEGATIVE = left. (It is the rotation about the bullet's own up axis, negated; for a tilted d this differs slightly from
  "yaw -= ang", so use the vector formulas to be exact.)
* PORT BUG (src/weapon.c ~l.203): `if (P->fanAxis) yaw += fan; else pitch += fan;` has BOTH signs wrong.
  Correct: `yaw -= fan` (yaw fan) and `pitch -= fan` (pitch fan; pitch is positive-up in angles()/fromAngles()).
  Exact version, with L = normalise((d.z, 0, -d.x)):  pitch fan: d' = d cos a + (L x d) sin a ;  yaw fan: d' = d cos a - L sin a.
  Take the cone-jitter axes before the fan (the port recomputes yaw/pitch after it; negligible difference).

Cross-checks by gun data (ground / air patterns; a0 = P+0xa, ax = P+3; table in build/gunre/gun_table.txt):
* Pitch (ax 0): Rayfall (13) a0 = -10922 (-60 deg, ground and air) plus cone: negative = UP, and Rayfall's later phase (E1 120 f at
  speed 0.002 with homing 0.25) is "go up, hang, then dive on the target" -> consistent with negative = up.
  Dummy guns 56..63: ground -10922 (up 60), air +7281 (down 40): ground lobs up, air shoots down -> consistent with positive = down.
  Gravity Gun air -4551, Catch Gun ground -5461, Stardust -1820 (up-lobbing), Phoenix air +6371 (down).
  Fang (30) / dummy 8 (41) ground 23665 (130 deg) and Star Layer ground 10922 - 3094*(-i): > 90 deg, applied at phase 1/2, no use.
* Yaw (ax 1): Left Arc (10) ground a0 = -5461 (30 deg) -> NEGATIVE = LEFT -> bullets go to the shooter's LEFT. Right Arc (11)
  ground +5461 -> RIGHT. Left Loop (34, JP "Left Loop") ground -8192 (45 deg) -> LEFT; Right Loop (35) +8192 -> RIGHT.
  Both named pairs agree with the derived sign on the GROUND pattern.
  CAVEAT: the AIR patterns have the opposite sign (Left Arc air +5461, Right Arc air -5461, Left Loop air +8192, Right Loop air
  -8192). That is in the data; the fan code never looks at the air flag, so in the air the "Left" guns shoot to the RIGHT.
  Names alone cannot prove the absolute sign (a mirrored convention would make the ground pattern the odd one out); the sign
  comes from the maths, and the ground names agree with it.
  Symmetric fans (Triple Row +/-60, Hornet 50/25, Shotgun +/-12, Splash +/-30, Bubble +/-40) carry no information.

## 2. Pod launch yaw/spread (FUN_8003d7e0 @ 0x8003d7e0, build/ghidra/bp_a_pod.c)

Pod record offsets are P+8 shifted: spread s16 = record 0xe4 (P+0xec), yaw ground/air = record 0xec,0xee / 0xf0,0xf2
(P+0xf4,0xf6 / 0xf8,0xfa), count = record 0xe2 (P+0xea).
* dir (param_2) is S+0x104 normalised (the stick in the "velocity" space, z negated). Heading Q+0x1c = (dir.x, 0, -dir.z):
  that is the WORLD heading (pod pos/vel are world; velocity is added to the position unnegated).
* Spread (before the elevation): heading rotated with FUN_8001bd54(spread_i deg -> rad) = rotY(h, +spread_i):
  POSITIVE spread = LEFT (counter-clockwise from above).
  Values (integer division): N == 2: i -> spread/2 - spread*i (+s/2 then -s/2); N == 3: s/2 - (s/2)*i (+s/2, 0, -s/2).
  Negative spread (N == 3 only, no real pod uses it): i0 = (s+360)/2, i1 = 180, i2 = -(s+360)/2 degrees.
  So pod i = 0 goes to the LEFT, the last to the RIGHT.
* Elevation: axis = heading x up = (-h.z, 0, h.x) = RIGHT of the (spread-rotated) heading; FUN_8001bf04(elev) about it:
  (right x forward) = up, so POSITIVE elevation = UP (record 0x106/0x108, signed degrees). Velocity = result normalised
  * 0.1*speed * 0.01. (The port's rotY(h, spread) + cos/sin(elev) is right.)
* Homing heading: Q+0x1c is rebuilt from dir ((dir.x, dir.y, -dir.z); y not zeroed, not normalised) and rotated by
  FUN_8001bd54(-yaw_i deg) = rotY(heading, -yaw_i): POSITIVE yaw = RIGHT (clockwise from above).
  yaw_i: N < 2: s16 at P+0xf4 (+4 if airborne); N >= 2: ((a - b) * i) / (N-1) + b with a = s16[P+0xf4+air*4], b = s16[P+0xf6+air*4]
  (C integer division): i = 0 -> b (record 0xee/0xf2), i = N-1 -> a (record 0xec/0xf0).
* PORT: rotY(h, +spread) is right; rotY(q->heading, -yaw) is right; the "GUESS: the sign of both turns" comment can go.
Cross-check by pod data: the only pod with both a spread and yaws is Double Wave Pod (33): N 2, spread 50, yaw (rec 0xec, 0xee) =
  (30, -30) ground and air. Pod 0: spread +25 -> LEFT; yaw b = -30 -> rotY(+30) -> heading also LEFT. Pod 1: spread -25 -> RIGHT,
  yaw a = +30 -> rotY(-30) -> RIGHT. Launch and homing turns agree on each side only if the two signs are opposite, as derived
  (this checks the RELATIVE sign; the absolute left/right comes from rotY and the z negation).
  Sideway Pods (28/29): N 2, spread 180 -> +90 (left) and -90 (right), yaw 0. Wall Pod (24): N 3 spread 25 -> 12, 0, -12 (integers).
  Ricochet (25) / Umbrella (30): N 3 spread 90 -> 45, 0, -45. Rear Shoot (26/27, dummy 37): N 1, yaw 180 -> homing heading starts reversed.

## 3. Pod steering (FUN_8003dd38 @ 0x8003dd38, build/ghidra/_scr_f.c lines 430+, block after label 8003df28)

  delta = target S+0x20.. - pod pos (FUN_8001c9c0), delta.y = 0.
  Lft = up x heading = (h.z, 0, -h.x)   (FUN_8001e60c(16c, 16c = (0,1,0), heading))
  e = atan2( dot(Lft, delta), dot(heading, delta) )     (FUN_801690c8(f1 = dot(Lft,delta), f2 = dot(heading,delta)))
  heading = rotY(heading, e * 0.001 * gain)  (FUN_8001bd54), heading.y = 0, normalised.   gain = u8 at P+0xfd + L (record 0xf5 / 0xf6)
* e > 0 when the target is on the heading's LEFT; rotY(+) turns LEFT. So it turns TOWARD the target. CONFIRMED.
  Proportional (e*0.001*gain of the error per frame, a gain of 100 closes 10 % per frame), not a rate limit.
* The port's err = atan2(h.z*d.x - h.x*d.z, dot(h,d)) is the same dot(Lft, d), and rotY(heading, err*0.001*gain) the same turn.
* Lock index: the game reads L = (Q+0x41 != 0) at the TOP of the homing branch (decompile line 96), BEFORE the lock test rewrites
  Q+0x41, and uses that OLD L for both gain (P+0xfd+L, line 133) and speed (P+0x100+2L, line 146). The port uses gain[was]
  (correct) but speed[q->locked] (the NEW state): a one-frame difference, worth changing to speed[was].

## 4. Robot-vs-robot push and the wall sphere

Spheres, rebuilt each frame (FUN_80062ff4 @ 0x80062ff4, and the tail of 8003b528 @ 0x8003bcb0..; B = body record = S+0x1b98):
  f = 0.01 * u16 B+0xa8 (S+0x1c40)          u = 0.01 * u16 B+0xa6 (S+0x1c3e)
  A centre (S+0x3343c..) = out[2].t * (1 - f) + out[11].t * f   [out[2].t at S+0x10e0/0x10f0/0x1100, out[11].t at S+0x1290/0x12a0/0x12b0;
                                                                 matrices at S+0x1074 + 0x30*k]
  A radius (S+0x33478) = 1.4 * u   (consts r2-0x7758 = r2-0x7af4 = 1.4, r2-0x7760 = r2-0x7af8 = 0.01)
  B centre (S+0x33448..) = out[2].t        B radius (S+0x3347c) = u
  (f can exceed 1: the Agilia family a8 = 120 puts A 20 % beyond out[11].)  The formula in the task is CONFIRMED.
Wall: FUN_80061be8 -> FUN_80061620: sphere A (radius S+0x33478) at S+0x3343c against all collision blocks whose flag word
  & 0x40000003 == 0 (block 0 is NOT special in this loop), via 8005ed28; the result moves the centre and the delta is accumulated
  into S+0x3346c..0x33474, which FUN_80063654 adds to S+0x20 AND to all 20 joint matrices. The correction is full 3-D (y too).
  If |A - S+0x33454| < tiny (r2-0x777c) A.x is nudged first (avoid a zero-length sweep). Which of the two sweep end points is
  "start" is not settled (A at S+0x3343c vs S+0x33454, the previous A?).
Floor/feet: SEPARATE. In the main update (build/ghidra/move_a.c ~l.1650): floorY = FUN_8006207c(A.x (S+0x3343c), out[0].t.y (S+0x1090)
  [+0.1 if HP < 1], A.z (S+0x33444), 1.0). hgt = max(out[2].t.y - S+0x24, S+0x17f4 * S+0x1c4) (S+0x17f4 = dimension word 0 = height,
  Ray 1.31; S+0x1c4 a scale). Airborne (S+0x139 == 2): lands when (S+0x17f4*S+0x1c4 + pos.y - floorY) < hgt - 0.1; grounded: stays/
  snaps when that value is < hgt + 0.5. Landing/snapping sets pos.y = floorY. So the floor is found by a down query from the
  foot-side joint out[0] at sphere A's x/z, not by sphere A touching the floor; sphere A (chest height, radius 0.49 for Ray) only
  pushes against walls/ceilings/slopes and the other robot.
Robot vs robot (FUN_80063da4 @ 0x80063da4 -> FUN_800633bc @ 0x800633bc; per player, called from the per-player update at
  _scr_f.c:190 BEFORE the wall sweep; skipped if S+0x1d8 & 0x40000 on either robot):
  for each other robot k in mask _DAT_8032a5f4[player] (and not 0x40000):
    flags (S+0x33480) = 0; if |myA - theirA| <= rA + rA' : flags |= 1; if |myA - theirB| <= rA + rB' : flags |= 2.
    If (S+0x33480 != 0 || S+0x33484 != 0) call push(me, them). S+0x33484 is only ever read (no store found), so it stays 0 and
    the push only happens when flags != 0.
  push: flags & 1: spheres (myA, theirA); else (flags == 2) (myA, theirB). [flags == 0 branch, myB vs theirB, is unreachable.]
    depth = (r1 + r2) - |c1 - c2|;  n = normalise(c1 - c2) (FULL 3-D incl. y);
    c1 += 0.5 * n * depth;   c2 -= 0.5 * n * depth.    Equal split, independent of velocity or mass. Not horizontal-only.
  After the push each robot's correction accumulator (S+0x3346c..0x33474, later added to S+0x20 and all joints) gets
    (A_now - A_before). A-A case: both robots move by half the overlap. QUIRK: in the A-vs-B case (flags == 2) the other robot's
    B sphere is what moved, but its accumulator receives its A delta (= 0), so only the pusher moves, by half the overlap.
  Two Rays side by side touch at 0.98 (= 2 * 0.49). Both players run the routine in their own update, so the second pass sees an
  already touching pair (depth ~ 0).
Hit centre (8003b528 @ 0x8003ba40): C = 0.5 * (out[2].t + out[11].t) (r2-0x7b58 = 0.5) -> S+0x1dc..0x1e4; if B+0xae != 0:
  C = pos + (2.0 * (C - pos)) / 3.0 (r2-0x7b54 = 2.0, r2-0x7b50 = 3.0, pos = S+0x20..0x28): the centre is pulled 1/3 of the way
  toward the feet (the S+0x20 origin). Radius 0.01 * B+0xac. Used by 0x80041358 (bullets) and the blast tests. This is NOT
  sphere A (B+0xa8 does not enter it).

Body record u16 dump (B[0xa6], B[0xa8], B[0xac], B[0xae]), bodies 0..33 (record 0x801c6c44 + b*0x134):
 id  a6  a8  ac  ae   name
  0  35  50  30  1   Ray 01
  1  35  50  30  1   Splendor
  2  35  50  30  1   Lava
  3  35  50  30  1   Coil (Milky Way)
  4  35  50  30  1   Earth
  5  35  50  30  1   Spica
  6  48  40  48  1   Bronzia
  7  48  40  48  1   (Metal Bear)
  8  48  40  48  1   Metal Ox
  9  35 120  18  0   Agilia
 10  35 120  18  0   Peking Duck
 11  35 120  18  0   Peregrine
 12  35  50  30  1   (Javelin)
 13  35  50  30  1   Bullova
 14  35  50  30  1   Halbert
 15  40  50  50  1   Criminal
 16  40  50  50  1   Arius
 17  40  50  50  1   Juggler
 18  35  72  28  0   Defender
 19  35  72  28  0   Berias
 20  35  72  28  0   Breaker
 21  48  45  40  0   Strato
 22  48  45  40  0   Pleasure Head
 23  48  45  40  0   Hover Head
 24  35  50  30  0   (old Ray)
 25  35  50  30  0   (hidden armor)
 26  35  50  30  0   (NG Ray)
 27  48  50  40  1   Crystalium
 28  35  72  28  1   Ruhiel
 29  35  50  30  1   Artemis
 30  35  72  28  1   R Type 1
 31  35  72  28  1   R Type 2
 32  35  72  28  1   R Type 3
 33  48  40  48  1   (Chicken robot)
 rA = 0.014*a6: 35 -> 0.49, 40 -> 0.56, 48 -> 0.672. rB = 0.01*a6: 0.35 / 0.40 / 0.48. Hit radius 0.01*ac: 0.30/0.48/0.18/0.50/0.28/0.40.
 B[0xaa] (u16 40/45/50/55) is not read by anything found.

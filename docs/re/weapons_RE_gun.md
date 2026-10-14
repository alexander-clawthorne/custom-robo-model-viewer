# Custom Robo (GCN, GXCE01) main.dol -- guns, reverse-engineering notes

Scope: gun record layout, firing state machine, projectile model, hit hand-off.
All addresses are main.dol RAM addresses. Ghidra output (annotated) is in build/gunre/*.c
(move1.c = FUN_80042ab8 + FUN_80041358, move_h.c = hit reaction, mv_states.c = action 3 / 9,
gun_a.c = math + sweep helpers). Machine-readable dump of all 64 guns: build/gunre/guns.json
(and gun_table.txt); script build/gunre/dump.py.

Conventions
- S = robot struct, S_n = 0x8024e120 + n*0x33520 (NOT 0x8024e140: every Ghidra constant
  `n*0x33520 + -0x7fdb1ee0` is 0x8024e120; e.g. FUN_80041358 victim selection).
- G = gun block = S+0x1e20. The 0x28c record is copied to G+8 (S+0x1e28) by 0x80005da4.
  So record offset r  <->  G+8+r  <->  S+0x1e28+r.
- Angles: "u16 angle" = 65536 per turn (spread/fan values). Homing limits are radians per frame.
- Distances/speeds of bullets are plain world units (no 0.01), per frame (60 Hz). Robot
  velocity (S+0x10) stays in hundredths/frame.
- Velocity-convention note: world z is negated in the S+0x10 velocity space (pos.z -= 0.01*vz).

## 1. Gun record (0x801ca560 + g*0x28c, 64 guns)

Names (cp932, 0x20 bytes each): +0x00 JP name, +0x20 US name (only ~10 filled in; others
repeat/placeholder), +0x40 bullet model name ("Bullet 1".."Bullet 24", "C2 Bullet n"),
+0x60 gun part model name, +0x80 hit effect name ("MG hit"). Names of guns 0..33 are real; 34..44
are unused dev guns ("dummy gun n" in the US slot, real JP names such as Left Loop, Circle
Sword, Thunder Gun, Flare Cannon, Angel Ring, Ripple Laser, X Laser, Meteor Flicker); 45..51 are
"illegal" guns (Orochi, Garuda, Luna/Diana Orbit, Ragnarok 1st-3rd); 52..63 empty dummies.
Record +0xa0 = "illegal" flag byte (0 for guns 0..41, 1 for 42..63) -- same flag stats.py notes.

| record off | G off | type | meaning | evidence |
|---|---|---|---|---|
| 0xa1 | G+0xa9 | u8 | GROUND-only cooldown frames after the last volley, before the recovery timer starts (0 for air) | 0x80042ab8: `*(G+0x2aa) = *(byte*)(G+0xa9)` after last volley when state==1 |
| 0xa2 | G+0xaa | u8 | homing mode: 0 = always home; 1 = home only while the target is in front of the bullet (dot(to_target, dir) > 0) | same fn, homing block |
| 0xa4 | G+0xac | u32 | 1 on guns with a coloured tracer (not read by the gameplay code I traced) | scan: no load found |
| 0xa8..0xb7 | | 4 x RGBA | tracer colours | cosmetic, unverified |
| 0xb8 | | u32 | tracer length 60 (15/20/6/35/... on tracer guns) | cosmetic, unverified |
| 0xbc | | u32 | always 1 | unknown |
| 0xd0 | | f32 | 5.0 on guns 10, 11 only | unknown |
| 0xd8 | | f32 | 1.0 (0.8 Sniper, 2.0 gun 32, 0.8 gun 4?) visual scale | cosmetic, unverified |
| 0xdc | G+0xe4 | pattern block | GROUND pattern (0xd8 bytes) | selected when G+0x2a4 == 1 |
| 0x1b4 | G+0x1bc | pattern block | AIR pattern (0xd8 bytes) | selected when G+0x2a4 == 2 |

Everything else (0x1b4+0xd8 = 0x28c) is the end of the record; no other gun-record field is read
by the battle code (I scanned the DOL for displacements S+0x1e28..S+0x20b4: only 0x1f04/0x1fdc
and the G-relative reads in FUN_80042ab8/FUN_80041358 touch it).

### Pattern block P (0xd8 bytes; same layout for ground and air)

| P off | type | meaning |
|---|---|---|
| +0 | u8 | wind-up frames after the press (becomes S+0xe2/S+0xe0): robot is frozen before the first volley. S+0x1f04 / S+0x1fdc are P+0 of ground/air |
| +1 | u8 | number of volleys (G+0x2a6 < P[1]) |
| +2 | u8 | bullets per volley |
| +3 | u8 | fan axis: 0 = fan in pitch (rotate about right = up x dir, "vertical" guns e.g. Angler), non-0 = fan in yaw (rotate about right x dir) |
| +4 | u8 | interval counter limit: next volley fires when counter >= P[4] -> volleys are P[4]+1 frames apart |
| +5 | u8 | recovery frames after the last volley; actual = trunc(P[5] * 0.01*body[0x4f]) (body[0x4f] = 100 normally) |
| +6 | s16 | random cone: each bullet gets +-P[6] (u16 angle) jitter about the pitch axis and again about the yaw axis (uniform) |
| +8 | u8 | phase index at whose FIRST frame the fan/jitter direction is applied (0 = at launch; guns 21,22,30,36,38,41 use 1..2) |
| +9, +0xe | | always 0 in all 64 guns (padding) |
| +0xa | s16 | fan start angle (u16 angle) |
| +0xc | s16 | fan step: bullet i (0..P[2]-1) gets angle P[0xa] - i*P[0xc] |
| +0x10 + k*0x28 | 5 x entry E | phases k = 0..4 (zero duration = skipped) |

Fan evidence: Triple Row (gun 1): a0 = step = 10922 = 60 deg, 3 bullets -> +60,0,-60 yaw. Hornet
(6): 9102 (50 deg) / 4551 (25 deg), 5 bullets. Angler (3): pitch, 0 and -9102.
Decompile 80042ab8: `FUN_8001b6ec(P[10]-P[12]*idx, dir, axis)`; FUN_8001b6ec takes the angle as an int
masked to 16 bits and uses the sin table at 0x801e6580 (so 0x10000 = 360 deg).

### Phase entry E (0x28 bytes; E = P + 0x10 + phase*0x28)

| E off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | u16 | phase duration in frames (sum over phases = lifetime) | 0x80041358 phase advance: `if E.dur <= ++frames` |
| +0x02 | u16 | always 0 | |
| +0x04 | f32 | speed, world units per frame (gun 0: 0.2208 ground, 0.17 air; Gatling 0.30; Sniper 0.42->0.26) | `FUN_8001ca28(E+4, pos, dir)` |
| +0x08 | f32 | yaw homing limit, radians/frame (0 = no turning) | homing block |
| +0x0c | f32 | pitch homing limit, radians/frame | homing block |
| +0x10 | f32 | bullet radius for the hit test (gun 0: 0.30) | 0x80041358 `dVar23 + E+0x10` |
| +0x14 | u8 | trail/beam length in frames AND linger frames after impact (0 for most; Dragon 40, V-Laser 60, Phoenix 50) | state-1/2 segment count `min(E+0x14+1, frames-1)`; state 2 dies when `E+0x14 < ++frames` |
| +0x15 | u8 | always 0 | |
| +0x16 | u16 | damage (gun 0: 28 in phase 0, 20 in phase 1 -> damage falls off with range) | -> H+0x14 |
| +0x18 | s16 | stun/down value added to the victim's gauge S+0x144 (gun 0: 280; Shotgun 1800) | -> H+0x16 |
| +0x1a | s16 | flinch push speed (x body[0x43]/1000, hundredths/frame) | -> H+0x18 |
| +0x1c | s16 | flinch push angle, degrees (0 = horizontal, 90 = up) | -> H+0x1a |
| +0x1e | s16 | flinch stun frames (x body[0x44]/100) (gun 0: 20 then 5) | -> H+0x1c |
| +0x20 | s16 | copied to H+0x1e; not read by any consumer I found (100 on most guns) | |
| +0x22 | s16 | knockdown launch speed (x body[0x43]/1000, hundredths/frame) (gun 0: 100 -> 10) | -> H+0x20 |
| +0x24 | s16 | knockdown launch angle, degrees (gun 0: 35 ground, 25 air) | -> H+0x22 |
| +0x26 | s16 | copied to H+0x24; not read by any consumer found (100/80/0) | |

Selected dumps (ground; air in guns.json). V = volleys, x = bullets/volley, w = wind-up, i = P[4],
rec = P[5], cd = record+0xa1, home = record+0xa2. Entry: dur speed yaw/pitch radius dmg stun.
- 0 Striker Gun (JP Basic Gun): w5 V3 x1 i10 rec40 cd10 home1. E0 25f 0.2208 .01/.01 r.30 dmg28 stun280 push10 stunframes20 launch 100@35deg;
  E1 157f 0.2208 no homing dmg20 stun200. Range 182 f = 40 units. Air: w5 V3 i10 rec50 spd .17.
- 1 Triple Row: w5 V3 x3 yaw-fan 60deg i6 rec20 cd20 home0. E0 7f .24 pitch-limit .04 r.16 dmg23 stun200; E1 5f speed 0.001 homing .2/.2 (stall and re-aim at the
  target); E2 159f .24 dmg21.
- 2 Six Barrel (Gatling): w5 V8 x1 i4 rec40 cd0 home1; .30/f r.10 dmg 9 then 13; stun150.
- 4 Sniper: w5 V1 i10 rec47 cd25; .42/.35/.26 per 10/12/122 f, dmg 100/80/70, stun 800, launch 170@35.
  Air pattern: w15 (long wind-up), i3, dmg 80/70/60.
- 12 Shotgun: V1 x3 yaw-fan 12 deg (2184); 4f/4f/5f life, speed .30, dmg 45/35/30, stun 1800 (instant knockdown), rec30 cd30.
- 13 Rayfall: V4 x1 pitch, random cone 5461 (30 deg) ground / 1820 air, fan start -10922; E1 120f speed .002 homing .25 (hangs, then dives on target).
- 6 Hornet: V1 x5 yaw-fan 50/25 deg, E0 15f then E1 10f stall with homing .25 then E2 .11 speed.
- 10/11 Left/Right Arc: V2 x1 yaw offset -/+5461 (30 deg), E1 5f stall .25 homing -> boomerang-ish.
- 8 Dragon: V1, dur 97+145, .165, dmg 108, stun 900, trail 40 f.
- 16 V Laser / 43 X Laser: speed .42, 125 f, trail 60 f beam, stun 2000 (always knocks down).
- 26 Knuckle: dur 1 f, radius 1.0, dmg 144, stun 2000 (melee-range punch; gun id 0x1a gets the wall check flag).
- 24 Giant Gun: speed .10, radius 1.1/1.5, dmg 40, stun 500; id 0x18 aims straight at the target.

## 2. Firing

Input: button bit 0x10 in the just-pressed mask S+0x242 (S+0x23e is the held mask). Holding does
nothing; each volley sequence needs a new press (checked in 0x80049924 near the press at the
`(S+0x242 & 0x10)` test, decompile move.c ~line 616-675).

Press (0x80049924, only when S+0xe2 < 1 and gun state S+0x20c4 == 0):
- S+0x528 (action) = 3, S+0x13a = 0, velocity S+0x10..0x18 = (0,0,0): the robot stops dead.
- pattern: S+0x139 == 2 (airborne flag; set when jumping/knocked up, 1 after landing) -> air block
  S+0x1fdc, else ground block S+0x1f04. S+0xe2 = S+0xe0 = P[0] (wind-up).
- From the sky-dash (action 8, 0x80048050) a press (bit 0x10, state==0) enters action 9 with the
  AIR block's wind-up; the dash steering/speed keep running (FUN_80047ac0 mirrors sky-dash motion).

Action 3 handler FUN_8004949c (table 0x801f8eb8 entry 3):
- Each frame: if S+0xe2 == 1 and gun state == 0 -> state = (S+0x139==2 ? 2 : 1), S+0x20ca = 0,
  S+0x20c6 = 0, S+0x20c8 = 0x100 (so the first volley fires at once). Then S+0xe2-- while > 0.
- Leaves the action (S+0x528 = 0) only when gun state == 0 and S+0xe2 < 1. There is no cancel: while
  in action 3 only this handler runs (hit stun still interrupts, see below).
- Air (S+0x139 == 2): S+0x14 (vy) = 0.1 * -(s16 at 0x801c6c0c = 40) = -4.0 hundredths/frame
  EVERY frame, vx/vz stay 0 (set at press): the robot sinks slowly (0.04 u/frame, 2.4 u/s) during the
  whole sequence. Ground: nothing moves.
- Animation hints only: motion ids 0x14 (ground fire) / 0x19 (ground recovery), 0x15/0x16 air.

Gun state G (FUN_80042ab8, called every frame as (0.01*body[0x4c], 0.01*body[0x4f], G)):
- G+0x2a4 (S+0x20c4) state: 0 idle, 1 firing with ground pattern, 2 with air pattern.
- G+0x2a6 (S+0x20c6) volleys fired so far; reaches P[1] after the last volley, then P[1]+1 in recovery.
- G+0x2a8 (S+0x20c8) frames since the last volley; fire when >= P[4].
- G+0x2aa (S+0x20ca) post-sequence timer.
Per frame in state != 0: if volleys < P[1]: if counter < P[4] counter++ else fire a volley
(P[2] bullets), counter = 0, volleys++. When volleys reaches P[1]:
- ground: timer = G+0xa9 (record 0xa1) frames, then volleys = P[1]+1 and timer = trunc(P[5]*body[0x4f]/100),
  then state = 0.
- air: volleys = P[1]+1, timer = trunc(P[5]*body[0x4f]/100) immediately, then state = 0.
Volley k fires k*(P[4]+1) frames after the first; the first fires about P[0] frames after the press.
Total lock time ~ P[0] + (V-1)*(P[4]+1) + [ground] cd + trunc(P[5]*b4f/100) + 1..2.
Gun 0 ground: 5 + 22 + 10 + 40 = 77 frames (1.28 s). Air: 5 + 22 + 0 + 50 = 77.

Interrupts (0x8004b0a0 hit reaction, move_h.c ~1497): when the shooter flinches or is knocked down
(S+0x1db8 == 0) the sequence is cut: if volleys <= P[1] -> timer = P[5], volleys = P[1]+1 (straight to
recovery, remaining volleys are lost); bombs are cancelled too. When knocked down by a GUN hit
(H+2 == 1) all of the victim's own bullets in flight are deleted (move_h.c ~1480, effect 0x3007d).

Muzzle / spawn (per bullet, first loop of 0x80042ab8):
- FUN_80095a30(*(S+0x944)) updates the gun part's joints; muzzle position = world translation
  (column 3, offsets 0xc/0x1c/0x2c of the matrix at joint+0x44; FUN_8001cef4) of joint *(S+0x8f0).
  S+0x944 = S+0x90c, S+0x8f0 = S+0x8b8 (parts.c 0x944/0x8f0 assignments): the gun part's root 0 and the
  FIRST effect-spawn joint of that root (part-file header word 8 list). All bullets of a volley use the same
  point (the fan is angular).
- Direction: +X axis of that joint's world matrix (column 0, FUN_8001cf10), normalised. For gun ids
  0x18 (and when G+0x26f01 set by 0x80044174) it is instead (opponent matrix translation at S_t+0x10d4 - muzzle)
  normalised. The joint orientation comes from the pose solve (aim toward the opponent; FUN_80058ad0
  feeds it the opponent's matrix translation at S_t+0x1080/0x1090/0x10a0) -- the aim limits were not traced.
- Spawn sweep: segment from the robot's matrix translation S+0x1284(+0xc) to the muzzle through the
  arena with radius 0.01; if blocked the bullet is placed at the hit point and dies at once (so you can't
  shoot through a wall you stand against).
- Slot: first free of the first n slots (state byte == 0). n = byte at 0x801f9081 + g*0x14 (20 for gun 0,
  28 Triple Row, 30 Gatling, 16 Sniper...; table 0x801f9074+g*0x14: +0 u32 effect id, +4 u32, +8 u32 impact
  effect 0x30007, +0xc u8 kind 1-4, +0xd u8 n, +0x10 f32 1.0). If none free the remaining bullets of the volley are dropped.

## 3. Projectile

Storage: G+0x300 + i*0x1360, 32 slots (the init loop runs n slots; the per-frame update loop always
walks all 32 = S+0x2120 .. S+0x28d20; S+0x28d20/21/22 are three flags set at init for gun ids 0x1a/0x18 (+0x28d20),
0x18 (+0x28d21), 0x1e (+0x28d22)). Slot layout (offsets from slot base):
+0x00 ptr to pattern block P; +0x05 state (0 free, 1 flying, 2 dead/trail); +0x06 phase; +0x08 u16 frame in
phase (state 1) or frames since impact (state 2); +0x0a bullet idx in volley; +0x0b u8 mask of players already
hit (1<<n); +0x0c u16 trail counter; +0x10 pos xyz; +0x1c dir xyz (unit); +0x28 ring buffer of 60 x xyz
positions (trail / swept hit test); +0x34c victim pointer = S+0x238 (opponent); +0x1340 air flag (state==2);
the rest (+0x300...) are effect handles.

Per frame (FUN_80042ab8 slot loop, then FUN_80041358; 80042ab8 runs first, so a new bullet already moved once):
1. state 1, entry E = P + 0x10 + phase*0x28.
2. If phase == P[8] and frame == 0: apply the fan and cone to dir (see P+3/+6/+0xa/+0xc).
3. Homing, if the slot has a target and (record 0xa2 == 0 or dot(to_target, dir) > 0): aim point = translation
   of the matrix at S_t+0x10a4; yaw: angle between horizontal(dir) and horizontal(to_target) via FUN_8001c5ac,
   rotate about Y by min(angle, E+8) toward the target; pitch: elevation(dir) - elevation(target) via atan2
   (0x801690c8), rotate about right axis by min(|diff|, E+0xc). Radians per frame. Then renormalise.
4. old = pos; pos += dir * E+4 (no gravity, no acceleration -- "acceleration" is done by phases with
   different E+4, e.g. 0.001 stall phases while homing hard).
5. Arena: FUN_8005d08c(0.01, new, old) = sphere sweep radius 0.01 against collision blocks 1..N-1 (blocks flagged
   0x80 are skipped, a hit on a block whose flag word has 0x40 is ignored and treated as a miss; block 0 = floor
   is tested via FUN_80061a18 + the floor-height query FUN_8006207c(x,y,z,0.01) -> hit if bullet y < floor).
   Hit: state = 2, pos = contact point, wall mark FUN_8006fdd0(E+0x16 damage, surface). Also state = 2 if the bullet
   leaves the arena circle: (x-_DAT_8024e100)^2 + (z-_DAT_8024e104)^2 > _DAT_8024e108.
6. Ring buffer write, then FUN_80041358:
   - victim candidates: players in the mask _DAT_8032a5f4[S+0x8c] (can stop the bullet) / _DAT_8032a604[S+0x8d]
     (take damage).
   - victim sphere: centre S_t+0x1dc (midpoint of the world positions of two body joint matrices,
     0x8003ba78: 0.5*(S+0x10e0 + S+0x1290) etc.), radius 0.01 * u16 at S_t+0x1c44 (= body record +0xac: Ray 0.30,
     Agilia 0.18, Criminal 0.50).
   - bullet shape: capsule over the last min(E+0x14+1, frames-1) segments of the ring buffer (a 1-frame segment
     normally), radius E+0x10. Test: if the victim centre projects inside a segment -> perpendicular distance <
     victim_r + E+0x10; past an end -> distance to that end < victim_r (bullet radius NOT added); plus a sphere test
     at the current position with victim_r + E+0x10.
   - guns 0x18/0x1a (flag at G+0x26f00) also require a wall-free line from victim centre to the bullet.
   - each bullet hits each victim once (slot mask). Hit while flying: state = 2 (bullet ends); hits during the
     state-2 linger (trail) are allowed for E+0x14+1 frames.
   - phase timer: `if (++frame >= E.dur) { phase++; skip zero-duration phases; frame = 0 }`; phase > 4 -> state 0
     (silent expiry).
Typical numbers: gun 0 ground 0.2208 u/frame = 13.2 u/s, homing 0.01 rad/f (0.57 deg/f) for the first 25 f only.

Hit hand-off (victim struct H = S_t+0x18a0 = the global the reaction code keeps in r13-0x4f1c):
 H+0 u8 read by 0x80041358 (==1 picks impact effect 0x30012; writer not found), H+1 hit counter++, H+2 u16 hit type (gun = 1;
 melee 4 (0x800450ec); 5 is written by 0x8004b0a0 itself at move.c:1973 with fixed numbers, purpose not traced), H+4 = attacker S, H+8/+0xc/+0x10 = bullet travel direction (z negated, unit, y kept),
 H+0x14 u16 = (u16)(E+0x16 * 0.01*attacker body[0x4c] + previous H+0x14) (damage accumulates until consumed),
 H+0x16 += E+0x18, H+0x18..0x24 = E+0x1a..0x26, H+0x26 = 0. Writers: 0x80041914..0x80041ab4 (state 2 branch),
 0x8004204c..0x8004223c (state 1 branch).

Consumer (0x8004b0a0, move_h.c ~1120-1500), victim body record B = S+0x1b98:
- HP S+0x74 (float, default 1000 from the loadout row; HP bar via 0x8007ab14):
  normal hit: HP -= H+0x14 * B[0x42] / 90  (1000 * (H14*B42/900)/100; DAT_801c6c00 = 900).
  while already knocked down (action 0xe/0xf): HP -= H+0x14 * B[0x47+type] * B[0x42] / 9000 (gun: B[0x48] = 30 -> 30 %).
- stun gauge S+0x144 (s16): += H+0x16 on each hit, decays 1/frame (move.c 1102). If it stays < B[0x40] (u16 per
  body: Ray 1000, Coil 800, Bronzia 1800, Agilia 200): FLINCH: action 0xd for trunc(H+0x1c * B[0x44]/100) frames, velocity =
  dir_h*cos(a)*m, vy = sin(a)*m with a = H+0x1a deg and m = H+0x18 * B[0x43] / 1000 (hundredths/frame), airborne flag set.
  Otherwise KNOCKDOWN: S+0x144 = 0xffff, action 0xe for B[0x46] frames (Ray 150), velocity = horizontal dir * cos(b) and
  vy = sin(b) scaled by H+0x20 * B[0x43] / 1000, b = H+0x22 deg; HP <= 0 forces knockdown and 0xff frames.
  H+0x16 < 0 forces knockdown; S+0x1db8 != 0 (armour state, not traced) zeroes H+0x16 (damage only).
- Horizontal direction = (H+8, H+0x10) normalised with y = 0 (zero vector -> (0,0,1)).
- Body bytes (stats per body, dumped): +0x40 u16 down threshold, +0x42 damage taken %, +0x43 knockback %, +0x44 stun
  duration %, +0x46 knockdown frames, +0x48..0x4b juggle damage % per hit type 1..4 (Ray 30/50/50/30), +0x4c/0x4d/0x4e
  attacker power % gun/bomb/pod (0x4c also read as the type-5 juggle byte -- probably just adjacent), +0x4f gun recovery %,
  +0xac hit radius x100.

## Not settled
- Which joint is the aim/homing point: S_t+0x10a4 (homing), S_t+0x10d4 (gun 0x18 aim), S_t+0x1080.. (pose solve target) are
  consecutive world-matrix translations of an array of 0x30-byte matrices; slot->joint mapping not done.
- Exact gun-joint aiming limits (pose solve 0x8005aee8) and what makes S+0x1d8 bit 2 toggle (no-opponent mode).
- HP source: S+0x70/0x74 = float(u16 at 0x801c5bdc + player*0x10) default 1000; whether real battles overwrite it per body
  was not traced.
- Meaning of E+0x20, E+0x26 (copied to H+0x1e/H+0x24, unread by the code I found), P+9/P+0xe, record 0xa4..0xd8 (tracer look).
- S+0x1db8 semantics; hit type values other than 1 (gun) and 4 (melee).
- Sign/handedness of fan angles (left/right arc): positive = rotation direction of FUN_8001b6ec about the computed axis; the two arc guns
  flip the sign between ground and air patterns.
- Effects/sounds indices (table 0x801f9074) beyond the slot count.

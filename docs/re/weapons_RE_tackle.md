# Custom Robo (GXCE01): tackle (action 12), weapons in the air, button 0x80 (tag change), KO flow

Everything below is read out of main.dol (Ghidra decompiles + capstone). Conventions as in
weapons_RE_damage.md: S = robot struct (0x8024e120 + 0x33520*n, `*(r13-0x5078)`), B = S+0x1b98 = copy of
the 0x134-byte body record, P = S+0x18a0 incoming-hit record. Velocity = hundredths of a world unit per
frame (pos += 0.01*vel, z negated). Timers are frames at 60 Hz.

New helper files (all under build/): tk_body.py (tackle fields + motion names, all 34 bodies; output saved in
tk_body_table.txt), tk_abs.py (find lis+disp absolute-address accesses), tk_callers.py (bl/b callers + data
pointers), annotated decompiles tk_ko_a.c (battle main loop 0x80010220, 8004dc78, 8004e8d4, 8008688c),
tk_ko2_a.c (8004ede4 + win checks 8004ff64/80050388), tk_tag_a.c (8004f6d4 tag swap), tk_mv_states_a.c
(actions 3-22), raw ghidra/tk_vec.c (vector helpers, see 1.3).

## 1. TACKLE (action 12, FUN_80046a68; hit test FUN_800450ec)

### 1.1 Entry (tail of action 0 = FUN_80049924, store at 0x8004aec4; decompile move_aa.c 616-740)
Priority inside action 0, all only when S+0xe2 < 1 (no landing lag / pending action timer):
  gun 0x10 (needs gun state S+0x20c4 == 0) > bomb 0x20 (S+0x28ece < 1) > pod 0x1000 (pressed AND ammo,
  S+0x2e468 < S+0x2e3c9) > tackle 0x40 > 0x80.
The tackle needs S+0x139 != 2 (grounded). A 0x40 press in the air does nothing and ALSO blocks the 0x80
check (0x80 is tested only in the `else` of "0x40 pressed"). On entry:
  S+0x528 = 12, motion index r13-0x4f14 = 0x23, S+0x148 = 0 (hit-done flag), S+0x186c = 0 (stage),
  S+0xe2 = S+0xe0 = B[0xda], heading S+0x110 = normalise_xz(opponent.pos - my.pos), z negated, y = 0,
  S+0x139 = 2 (airborne flag; the floor test in 8004b0a0 resets it to 1 on the same frame, no real flight),
  S+0x128 = 0 (no ground drive), S+0x13a = 0, S+0x1dbc = 1 (draw-side flag, read only at 0x80038b7c),
  sound/effect 0x30001 at S+0x20. Velocity is NOT changed on the entry frame (action 12 runs from the next frame).
Only action 0 enters 12 (action table 0x801f8eb8, entry 12 = 0x80046a68).

### 1.2 Body record fields (B = 0x801c6c44 + 0x134*body); types checked from the load instructions
| off | type | meaning | where |
|---|---|---|---|
| 0xd0 | u8 | armour mask: bit s = invulnerable during stage s. Bit 3 (8) never set in data; if set it disables the stage-1 hit test and the glow goes to S+0x1880 instead of 0x187c | 8004b0a0 (move_aa.c 1117-1127), 46a68, 450ec |
| 0xd1 | u8 | mask of stages for which the action RETURNS 0 instead of 1; the return value goes to S+0x1c8, read only by the draw code (0x80038fb8, 0x80039348, 0x80039fcc) and decides whether S+0x30 / the render matrices are refreshed (0x8003b9e4-0x8003b9f4). Cosmetic | 46a68 tail |
| 0xd2 | s16 | damage, added to P+0x14 (no attacker power scaling) | 450ec |
| 0xd4 | s16 | knock-down launch speed (P+0x20) | 450ec |
| 0xd6 | s16 | launch elevation in degrees (P+0x22) | 450ec |
| 0xd8 | s16 | homing turn rate: value/1000 = radians per frame, stages 0 and 1 only | 46a68 |
| 0xda,0xdc,0xde | s16 | duration of stage 0,1,2 in frames | 46a68 |
| 0xe0,0xe2,0xe4 | s16 | speed of stage 0,1,2: vel = 0.1 * value (hundredths/frame) | 46a68 |
| 0xe6,0xe8,0xea | s16 | pitch of stage 0,1,2 in degrees (+ up, -90 straight down) | 46a68 |
| 0xec | s16 | hit sphere radius /100 | 450ec |
| 0xee | s16 | hit sphere centre ahead of the robot /100 | 450ec |
| 0xf0 | s16 | hit sphere centre height offset /100 (negative = below the body centre) | 450ec |
| 0xf2.. | SJIS | NUL-terminated tackle name (default "サマーソルト２"; body 15 "皇帝頭突き") | dump |

Dump, bytes from B+0xd0 (full 34-body table in build/tk_body_table.txt):
  body 0 : 02 02 007e 00dc 005f 000a 000a 000e 0023 0050 00f0 0050 0000 0000 ffa6 0028 0014 0000 ..
  body 5 : 03 00 0075 00dc 000a 000a 000b 000c 0023 00a0 00a0 0050 0032 ffce ffa6 003c 003c ffb5 ..
  body 10: 02 00 0067 00a0 0046 0028 0005 001e 0005 00c8 0078 0050 0000 0028 ffc4 0023 001e ffe2 ..
  body 20: 02 00 0090 0082 0046 0064 0005 0078 001e 000a 0028 0028 0000 000a 0000 002d 000a 0000 ..
Decoded: d0 d1 | dmg d2, launch spd d4, launch ang d6, turn d8 | dur da dc de | spd e0 e2 e4 | pitch e6 e8 ea | rad ec fwd ee up f0
  0 : 2 2 | 126 220 95 10  | 10 14 35 | 80 240 80   | 0 0 -90    | 40 20 0    cr_atk_sf011_tackl_n (203)
  5 : 3 0 | 117 220 10 10  | 11 12 35 | 160 160 80  | 50 -50 -90 | 60 60 -75  cr_atk_ab023_spnkkt (208)
  10: 2 0 | 103 160 70 40  | 5 30 5   | 200 120 80  | 0 40 -60   | 35 30 -30  cr_atk_lr042_shoryu (213)
  20: 2 0 | 144 130 70 100 | 5 120 30 | 10 40 40    | 0 10 0     | 45 10 0    cr_atk_ls073_skyeratk (223, table bit 15 set)
Armour: Ray stage 1 only; bodies 5/16/24/26/27/28/31/32 (d0=3) stages 0+1; body 29 (d0=7) all stages; body 12 (d0=0) never.

### 1.3 Per frame (FUN_80046a68), exact
Vector helpers (build/ghidra/tk_vec.c): 8001c98c a-b, 8001ca28 dst += s*src, 8001ca5c scale, 8001ca84 normalise, 8001cb4c length,
8001e5e4 dot, 8001e60c cross, 8001c5ac = angle between two vectors (acos(dot/(|a||b|)), radians), 8001bf04(angle, v, axis) rotates v
about axis (Rodrigues), 8001f428 / 8001f4c0 = cos / sin of a 16-bit angle (65536 = 360 deg; pitch is converted as pitch*65536/360).
  S+0x550 += 1 (wraps to 9.9 at 10, effect timer); S+0x1d8 |= 0x40000.
  if S+0xe2 < 1:   (no motion/velocity/homing code runs on this frame; velocity keeps its last value)
      if stage < 2: stage++, S+0xe2 = S+0xe0 = B[0xda + 2*stage]
      else END: S+0x13a = 0, S+0x528 = 0, S+0x128 = 0, S+0x13c = 0 (impact 0, no landing lag), S+0x1dbc = 0
  else:
      motion frame S+0x548 = 20*(e0-e2)/e0 + 20*stage        (0..60 over the three stages)
      if stage < 2 (homing):
          t = (_DAT_8032a5a8, 0, _DAT_8032a5b0): xz of normalise(opp.out[1].t - my.out[11].t), z negated
              (computed at the top of 8004b0a0 every frame, move_aa.c 1060-1071)
          ang = angle(t, heading)
          if ang <= B[0xd8]/1000: heading = t           (not renormalised; the velocity code normalises)
          else if |t x heading| > 0 and dot(t, heading) > 0: rotate heading toward t by B[0xd8]/1000 rad about t x heading
          (a target more than 90 degrees off the heading is ignored: no turn at all)
      if S+0x13a == 0:  vel = normalise(heading); vel.xz *= cos(p); vel.y = sin(p); vel *= 0.1*B[0xe0+2*stage]
                        with p = B[0xe6+2*stage] degrees
      else:             vel = 0; S+0x13a -= 1               (hit-stop, 1.4)
      S+0xe2 -= 1
  stage armoured -> S+0x187c = 1.0 (S+0x1880 if B[0xd0] & 8): visual glow, decays x0.95/frame.
  facing globals 0x8032a5c0/cc/d8 = heading; the body facing S+0x158/0x170/0x17c then follows by 0.5*old + 0.5*new per frame
  (move_aa.c 1840-1860), so the hit sphere (1.4) uses a lagging facing.
Gravity: none (only action 0 adds gravity). Walls: no abort; the common tail of 8004b0a0 only removes the velocity component into
the wall (slide). Floor: the snap in 8004b0a0 removes the velocity going into the floor, so a -90 pitch stage presses the robot on
the floor (Ray stage 2 = 35 frame recovery at 0.08/frame into the floor, not a dive). On a flat arena the tackle stays at floor height.
Frame budget: each stage runs B[da+2k] velocity frames plus one transition frame (3 in all). Ray: 10+1+14+1+35+1 = 62 frames in
action 12 (59 with velocity). Speeds, Ray: 8 / 24 / 8 hundredths per frame = 4.8 / 14.4 / 4.8 units per second.
What ends it: only the stage timers. It is cut short (action 13/14, S+0x1dbc = 0) by an incoming hit in a stage that is not armoured;
in an armoured stage the hit record is discarded unprocessed (8004b0a0 armour test, move_aa.c 1117-1127). No landing lag, no wall
abort, no lockout afterwards, and landing a hit does not end it.

### 1.4 Hit test (FUN_800450ec; called per attacker from 8003b528 at 0x8003c07c / 0x8003c200, after all players' updates; skipped for an
attacker in action 18 with e2 < e0)
  only when S+0x528 == 12 && S+0x186c == 1 (stage 1) && (B[0xd0] & 8) == 0.
  victims: mask 0x8032a604[S+0x8d] (enemy team) or, if _DAT_80321d40 & 0x100, 0x8032a5f4[S+0x8c] (everybody else).
  per victim: skip if S+0x148 != 0 (this tackle already hit) or victim S+0xe6 != 0 (counter invulnerability); the victim's stage armour is
  NOT checked here (it is checked when the hit is processed).
  C = attacker hit centre S+0x1dc (0.5*(out[2]+out[11]), pulled 1/3 to the feet if B+0xae)
      + 0.01*B[0xee] * (S+0x170, S+0x174, -S+0x178) (smoothed 3-D facing) + (0, 0.01*B[0xf0], 0)
  hit if |C - victim.S+0x1dc| < 0.01*B[0xec] + 0.01*victim B+0xac     (sphere vs sphere, 3-D)
  on a hit: victim P: kind (P+2) = 4, P+1++, P+0x14 += B[0xd2], P+0x16 = -1 (always knock down), P+0x20 = B[0xd4], P+0x22 = B[0xd6],
      P+0x24 = 100, P+0x26 = 0, P+8..0x10 = normalise(heading). P+0x18..0x1e (stagger) are not written.
      VICTIM POSITION := ATTACKER POSITION (S_v+0x20..0x28 = S+0x20..0x28, disassembly 0x800454c4-0x8004553c) and then += 0.3*heading
      (constant r2-0x7a38 = 0.3), unless the victim is in action 12 with S+0x1db8 != 0. So the victim is pulled onto the attacker.
      attacker: S+0x148 = 1, S+0x13a = 3 (three frames of zero velocity, 1.3), hit effects.
  Result: at most ONE hit per tackle in total (one victim, once). Damage once processed: P14 * victim B42 / 90 HP (Ray 126 -> 140 HP vs B42 = 100);
  knock-down with launch speed B[0xd4]*B43/1000 at B[0xd6] degrees (Ray 95 degrees: vy = 0.996, horizontal = -0.087 i.e. slightly toward the
  attacker), gravity 100 %.

### 1.5 Motion
Motion index 0x23 in the per-body motion table (body dimension block 0x801c5c10 + 0x78*body, word 2 = table pointer, 0x801c51b8 for Ray, stride
0x4c per body; entries u16, bit 15 -> S+0x85b). Resolved IDs 203..234 = cr_atk_*: body 0 cr_atk_sf011_tackl_n (203), 5 cr_atk_ab023_spnkkt (208),
10 cr_atk_lr042_shoryu (213), 20 cr_atk_ls073_skyeratk (223). One 60-frame clip for all three stages. Body 31/32 reuse 234, body 33 = 230.

## 2. WEAPONS IN THE AIR
Action functions: 3 gun 0x8004949c, 4 bomb 0x800490c8, 5 pod 0x80048e7c, 6 air jump 0x80048b10, 7 air dash 0x80048708, 8 sky dash 0x80048050,
9 gun in sky dash 0x80047ac0, 10 bomb in sky dash 0x8004752c, 11 pod in sky dash 0x80046f58.
* Plain airborne action 0 (confirmed): with e2 < 1 a press of 0x10 (gun idle) enters 3, 0x20 (S+0x28ece < 1) enters 4, 0x1000 with ammo enters 5, in
  the air as on the ground. The pattern block is picked by S+0x139 == 2 (S+0x1fdc air, S+0x1f04 ground, stride 0xd8); bomb/pod get the airborne flag.
  Tackle is ground-only; 0x80 also works in the air if 0x40 is not pressed.
* Air jump (6) and air dash (7): no button handling at all. The only functions in the DOL that load S+0x242 / S+0x23e are 0x80049924 (action 0),
  0x80048050 (8), 0x800490c8 (4, held), 0x8004752c (10, held), 0x80045fec (14), 0x8003aca4 (target switch), 0x8003b850/0x8003b890 (tag).
  6 = e2 frames of squat (motion index 0xb), then vy = sqrt(...) and -> action 0 (weapons available again right after the impulse);
  7 = hang e2 frames (stat 9), then fly S+0x13a frames, -> action 0 at the end (S+0xe4 = S+0x33430 landing-lag stat). No weapon, tackle or cancel
  in 6 or 7.
* Sky dash (8): e2 counts the initial hang (stat 9 frames); presses are read only when e2 < 1 AND S+0x13a != 0 (the dash is running), so not during
  the hang. Each frame: heading turns toward the stick by at most 0.1*S+0x33438 degrees (FUN_8001bd54), vel = heading * 0.1*S+0x33436, tilted by
  S+0x1bf8 degrees about (up x heading), S+0x13a -= 1. Presses in this order:
    0x10 and S+0x20c4 == 0 -> action 9, e2 = e0 = byte S+0x1fdc (air-block wind-up; 0 = the gun state starts at once)
    0x20 and S+0x28ece < 1 -> action 10, e2 = e0 = 4 (0x801c6c10), S+0x138 = 0, FUN_8003f248(bomb, stick, 1)
    0x1000 with ammo       -> action 11, e2 = e0 = 10 (0x801c6c14)
  The steering/speed block is identical in 8, 9, 10, 11: the dash keeps flying and S+0x13a keeps counting down.
  9: r13-0x4f18 = 1 every frame (gun update asked to fire); when S+0x20c4 == 0 and e2 < 1 -> back to 8 (r13-0x4f18 = 0). At e2 == 1: gun state
     S+0x20c4 = 2 (air pattern), S+0x20ca = 0, S+0x20c6 = 0, S+0x20c8 = 0x100 (first volley fires at once). It stays 9 until the whole volley
     sequence is over (gun state back to 0).
  10: r13-0x4f18 = 2; e2 counts 4 frames; afterwards, when the bomb button is not held (S+0x23e & 0x20 == 0): FUN_8003f248(bomb, stick, 3) = air
      throw and -> 8. While held it stays in 10 (aiming, still dashing).
  11: pod launched on the first frame (e2 == e0): FUN_8003d7e0(pod, stick or last dir, airborne flag); e2 counts down 10 frames, then -> 8.
  The dash ends (S+0x13a == 0) in the middle of one of them: 9 -> action 3 (the normal gun action finishes the volley, sinking 0.04/frame),
      10 -> action 4 (normal bomb hold/throw), 11 -> action 0; S+0xe4 = S+0x33430, S+0x128 = 0.
  The weapon test comes before the velocity write, so the press frame already moves under the new action.
* Not accepted in any dash state: tackle (0x40) and 0x80.

## 3. BUTTON 0x80 / ACTIONS 17, 18, 19 = TAG CHANGE, tag mode only (_DAT_80321d50 == 2)
* The input mapper 0x800057b8 turns the 6th mapped button into 0x80 only if _DAT_80321d50 == 2 (0x800058dc-0x80005900); in every other mode the
  same button yields 0x100 = target switch (0x8003aba0, cycles S+0x238/S+0x228 among enemies; no use in a 1v1). So 0x80 does not exist in a 1v1
  (mode 0) or free-for-all (mode 1) battle: actions 17, 18, 19, the 0x200 input and the opponent-freeze logic are unreachable there.
* Battle-mode setup functions are in the table 0x801e4800, indexed by byte r13-0x5056 (dispatch at 0x800927f4): 0x8000f7a4 (mode 0, 2 robots, the
  VS 1v1), f230, e64c (mode 1), de50 (mode 2, r13-0x4ebc = 4 robots), e3b8, e1e4, ... The SJIS names after the table are "４人対戦",
  "４人対戦(1名ＣＯＭ)" ... "(4名ＣＯＭ)".
* Mode 2 = TAG battle: each side has two robots (index side*2 + {0,1}); DAT_8032a884[side*0x18] = the active one. FUN_8004f6d4 (every frame
  from 8004ede4 when mode == 2): when the active robot is in action 17 or 18 and its S+0xe2 == 1, the side's active index is toggled; the bench
  robot takes over position, facing, velocity and several state blocks (0x9c / 0xc8 / 0x740 bytes), the old robot becomes the idle dispatcher
  0x80005b88, sound 0x30085, HUD flag |= 1, swap counter (DAT_8032a888 + side*0x18) += 1. The benched robot recovers HP (S+0x74, float) toward the
  cap 0x801c6c3e = 150 at a rate set by 0x801c6c38/3a/3c (10, 60, 10) and the swap count (my reading of the formula; not exercised) and its
  down gauge S+0x144 keeps decaying.
* 0x8003b528 (0x8003b7ec-0x8003b898), mode 2 only: when a robot's S+0x242 has 0x80, the partner (index ^ 1) gets 0x200 OR-ed into its pressed mask;
  0x200 is the quick-recover input of action 14 and nothing else produces it, so quick recover is tag-only too.
Rules (action 0 tail, move_aa.c 690-703; 45b40, 45a48, 459a0): 0x80 pressed, S+0x146 == 0 (7 s lockout), none of 0x10/0x20/0x1000/0x40 handled first,
grounded or airborne (velocity zeroed, S+0x128 = 0): action 17, e2 = e0 = 10, S+0xe6 = 3, motion index 0x24 = sf_change (ID 64, same for every body),
glow S+0x1884 = 1. While e2 < e0 the opponent's whole 8004b0a0 returns at once and its weapon updates are paused (0x8003c030): frozen ~9 of the 10 frames.
At e2 == 1 the swap above runs; at the end S+0x146 = 420 and action 0. Action 18 (quick recover from 14, HP >= 1 and 0x200): 10 frames with e6 = 3,
S+0x146 = 420, action 19 (e2 = e0 = 30, e6 = 90, invulnerable), then action 0.
Unsettled: whether the CPU AI ever presses 0x80 (0x800dbad0 only ORs 1 and 0x40 as far as listed); what S+0x1d8 & 0x10000000 is (set per HUD record by
0x80089550 at HUD init, cleared by 0x8008688c which also rewrites S+0x8d from the setup table and moves the robot: it looks like a "not yet joined"
state of the 4-player entry rather than the round-over flag the damage notes guessed).

## 4. KO FLOW AND END OF BATTLE
Phase word _DAT_80321d98 (main switch in 0x80010220): 0 intro (150+ frames), 1 ready (240), 2 fighting (FUN_8004ede4 each frame), 3 round ended (KO),
4 time up, 5 result screen, 6/7 story continue countdown, 8 post-battle screen, 9 leave. Robots are updated every frame in every phase (0x8003b528 runs
after the switch), but input is zeroed unless the phase is 2 (0x80005bc8), and hits are still processed in phases 2 and 3. 8004ede4 (win check) runs
BEFORE 8003b528 in a frame, so a KO set in frame N is seen in N+1.
KO (8004b0a0 end, move_aa.c 1917-1962): HP (float S+0x74) < 1 -> HP = 0, S+0x1d8 |= 0x200, velocity zeroed, own gun bullets deleted (effect 0x3007d), HUD
flag 0x20. The lethal hit forces knock-down with e2 = e0 = 255 and the robot never gets up (action 14 holds the lying frame once e2 expires). Training
(_DAT_80321d40 & 0x100000) adds 1000 HP instead.
Win check (8004ede4: 0x80050388 mode 0, 0x8004ff64 mode 1, inline mode 2), armed once every robot has S+0x1d8 & 0x80 and none has 0x10000000 (sets
_DAT_80321d40 |= 0x20000000, mv_state.c 2041-2084). Mode 0 normal: alive = no S+0x1d8 & 0x200; exactly one alive -> _DAT_80321d56 = that robot, none
alive (double KO) -> 0xffff (draw). Variant with _DAT_80321d40 & 0x1000000 (story): robot 0 KO -> winner 1, else all others KO -> winner 0.
  -> phase 3, _DAT_80321db4 = 240. At db4 = 235 (5th frame) the KO banner (0x8007dd14(1, 0xd2)). At 0: fade (0x80096ebc(0, 30)); winner 0xffff -> phase 9;
  else FUN_8004dc78 assigns poses and phase 5 starts with db4 = 450 (330 when _DAT_80321d40 & 0x8800000).
Poses (8004dc78 -> 8004e8d4(slot, action, rand)): mode 0 normal: only the winner, action 20 with S+0x1ca = rand & 1, motion index 0x21 + S+0x1ca =
sf_win_<body>_01/_02 (body 0: ids 101/102; 5: 111/112; 10: 121/122; 20: 141/142). With the 0x1000000 variant the winner gets 20 and every other robot that is
alive (S+0x1d8 & 0x800200 == 0) gets action 21 (lose pose, motion index 0x20 = sf_lose_<body>_01: 169 / 174 / 179 / 189). A KO'd robot gets no pose, nor does an
alive loser after a mode-0 time-up. 8004e8d4: e2 = e0 = 0x8c + 0x1e + (1 - slot)*0x5a = 260 frames for slot 0 (170 for slot 1), velocity 0, snapped to the floor,
facing the arena centre. Actions 20/21 play the clip 0..138.5 over e0 frames (0.53 per frame) then hold the last frame until the battle ends (they never exit).
Timeline of a 1v1 KO (N = frame the lethal hit is processed): N+1 win detected, phase 3; N+241 poses start; 450 frames of result screen; then phase 8 (post-battle
screen) or 9. About 240 + 450 = 690 frames (11.5 s) from the KO to leaving the arena.
Time up: 8004ede4 counts db4 down from _DAT_80321d54 (frames; when the menu leaves it 0 the default is 7200 for 2 robots, 10800 for 3-4, 5400 otherwise, see
0x8000f8a0-0x8000f8dc; menu value 0xffff = unlimited, flag 0x80000000) while (d40 & 0xc0000000) == 0 and the armed flag is set. At 0 the highest rounded HP
(FUN_8015f8a0) wins, a tie gives winner 0xffff; db4 = 180, phase 4 (banner at 0xaf, 0x8007dd14(8, 0x78)); at 0 FUN_8004dc78, phase 5 (240 frames when the
result flags & 0x200 (draw), else 450).
Ring-out: none. Falling costs no HP. The only rule is in 8004b0a0 (move_aa.c 1800): if S+0x24 (y) < -4.0 the position is reset to (0, 1, 0). Terrain damage is
only the magma face (damage notes 2.5).

## 5. Not settled
* Gameplay meaning of the action return value / S+0x1c8 (draw code only as far as found).
* S+0x1d8 & 0x10000000 (see 3). The AI's use of 0x80. Mode 1 details. Story-mode rule bits in _DAT_80321d40 set by scripts.
* Whether the death-timer path in 8004b0a0 (damage notes 2.6) is a scripted kill; skip/shorten handling in phase 5.

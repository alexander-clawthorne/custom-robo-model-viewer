# Custom Robo (GXCE01) battle rules: hit reactions, damage, hit volumes

Everything below was read out of main.dol (Ghidra decompiles + capstone). S = robot struct
(0x8024e120 + 0x33520 * player; `*(r13-0x5078)`), B = S+0x1b98 = copy of the 0x134-byte body record
(`*(r13-0x4f24)`), P = S+0x18a0 = the "incoming hit" record (`*(r13-0x4f1c)`).
All timers are frames at 60 Hz. Velocities S+0x10/0x14/0x18 are hundredths of a world unit per
frame (pos += 0.01 * vel, z negated).

Helper scripts (all under build/, nothing else touched):
  build/dmg_scan.py  <disp> [st|ld]   every D-form load/store with that displacement (+ enclosing fn)
  build/dmg_scan2.py <lo> <hi>        every addi with an immediate in range
  build/dmg_callers.py <addr> [n]     bl callers with the preceding li's
  build/dmg_ghidra.ps1                corrected copy of tools/ghidra/decompile.ps1 (see "Tool bug")
  build/dmg_*.c, build/w_acts_a.c, build/move_aa.c   annotated decompiles; build/ghidra/dmg_a.c, dmg_b.c raw

## Tool bug worth knowing
tools/ghidra/decompile.ps1 (the uncommitted version in the working tree) ignores -Proj: PowerShell
variable names are case-insensitive, so `$proj = Join-Path $root 'build\ghidra'` overwrites the `$Proj`
parameter and every run opens build\ghidra\proj. My first batch (build/ghidra/dmg_a.c) therefore ran
in the shared project (it finished fine). All later runs used build/dmg_ghidra.ps1 -ProjDir
build\ghidra_dmg, which works. Rename the local variable (e.g. `$projDir`) to fix the original.

## 0. The big picture: one hit record, processed once per frame in FUN_8004b0a0

Per player per frame (FUN_8003b528 -> `**S` = FUN_8004b0a0):
  1. FUN_8004b0a0 returns at once if the OPPONENT (S+0x238) is in action 17 or 18 and its S+0xe2 < S+0xe0
     (the opponent is frozen for the rest of that action, see action 17).
  2. the current action function runs (table 0x801f8eb8, index S+0x528).
  3. timers tick: S+0x144 (down gauge) -1 if >0, S+0xe6 (invulnerability) -1, S+0xe7 -1, S+0xe8 -1,
     S+0x146 (lockout) -1 if >0.
  4. the hit record P is evaluated (this is where HP changes), then cleared (kind, damage, down, count).
  5. magma check, KO check, then the P[0] "invulnerable" export, position update.
After all players: gun hits (FUN_80041358), bomb/pod hits (FUN_800443c0) and tackle hits (FUN_800450ec)
WRITE into the victims' P for the next frame. So a hit lands the frame after the overlap.

Hit record P (victim S+0x18a0; written by 80041358 gun, 800443c0 bomb/pod, 800450ec tackle, 8004b0a0 magma):
  +0x00 u8   export: 1 if this robot was invulnerable on the last frame it was updated (read by the gun/bomb
             hit code to pick the "blocked" spark 0x30012 instead of the impact effect)
  +0x01 u8   hit counter (++ per hit written)
  +0x02 u16  kind: 1 gun, 2 bomb, 3 pod, 4 tackle (action 12), 5 magma. 0 = nothing pending.
             Cleared every frame at 8004b0a0 end (together with +0x14, +0x16, +0x01).
  +0x04 ptr  attacker S (last writer)
  +0x08..0x10 f32 x,y,z  knock direction (y unused); gun: bullet heading; bomb/pod: (victim centre - blast
             centre) flattened to xz; z is stored negated. Normalised.
  +0x14 u16  DAMAGE, accumulated over all hits this frame: += weapon_damage * (0.01 * attacker B[0x4c/4d/4e])
  +0x16 s16  DOWN value, accumulated: += weapon_down; -1 (0xffff) means "always knock down"
  +0x18 s16  stagger knock speed        +0x1a s16 stagger elevation angle (deg)
  +0x1c s16  stagger duration (frames)  +0x1e s16 stagger gravity (% of normal)
  +0x20 s16  knock-down launch speed    +0x22 s16 launch elevation (deg)
  +0x24 s16  knock-down gravity (%)     +0x26 u8  direction mode (0: use +0x08 vector; !=0: victim pos - attacker pos)
  Fields +0x18..+0x26 are overwritten by the last hit written, so the LAST projectile of a frame decides
  the reaction; damage and down value add up.

Where the fields come from (weapon records are the weapons agent's domain; offsets are DOL record offsets
and the numbers are plausible, i.e. checked against real data):
  gun  : per-phase record at gunrec+0xec (+0x28 per phase). Fields: +0x00 u16 phase frames, +0x10 f32 hit
         radius, +0x14 u8 trail segments, +0x16 damage, +0x18 down, +0x1a stagger spd, +0x1c stagger ang,
         +0x1e stagger frames, +0x20 stag grav, +0x22 launch spd, +0x24 launch ang, +0x26 kd grav.
         Striker Gun phase 0: radius 0.30, damage 28, down 280, stagger 10 @0deg for 20 frames, grav 100,
         launch 100 @35deg, grav 100.
  bomb : explosion record at bombrec+0xac, +0x50 per explosion (R = those): R+0x08/0x0c f32 radius start/end,
         R+0x10 f32 vertical scale, R+0x14 damage, +0x16 down, +0x18 .. +0x24 as P+0x18..0x24 (no shift).
         Bobcat Bomb: explosion 0 damage 68 down 600 stagger 120@70deg/30f, launch 120@70deg; explosion 1
         damage 53 down 420.
  pod  : R = podrec+0x114 (pod record is S+0x2e2e8): same layout as the bomb. Lineal Pod: damage 38 down 300,
         stagger 100@50deg/20f, launch 100@50deg. Seeker Pod: damage 26 down 300 launch 150@20deg.
  (The gun's fields are shifted by +2 relative to bomb/pod: gun damage is R+0x16, bomb/pod R+0x14.)
Attacker power: B[0x4c] gun, B[0x4d] bomb, B[0x4e] pod (percent; Ray 100, Coil 95, Bronzia 105, "Ray(24)" 150).
It scales the damage only, not the down value. B[0x4f] is the gun's reload scale (frame delay * B[0x4f]/100
in FUN_80042ab8), not a damage term.

## 1. Actions (table 0x801f8eb8; S+0x528). Motion = index into the per-body table at S+0x554 (= dimension block
word 2; Ray's is 0x801c51b8), names via motion.py. Ray's indices: 0x0c sf_landing, 0x1a sf_fall_back,
0x1b sf_fall_back_wall, 0x1c sf_fall_front, 0x1d sf_fall_front_wall, 0x1e sf_fall_magma, 0x1f sf_sf011_powerup,
0x20 sf_lose, 0x21/0x22 sf_win_01/02, 0x23 cr_atk_sf011_tackl_n, 0x24 sf_change.

Every store to S+0x528 in the DOL was enumerated (41 sites, build/dmg_scan.py 0x528 st). Values written:
0 (idle), 3,4,5 (gun/bomb/pod), 6,7,8 (air jump/dash/sky dash), 9,10,11, 12, 13, 14, 15, 16, 17, 18, 19 and the
generic setter 8004e8d4 (called only with 0x14/0x15). NOTHING writes 1, 2 or 22.

| act | fn | what it is | duration | velocity |
|---|---|---|---|---|
| 1  | 80049904 | latch-flags stub: copies S+0x1d8 and S+0x4cc to globals, returns. Never entered in battle code. | - | untouched |
| 2  | 800498fc | empty (`return 1`). Never entered. | - | untouched |
| 12 | 80046a68 | TACKLE (button 0x40, grounded only, set in 80049924 @8004aec4). Motion 0x23 (cr_atk_*_tackl_n), frame = 20*(e0-e2)/e0 + 20*stage. 3 stages (S+0x186c = 0,1,2), each B[0xda+2s] frames (Ray 10,14,35 = 59). Sets S+0x139=2, S+0x1dbc=1. Invulnerable in the stages whose bit is set in B[0xd0] (Ray 0x02 = stage 1, R-types 0x03). Stage 1 is the strike: sphere 0.01*B[0xee] ahead and 0.01*B[0xf0] above the body centre, radius 0.01*B[0xec] (+ target radius); hit once (S+0x148) -> target kind 4, damage += B[0xd2], down -1, launch B[0xd4] @ B[0xd6] deg (Ray 126 / 220 / 95), kd gravity 100; victim also shoved 0.3 along the heading; 3 frames of S+0x13a hitstop. Ends -> action 0. | each frame: heading S+0x110 homes on the opponent by <= B[0xd8]/1000 (stages 0,1), then vel = heading pitched by B[0xe6+2s] deg (Ray 0,0,-90) * 0.1*B[0xe0+2s] (Ray 80,240,80 -> 8,24,8 hundredths/frame). No gravity added, S+0x128=0 |
| 13 | 800468b8 | HIT STAGGER (flinch). Entered from the hit code. Motion: grounded 0x0c (sf_landing) frame 18.9*(e0-e2)/e0; airborne (S+0x139==2) 0x1a fall_back (S+0x14a=0) or 0x1c fall_front, frame 0. | e2 = P+0x1c * B[0x44] / 100 frames (Striker 20 frames at B44=100); re-hit restarts it; -> action 0 | impulse at entry (section 2); each frame vy -= 0.006 * P+0x1e (stagger gravity %); S+0x128=0 (no drive); horizontal velocity decays only through the normal ground-slide code |
| 14 | 80045fec | KNOCK-DOWN (flying, then lying). Input mask |=4 in the per-player table. Motion: airborne 0x1a/0x1c frame 0; grounded: frame = e0-e2 for the first 30 frames (falling over), then held, then 30+(30-e2) over the last 30 frames (getting up). | e2 = B[0x46] (Ray 150 frames = 2.5 s, counted from the hit, not from landing; Defender 170, Strato 192; R-Type2 45, R-Type3 0); dead: 255 and the get-up never happens. Re-hit while in 14/15 reloads it (juggle). | launch impulse (section 2); vy -= 0.006 * P+0x24 per frame (P+0x24 = kd gravity %, 100 = same as a normal fall); S+0x128=0. Landing handled by the common tail of 8004b0a0 |
|    |          | Quick recover: HP>=1, input S+0x242 & 0x200 and S+0x146==0 -> action 18, e2=e0=10, e6=3, vel 0, S+0x128=0. | | |
|    |          | Timer end (e2<1): HP>=1 (or training flag 0x100000) -> action 16, e2=e0=30, e6=60, vel=0, sound 0x3007c. HP<1: stays. | | |
| 15 | 80045ce8 | MAGMA BOUNCE ("sf_fall_magma", motion 0x1e). Entered only by a kind-5 hit. Pod/bomb-style air control: vel += 0.003*stat(S+0x33428) * stick (S+0x104), horizontal speed capped at 0.1*stat(S+0x33426), vy -= gravity step. Effect on S+0x1e04 follows the body. | e2 = 50 (tune 0x801c6c18). When e2<1: same exit as 14 (-> 16 with e6=60, or stays if HP<1). If the robot dies in 15 it is converted to action 14 with e2=2. | see 2.5 |
| 16 | 80045c08 | REBIRTH / get-up with power-up flash (motion 0x1f sf_sf011_powerup, frame 20*(e0-e2)/e0, per-player input flag |=0x10). | 30 frames, then -> action 0 | none is applied: vel was zeroed at entry, no gravity -> if entered in the air the robot hangs there for the 30 frames. S+0xe6 = 60 EVERY frame of the action (tune 0x801c6c0e), so invulnerable for 30 + 60 = 90 frames from the start of the rise |
| 17 | 80045b40 | button 0x80 move (motion 0x24 sf_change, frame 20*(e0-e2)/e0, S+0x1884 glow = 1.0). Needs S+0x146==0 and no tackle/gun/bomb/pod/pod pressed; set in 80049924 @8004aff4 with e2=e0=10, e6=3, vel 0. Ends -> action 0 and S+0x146 = 420 (7 s lockout, tune 0x801c6c40) also written by action 18. The OPPONENT's whole update (8004b0a0) returns early while this action has e2<e0, i.e. the opponent is frozen for ~9 of the 10 frames; this robot's own gun/bomb/pod updates (8003b528) are paused during the same frames. | 10 frames | zeroed at entry |
| 18 | 80045a48 | QUICK RECOVER from knock-down (14 -> 18). Same freeze of the opponent as 17. motion 0x24. | 10 frames, e6=3; at the end S+0x146=420 and -> 19 with e2=e0=30 (tune 0x801c6c34), e6 = 90 (tune 0x801c6c36) | zero |
| 19 | 800459a0 | recover recovery: motion 0x24, then -> action 0. Invulnerable for the e6=90 set at its start (90 frames from the start of 19). | 30 frames | zero |
| 20 | 80045868 | WIN POSE. Motion 0x21+S+0x1ca (sf_win_01/02, S+0x1ca random 0/1). Set by 8004dc78 for the winning team (via 8004e8d4(.,0x14,rand&1)); e2 starts 0x8c+0x1e+(1-slot)*0x5a, frame ramps 0..138.5 over the last e0 frames. | ~170/260 frames | zero (8004e8d4 also snaps the robot to the floor) |
| 21 | 80045724 | LOSE POSE (alive loser). Motion 0x20 sf_lose. Set by 8004dc78 (8004e8d4(.,0x15,0)). | same timing | zero |
| 22 | 800456b8 | idle pose counter (motion 0, frame += 1 each frame, copies the look-at vectors). Never entered. | - | zero |
Also relevant: 3/4/5 (ground gun/bomb/pod) and 9-11 (the same during a sky dash) are interrupted by any
processed hit (a hit sets 13/14 directly); 12 is interrupted too (S+0x1dbc cleared) unless the stage is armoured.

Invulnerability / "rebirth" summary
  Counter S+0xe6 (u8), ticked -1 per frame in 8004b0a0 after the action. Invulnerable while non-zero.
  There is NO invulnerability after an ordinary hit/stagger/knock-down (no store to e6 in the hit path).
  Writers: action 16 sets 60 every frame (-> 90 frames total from the start of the rise), 14->18 and 17 set 3,
  18->19 sets 90. Also invulnerable (no counter): action 12 in the stages flagged in B[0xd0], and action 7
  (air dash, S+0xe2==0) against kind-1 gun hits when B[0x5f]!=0 (bodies 12,13,14,27,33).
  The "flag" is P[0] (S+0x18a0), recomputed at the end of every 8004b0a0 as (invulnerable || action7 case).
  Nothing in S+0x1d8 marks invulnerability. S+0x1d8 bits actually seen: 0x2 no look-at, 0x200 KO,
  0x40000 tackle running (rebuilt every frame), 0x800000 death timer running, 0x100000 death timer done,
  0x10000000 round over (hits only trigger FUN_8008688c and the rest of 8004b0a0 returns), 0x8000000 sky-dash shot.

## 2. Damage, down gauge and launch (FUN_8004b0a0, annotated in build/move_aa.c lines 1085-1900)

Gate: processed iff P+2 != 0, battle phase word 0x80321d98 is 2 or 3, S+0x1d8 & 0x10000000 == 0 and the robot is
not invulnerable (above). Damage is applied only if the opponent S+0x238 exists and is alive (HP >= 1) or the
training flag _DAT_80321d40 & 0x100000 is set (so nothing happens once the other robot is dead).

2.1 HP. HP is the float S+0x74 (max S+0x70), initialised in 80005da4 from the u16 at entry+0xc of the 16-byte
per-player setup table at 0x801c5bd0 (default 1000; the VS menu code at 0x800f63ec stores 0x3e8). It is NOT the
body record's u16 at +0x40, even though tools/stats.py calls that "hp": B+0x40 is the DOWN-GAUGE CAPACITY
(read at 0x8004b8fc, and by the HUD at 0x8007b958 as S+0x144 / B+0x40). The story scripts set HP through the
battle-setup opcodes (not traced).
  standing / stagger case:  HP -= 1000 * ((P14 * B42) / 900) / 100          = P14 * B42 / 90
  already in action 14/15:  HP -= 1000 * (0.01 * P14 * B[0x47+kind] * B42 / 900) / 100
  (900 = s16 at 0x801c6c00; B42 = damage-taken percent; B[0x47+kind] = damage percent while down: kind 1 gun = B48,
   2 bomb = B49, 3 pod = B4a, 4 tackle = B4b, 5 magma = B4c.)
  FUN_8007ab14 adds the same amount to the per-player HUD accumulator (0x80386.. stride 0x34).
  Worked example: Striker Gun (damage 28) on Ray 01 (B42=100): 31.1 HP per hit, 8.4 if the target is down (30%).
  Bobcat first explosion 68 -> 75.6; Lineal Pod 38 -> 42.2. Magma touch: P14 = 100 (tune 0x801c6c1a) -> 111 HP.
  Body table (B42 dmg taken %, B43 knock %, B44 stagger %, B46 down frames, B48-B4b % while down, B40 gauge):
    Ray/Splendor/Lava 100/95/105, 100, 100, 150, 30/50/50/30, 1000 | Coil/Earth/Spica 110/105/115, 110,100 | 800
    Bronzia/MetalBear/Metalox 90/85/95, 110, 80, 150 | 1800 | Agilia/Peking/Peregrine 125/120/130, 120, 90, 150,
    50/60/60/50 | 200 | Defender/Berias/Breakr 120/115/125, 120, 100, 170, 40/55/55/40 | 600 |
    Strato/Pleasure/Hover 85/80/90, 110, 80, 192, 38/63/63/35 | 1200 | R Type 2: B43=B44=0, B46=45 | R Type 3:
    B42=20, B43=B44=B46=0, gauge 30000 | body 24 (second "Ray") B42=160, B4c-4e=150, B48-4b=80.
    (full dump: run the python in section 7)

2.2 Down gauge S+0x144 (s16). Decays by 1 EVERY frame while > 0 (8004b0a0). Each processed hit:
  if S+0x1db8 != 0: P+0x16 is zeroed first (that flag is in the 0x1db8-0x1dd0 effect block, no store found; it makes
  the robot unflinching: damage still applies). If P+0x16 < 0 or HP < 1 (and not training): gauge = 0xffff (force down).
  else gauge += P+0x16; gauge < B+0x40 -> STAGGER (action 13); gauge >= B+0x40 -> gauge = 0xffff -> KNOCK-DOWN.
  After the knock-down the gauge is reset to 0. A stagger does not reset it.
  Example: Ray 01 (1000) vs Striker Gun (280): the 4th shot inside ~ (decay 1 per frame) knocks down.

2.3 Stagger (action 13), unless S+0x1db8: e2 = e0 = (P+0x1c * B44) / 100 (integer division);
  dir = knock direction (xz unit, z negated) -> v = dir * cos(P+0x1a deg) horizontally, + sin(P+0x1a deg) vertically,
  scaled by (P+0x18 * B43) / 1000 (hundredths per frame). Velocity is SET (S+0x10 zeroed first), not added.
  Tackle (kind 4) uses 0.1 * P+0x18 without B43. S+0x139 = 2 (airborne flag; the next floor test resets it),
  S+0x14b=0, S+0x13a=0, S+0x140=0.

2.4 Knock-down (action 14), or 15 for kind 5: S+0x528 = 14 (15), e2 = e0 = B+0x46, forced to 0xff when HP <= 1 and
  not training (dead: never gets up); kind 5 uses 50. S+0x139 = 2. Velocity = dir with elevation P+0x22 deg:
  vx = dir.x*cos, vy = sin, vz = dir.z*cos, scaled by (P+0x20 * B43) / 1000. S+0x14a = (dot(previous heading, new vel) < 0)
  picks fall_back vs fall_front. If kind == 1 the victim's own in-flight gun bullets (S+0x1e20 array, 0x20 x 0x1360)
  are deleted. If the robot is ALREADY in 14 or 15 the same launch is applied again (juggle) with S+0x144 reset to 0 and
  e2 reloaded to B+0x46 (or 50 for kind 5).
  Direction mode: P+0x26 == 0 -> P+8 vector (gun: bullet heading; bomb/pod: away from the blast centre, flattened),
  else away from the attacker robot's position.
  Down-hit vertical gravity afterwards: vy -= 0.006 * P+0x24 per frame (0.3*2.0*0.01*P24).

2.5 Magma (kind 5): in 8004b0a0 after the position update, if the robot is grounded (S+0x139 != 2), not KO'd and
  S+0x1d8 & 0x10000000 == 0, and the floor face "kind" word (0x8032c298 = floor query result +0x10) >> 3 == 1
  (face kind 0x8; present in arenas 2,4,8,20,23,30,38,39 per tools/collision.py) it writes into its OWN hit record:
  kind 5, damage += 100 (tune 0x801c6c1a), down -1, launch speed 250 (0x801c6c16) @ 90 deg (straight up), gravity
  50 (0x801c6c18), direction = current heading. It is processed next frame like any hit (ignored while
  invulnerable). Touching magma therefore costs ~111 HP (P14 * B42 / 90), launches the robot straight up with
  speed (250 * B43) / 1000 = 25 hundredths per frame under gravity 0.3 hundredths per frame^2 (P+0x24 = 50), puts it
  in action 15 for 50 frames, then rebirth (90 invulnerable frames).
  Face kinds 0x18-0x1c are conveyors/wind (8005cf0c: S+0x334a0.. = (0,0,-2.2),(0,0,-2.2),(0,0,2.2),(0,0,6),(0,0,-6)).

2.6 0 HP (end of 8004b0a0): if HP < 1 and not training: HP = 0, S+0x1d8 |= 0x200 (KO), S+0x10/S+0x18 and the pending
  position correction zeroed, every live gun bullet of the dead robot is deleted (with effect 0x3007d) and the table
  flag 0x20 is raised for the HUD. (Training flag 0x100000 instead does HP += 1000.) The lethal hit itself always
  forces the knock-down (gauge 0xffff, e2 = 255). In action 14 the dead robot never gets up (timer expiry only acts
  if HP >= 1). A death timer (S+0x1d6, 0xf0 frames airborne branch / 0x3c grounded branch, S+0x1d8 0x800000 then
  0x100000, FUN_8006e690(S+0x860) = explosion) runs only if P+0x18,+0x1a,+0x1e,+0x20,+0x22,+0x24 are all zero
  (80046168-800461b4), which a normal weapon hit does not leave behind -- NOT settled what that path is for
  (scripted kills?). The winner is chosen outside this file: 8004dc78 gives the winning team action 20, the alive
  losers action 21 (_DAT_80321d56 = winning team, _DAT_80321d50 = mode).

## 3. Hit volumes and collision radii

Body record words (u16, B = S+0x1b98; per-body values dumped in section 7):
  B+0xa6 (S+0x1c3e)  collision radius base, hundredths  (35 for most, 48 heavy/tall, 40 Criminals)
  B+0xa8 (S+0x1c40)  % along the segment out[2] -> out[11] where collision sphere A sits (50 normal, 120 light bodies, 72, 45...)
  B+0xaa (S+0x1c42)  never read
  B+0xac (S+0x1c44)  PROJECTILE HIT RADIUS, hundredths (Ray 0.30, Bronzia 0.48, Agilia 0.18, Criminal 0.50, Strato 0.40)
  B+0xae (S+0x1c46)  flag: pull the hit centre 1/3 of the way toward the feet
out[k] = world matrix k of the solved pose, 3x4 floats at S+0x1074+0x30k (translation at +0x0c/+0x1c/+0x2c);
rig slot r = out[r+1], so out[2] = slot 1 and out[11] = slot 10 (names of those slots not determined).

3.1 Projectile hit centre (8003b528): C = 0.5*(out[2].t + out[11].t) -> S+0x1dc/0x1e0/0x1e4;
  if B+0xae != 0: C = pos + 2/3*(C - pos) (pos = S+0x20..0x28). Radius rB = 0.01*B+0xac. A sphere, not a capsule.
  Gun bullets (80041358): swept sphere along the bullet's trail. Bullet state +5 == 1 (flying) and == 2 (after its
  first impact, trail still live) are both tested. The path is the polyline of the last (R+0x14 + 1) frame positions
  (ring of 60 x 12 bytes at bullet+0x28); for each segment the closest point is found: if it falls inside the segment
  the test is dist(C, point) < rB + bulletR, if it is an endpoint the test is dist < rB; state 1 also tests the head
  position with rB + bulletR. bulletR = f32 R+0x10 of the current phase (Striker 0.30). Each bullet hits each target
  once (mask byte +0xb); on a hit the bullet goes to state 2 and its effect object is stopped. Target set = enemies
  unless the DAT_80321d40 option bits say otherwise. If the weapon array's flag at +0x26f00 is set, a 0.01-radius sweep
  (FUN_8005d08c) from the bullet to the target is made first and a wall in between cancels the hit.
  Bombs/pods (800443c0): a vertical CYLINDER around the blast centre: horizontal distance^2 < (rB + r)^2 and
  |C.y - blast.y| < r*R+0x10 + rB, with r(t) = ramp 0 -> R+8 over R[2] frames then R+8 -> R+0xc over R[4] frames
  (a bomb has R[0] explosions every R[1] frames; each one hits each victim once, inst+1 bit mask). Seeker pods have
  vertical scale 3.44 (tall column), Lineal 0.59, Bobcat 0.59.
  Tackle (800450ec): sphere at C_attacker + 0.01*B[0xee] * heading + 0.01*B[0xf0] up, radius 0.01*B[0xec] + victim
  0.01*B+0xac, stage 1 only.

3.2 Collision spheres (80062ff4 and the tail of 8003b528, rebuilt every frame):
  A: centre = lerp(out[2].t, out[11].t, B+0xa8/100) -> S+0x3343c.., radius S+0x33478 = 1.4 * 0.01 * B+0xa6
     (Ray, Splendor, Lava, Coil, Earth, Spica, Agilia family, Defender family, R types: 0.49; Bronzia family and Strato
     family and body 33: 0.672; Criminal family 0.56).
  B: centre = out[2].t -> S+0x33448.., radius S+0x3347c = 0.01 * B+0xa6 (0.35 / 0.48 / 0.40).
  Arena walls: FUN_80061be8 -> FUN_80061620 sweeps sphere A (radius S+0x33478, from S+0x3343c to last frame's S+0x33454)
  against every non-skipped collision block (block bounding sphere, then FUN_8005ed28 vs faces), writes the correction
  into S+0x3346c.. which FUN_80063654 adds to S+0x20 and all 20 joint matrices. So the REAL wall radius is
  0.014*B+0xa6 = 0.49 for Ray, not 0.45; sphere A centre sits between slots 1 and 10, not at a fixed height.
  Floor: separate downward query FUN_8006207c at (A.x, y of out[0].t, A.z).
  Robot vs robot (FUN_80063da4 then FUN_800633bc; runs before the wall sweep): flags bit0 = dist(myA, theirA) <=
  rA + rA', bit1 = dist(myA, theirB) <= rA + rB'. If bit0: A vs A, else if bit1: myA vs theirB, else B vs B (when no
  flag). Overlap depth = (r1 + r2) - dist; each centre is moved apart by HALF of it along the line joining the centres
  (3-D, including y); no mass or velocity involved. Two Rays: A-A contact at 0.98.

## 4. Things I could not settle
  * Names of rig slots 1 and 10 (the two joints that define the body centre and sphere A).
  * What S+0x1db8 is (unflinching flag; no store found, not the body record, probably set by a bulk copy or script).
  * What produces the "all P params zero" death-timer path (section 2.6) and what sets S+0x1d8 & 0x10000000.
  * HP source in story mode (VS is 1000 flat; story goes through SYS_BATTLE_SET / EV_SET_BTL_RULE, not decoded).
  * What the button mapped to 0x80 is called in the game; mechanically it is the 10-frame "sf_change" freeze move
    (opponent's update skipped, own weapons paused, 3 invulnerable frames, 7 s lockout). 0x200 as an input bit is
    also produced for the teammate of a robot whose 0x80 is pressed (8003b528, mode 2), so it may be a team assist.
  * Whether a kind-4 (tackle) stagger path (action 13 with S+0x1db8 set, 0.1*P+0x18 speed) is ever used.
  * Wall sweep argument order (start = current A, end = previous A) and the exact face-contact handling in 8005ed28;
    not needed for the radius but matters for sliding.
  * B[0x47] itself (the kind-0 slot of the "% while down" array) and why kind 5 reads B[0x4c] (aliasing with gun power).

## 5. Port-ready numbers
  HP loss per hit  = round-free float: dmg_total * B42 / 90 (dmg_total = sum of weapon_damage * power%/100 per frame)
  if already knocked down: * B[0x47+kind]/100.
  down gauge: += down value per hit, -1 per frame, stagger below B+0x40, knock-down at >= B+0x40, reset to 0 on
  knock-down; forced when the hit's down value < 0 or HP reaches 0.
  stagger: frames = P+0x1c * B44 / 100; v = (P+0x18 * B43 / 1000) * (cos a * dir.xz, sin a); gravity 0.006 * P+0x1e per frame.
  knock-down: frames = B46 (255 if dead); v = (P+0x20 * B43 / 1000) * (cos b * dir.xz, sin b); gravity 0.006 * P+0x24.
  after it: 30-frame rise (no motion applied) with 60 + 30 = 90 invulnerable frames; quick recover (needs input, 420 frame
  lockout) 10 + 30 frames with e6 = 3 then 90.
  radii: wall/robot sphere A = 0.014*a6 at lerp(slot1, slot10, a8/100); sphere B = 0.01*a6 at slot 1; projectile hit
  sphere = 0.01*ac at the slot1/slot10 midpoint (pulled 1/3 toward the feet when ae = 1).

## 6. Constants (0x801c6c00 block, s16 unless noted)
  +0x00 900 damage divisor | +0x04 f32 2.0 gravity base | +0x08 f32 6.0 | +0x0c 40 | +0x0e 60 rebirth i-frames
  +0x10 4 bomb action frames | +0x12 175 | +0x14 10 pod action frames | +0x16 250 magma launch speed | +0x18 50 magma
  e2 and gravity | +0x1a 100 magma damage | +0x1c.. floor table (100,100,100 | 50,70,85 | 70,70,90 | 35,10,100 ...)
  +0x30 10 | +0x32 100 | +0x34 30 recover frames | +0x36 90 recover i-frames | +0x38..0x3e 10,60,10,150 (start placement)
  +0x40 420 lockout after button-0x80 / quick-recover.

## 7. Reproduce the body table
  PYTHONUTF8=1 python - <<'E'
  import sys,struct; sys.path.insert(0,'tools')
  from dol import Dol; d=Dol()
  for i in range(34):
      r=d.read(0x801c6c44+0x134*i,0x134); u=lambda o: struct.unpack('>H',r[o:o+2])[0]
      print(i,u(0x40),list(r[0x42:0x50]),r[0x5f],u(0xa6),u(0xa8),u(0xac),u(0xae))
  E

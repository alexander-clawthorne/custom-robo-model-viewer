# Custom Robo (GXCE01) bombs and pods, reverse-engineered from main.dol

Research notes, not shipped. Sources: Ghidra decompiles in build/ghidra (annotated copies bp_a_*.c),
`build/bp_dump.py` (record dumper, run `PYTHONUTF8=1 python build/bp_dump.py bombs|pods [idx..]`),
`build/bp_scan.py` (find loads/stores with a given displacement), `build/bp_decompile.ps1`
(private-project wrapper, see "Tooling note" at the end).

Conventions. S = robot struct (player n at 0x8024e120 + n*0x33520; S+0x20 = position, S+0x238 = opponent S).
L = bomb launcher = S+0x28d40. P = pod launcher = S+0x2e2e0. The part record is copied to L+8 / P+8, so
**L/P offset = record offset + 8**. World units; velocities in the robot code are hundredths per frame, the bomb and
pod code below uses its own factors (stated each time). 60 Hz. z of the stick/velocity vectors is negated relative to
world z; the launch code un-negates it.

Vector helpers (all verified by disassembly): 8001c98c(d,a,b) d=a-b; 8001c9c0(a,b) a-=b; 8001c9f4(a,b) a+=b;
8001ca28(f,a,b) a+=f*b; 8001ca5c(f,a) a*=f; 8001ca84(a) normalise; 8001cb4c(a) length; 8001ccc4(x,y,z,d) set;
8001cef4(m,d) d=translation column of 3x4 matrix; 8001cf10(m,d) d=X column; 8001e60c(d,a,b) d=a x b; 8001e5e4 dot;
8001c5ac angle (rad) between two vectors; 801690c8 atan2; 8001f4c0(i) sin of a 16-bit angle (0x10000 = 360 deg).
8001bd54(theta,v): rotation about +Y, x'=c x+s z, z'=-s x+c z (right handed). 8001bf04(theta,v,axis): Rodrigues, right
handed about `axis`. Sweep: FUN_80061620(r,out,from,to,mode) (wrappers 80061a18 / 80061b38 / 80061b90 differ in mode and in
clearing the result struct at 0x8032c234): sphere of radius r from `from` to `to`, `out` = contact position, normal at
0x8032c264, surface flags at 0x8032c270/274, block index at 0x8032c284. **Both weapons call it from the NEW position back to
the OLD position** (bomb: 80061b38(0.0001, out, newpos, oldpos); pod: 80061b90(0.3, out, newpos, oldpos)).

---------------------------------------------------------------------------------------------------------------

## 1. Records

### 1.1 Explosion descriptor E (shared by bombs and pods)

Bomb: E = record+0xac (ground, desc 0) or record+0xfc (air, desc 1). Pod: E = record+0x114 (P+0x11c, S+0x2e3fc).
Read by FUN_80044ea4 (init), FUN_80044a90 (step, 0x80044a90) and FUN_800443c0 (hit test, 0x800443c0).

| E+  | type | meaning | proof |
|-----|------|---------|-------|
| 0x00 | u8  | N, number of bursts (max 4: burst slots are 4 x 0x26c) | 44a90 loop `iVar8<4`; 44ea4 `iVar2<*param_2` |
| 0x01 | u8  | gap, frames between burst starts | 44a90: spawn when t%gap==0 and t<=(N-1)*gap |
| 0x02 | u16 | grow, frames for the radius to reach r1 | 44a90 radius formula |
| 0x04 | u16 | tail, frames to go from r1 to r2; burst dies at t > grow+tail | 44a90 |
| 0x08 | f32 | r1, radius at t=grow | 44a90 / 443c0 |
| 0x0c | f32 | r2, radius at t=grow+tail | |
| 0x10 | f32 | vertical factor v (hit half-height = r*v + R) | 443c0 |
| 0x14 | u16 | damage base | 443c0: H+0x14 += E14 * power (see 5) |
| 0x16 | s16 | down value | 443c0: H+0x16 += E16 |
| 0x18 | u16 | reaction A (flinch speed) | consumer 0x8004b0a0 (see 5) |
| 0x1a | u16 | reaction B (flinch angle, degrees) | |
| 0x1c | u16 | reaction C (flinch stun, frames at body+0x44 = 100) | |
| 0x1e | u16 | reaction D (stored, not seen read) | |
| 0x20 | u16 | reaction E (knock-away speed) | |
| 0x22 | u16 | reaction F (knock-away angle, degrees) | |
| 0x24 | u16 | reaction G (stored, not seen read) | |
| 0x26 | u16 | zero everywhere (pad); the bomb per-shot table starts at E+0x28 | |

Radius of a burst at its own age t (burst+2): `t <= grow: r = t*r1/grow; else r = (t-grow)/tail*(r2-r1)+r1`
(44a90 and again in 443c0). Burst is active while t <= grow+tail. Whole explosion X ends when
X.t > grow + (N-1)*gap + tail.

### 1.2 Bomb record (0x17c bytes, table 0x801d4860, 52 entries) -- record-relative offsets

| off | type | meaning | proof |
|-----|------|---------|-------|
| 0x00 | char[0x20] | Japanese name | |
| 0x20 | char[0x20] | US name (some entries still Japanese) | |
| 0x40 | char[0x20] | explosion-type label, e.g. "Explosion Type A" in Shift-JIS. L+0x5583 = (strcmp(label, "Explosion Type R") == 0), see 5 | FUN_80041140 |
| 0x60 | char[0x20] | projectile model/effect name (e.g. "missile1") | not read by logic |
| 0x80 | char[0x20] | explosion effect name | not read by logic |
| 0xa0 | u8 | always 0 | |
| 0xa1 | u8 | n, bombs per throw (1; Gemini 2; Delta 3) | FUN_8003f248 loop `iVar8 < L+0xa9` |
| 0xa2 | u16 | proximity trigger radius, 0.01 units (90 = 0.9) | FUN_8003fe98 `0.01*u16(L+0xaa)` |
| 0xa4 | desc0 | ground descriptor | |
| 0xf4 | desc1 | air descriptor (used when S+0x139 == 2, i.e. airborne) | FUN_8003f248 `param_3==1`, 0x50 stride |
| 0x144.. | | ff x16, then 00 00 00 3c, 00 00 00 01, ..., f32 1.0 at 0x178; not read by the bomb code | unsettled |

Descriptor (desc0 at +0xa4, desc1 at +0xf4; offsets relative to the descriptor):

| +   | type | meaning | proof |
|-----|------|---------|-------|
| 0x00 | u8 | aim%: initial cursor = own_pos*(1-aim/100) + opp_pos*(aim/100) (can be >100) | FUN_8003f248 begin branch |
| 0x01 | u8 | reload frames: L+0x18e set to this at throw | FUN_8003f248 `L+0x18e = *(byte*)(desc+1)` |
| 0x02 | u8 | fuse frames after landing (state 2) | FUN_8003fe98 `piVar10+0x4e < *(byte*)(*piVar10+2)` |
| 0x03 | u8 | pad | |
| 0x04 | u16 | speed, 0.001 units/frame | FUN_8003f248 flight time, FUN_8003fe98 rolling/explosion drift |
| 0x06 | u16 | 0 | |
| 0x08 | E | explosion descriptor (1.1), 0x28 bytes | |
| 0x30 + 8k | 4 x s16 | per shot k (0..3): lateral offset, forward offset (0.01 units, relative to the cursor), arc height A, side swing B (0.003*dist per unit) | FUN_8003f248 / FUN_8003fe98 |

Per-bomb flags set by FUN_80041140 (0x80041140) from the bomb INDEX: index 14 (Tomahawk) sets L+0x5580 (proximity is also
tested 1 unit below the bomb); index 28 (Double Mine) sets L+0x5581 (after the arc ends the bomb stops instead of
rolling on); index 27 (Smash) sets L+0x5582 (explosion is placed at the owner, see 2.7). L+0x5583 = (label == "Type R"; FUN_80165720 is strcmp, 0 when equal).

### 1.3 Pod record (0x174 bytes, table 0x801d9590, 57 entries) -- record-relative offsets (P offset = rec + 8)

| off | type | meaning | proof |
|-----|------|---------|-------|
| 0x00 | char[0x20] x2 | JP name, US name | |
| 0x40 | char[0x20] | JP short name | |
| 0x60 | char[0x20] | explosion-type label; P+0x5000 = (label == "Type R", strcmp == 0) | FUN_8003f048 |
| 0x80,0xa0,0xc0 | char[0x20] | model / effect names | not read by logic |
| 0xe1 | u8  | max pods out at once (P+0xe9) | movement 0x80049924, FUN_8003d7e0 |
| 0xe2 | u8  | pods per press | FUN_8003d7e0 loop |
| 0xe4 | s16 | spread angle in degrees (N=2: +s/2,-s/2; N=3: +s/2,0,-s/2) applied to the launch velocity | FUN_8003d7e0 |
| 0xe6 | u16 | detonation range, horizontal, 0.01 units | FUN_8003dd38 (P+0xee) |
| 0xe8 | u16 | detonation range, vertical, 0.01 units | (P+0xf0) |
| 0xea | u16 | lifetime frames, counted only after the launch phase | Q+0x46 |
| 0xec,0xee | s16 x2 | initial heading yaw (ground press): N=1 uses 0xec, N>=2 lerps from 0xee (i=0) to 0xec (i=N-1) | FUN_8003d7e0 (P+0xf4/f6) |
| 0xf0,0xf2 | s16 x2 | same for an air press | (P+0xf8/fa) |
| 0xf4 | u8  | lock-on enabled | FUN_8003dd38 (P+0xfc) |
| 0xf5,0xf6 | u8 x2 | steering gain: turn 0.001*g of the remaining angle per frame; [0] unlocked, [1] locked | (P+0xfd/0xfe) |
| 0xf8,0xfa | u16 x2 | homing speed, 0.001 units/frame; [0] unlocked, [1] locked | (P+0x100/0x102) |
| 0xfc,0xfe | u16 x2 | hover height above the floor, 0.01 units (+0.6 fixed); [0] unlocked, [1] locked | (P+0x104/0x106) |
| 0x100 | u16 | lock-on cone half-angle, degrees (>180 = always) | (P+0x108) |
| 0x102 | u16 | lock-on range, 0.01 units | (P+0x10a) |
| 0x104 | s16 | pause frames when the lock is first acquired (pod freezes, velocity 0) | (P+0x10c) -> Q+0x42 |
| 0x106,0x108 | s16 x2 | launch elevation, degrees, [ground, air]; + = pitch up | FUN_8003d7e0 (P+0x10e+2*air) |
| 0x10a,0x10c | s16 x2 | launch speed, 0.001 units/frame, [ground, air] | (P+0x112+2*air) |
| 0x10e,0x110 | s16 x2 | launch-phase frames, [ground, air] | (P+0x116+2*air) -> Q+0x43 |
| 0x112 | u8 | wall response: 0 = slide along the wall, !=0 = reflect | FUN_8003dd38 (P+0x11a) |
| 0x113 | u8 | scale percent (Q+0x4c = this/100; giant pod 250) | FUN_8003f048 (P+0x11b); appears visual |
| 0x114 | E | explosion descriptor (1.1) | |
| 0x144..0x173 | | ff x16, 00 00 00 3c, 00 00 00 01, ..., f32 1.0; one oddity: pod 25 has 7fff 7fff at 0x144; not read by pod code | unsettled |

Pod flags set at init by FUN_8003f048 (0x8003f048): pod index 30 (Umbrella) sets P+0x5001 (no detonation test during the
launch phase). P+0x5002 = pod index is in the u16 list at 0x801f9980 {0,3,6,9,12,18,23,25,26,27}: vertical detonation test
becomes `dy < H and dy > -0.65` instead of `|dy| < H` (dy = target.y - pod.y).


### 1.4 Dumps (generated by build/bp_dump.py; "jp" = Shift-JIS name omitted)

Bombs: n = per throw, prox = trigger radius, per descriptor (G ground, A air): aim, reload, fuse, speed, shots = (lat, fwd, A, B), E = (N, gap, grow, tail, r1, r2, v, dmg, down, react[7]).

```
bomb 0 'Bobcat Bomb' type A n=1 prox=90
   G: aim=90 reload=90 fuse=0 speed=160 shots=[[0, 0, 100, 0]]
      E: N=1 gap=1 grow=16 tail=20 r1=1.0 r2=1.15 v=0.59 dmg=68 down=600 react=[120, 70, 30, 100, 120, 70, 100]
   A: aim=85 reload=90 fuse=0 speed=180 shots=-
      E: N=1 gap=1 grow=14 tail=18 r1=0.95 r2=1.12 v=0.59 dmg=53 down=420 react=[120, 70, 30, 100, 120, 70, 100]
bomb 1 '<jp>' type I n=1 prox=100
   G: aim=88 reload=90 fuse=0 speed=160 shots=[[0, 0, 100, 0]]
      E: N=1 gap=1 grow=16 tail=20 r1=0.7 r2=1.0 v=3.23 dmg=69 down=600 react=[135, 20, 20, 100, 135, 20, 100]
   A: aim=83 reload=90 fuse=0 speed=180 shots=-
      E: N=1 gap=1 grow=14 tail=18 r1=0.65 r2=0.8 v=3.79 dmg=53 down=420 react=[135, 20, 20, 20, 135, 20, 100]
bomb 2 '<jp>' type E n=1 prox=100
   G: aim=85 reload=90 fuse=0 speed=160 shots=[[0, 0, 100, 0]]
      E: N=1 gap=1 grow=14 tail=18 r1=0.8 r2=1.12 v=0.59 dmg=59 down=600 react=[0, 0, 70, 100, 0, 0, 100]
   A: aim=80 reload=90 fuse=0 speed=180 shots=-
      E: N=1 gap=1 grow=14 tail=15 r1=0.72 r2=0.88 v=0.59 dmg=47 down=420 react=[0, 0, 70, 100, 0, 70, 100]
bomb 3 '<jp>' type K n=1 prox=100
   G: aim=70 reload=90 fuse=0 speed=120 shots=[[0, 0, 100, 0]]
      E: N=3 gap=8 grow=12 tail=18 r1=0.5 r2=0.7 v=2.5 dmg=24 down=300 react=[120, 20, 30, 100, 120, 20, 100]
   A: aim=70 reload=90 fuse=0 speed=120 shots=[[0, 0, 100, 0]]
      E: N=3 gap=8 grow=10 tail=18 r1=0.5 r2=0.7 v=2.5 dmg=17 down=210 react=[120, 20, 30, 100, 120, 20, 100]
bomb 7 'Scout Bomb #H' type C n=1 prox=100
   G: aim=110 reload=90 fuse=0 speed=160 shots=[[-84, 0, 60, -55]]
      E: N=1 gap=1 grow=14 tail=18 r1=1.0 r2=1.4 v=0.59 dmg=60 down=600 react=[80, 20, 30, 20, 80, 20, 20]
   A: aim=110 reload=90 fuse=0 speed=180 shots=[[70, 0, 60, 55]]
      E: N=1 gap=1 grow=13 tail=16 r1=0.9 r2=1.1 v=0.59 dmg=42 down=420 react=[80, 20, 30, 20, 80, 20, 20]
bomb 9 '<jp>' type K n=1 prox=100
   G: aim=85 reload=90 fuse=0 speed=100 shots=[[-200, -150, 110, -150]]
      E: N=3 gap=12 grow=12 tail=18 r1=0.5 r2=0.7 v=3.3 dmg=34 down=600 react=[120, 20, 30, 100, 120, 20, 100]
   A: aim=80 reload=90 fuse=0 speed=110 shots=[[200, -150, 110, 150]]
      E: N=3 gap=12 grow=12 tail=18 r1=0.5 r2=0.7 v=3.3 dmg=27 down=420 react=[120, 20, 30, 100, 120, 20, 100]
bomb 11 'Rogue Bomb #D' type F n=1 prox=90
   G: aim=70 reload=80 fuse=120 speed=170 shots=[[0, 0, 100, 0]]
      E: N=1 gap=1 grow=15 tail=70 r1=1.3 r2=1.3 v=0.55 dmg=75 down=600 react=[150, 50, 30, 100, 150, 50, 100]
   A: aim=80 reload=90 fuse=10 speed=160 shots=[[0, 0, 30, 0]]
      E: N=1 gap=1 grow=12 tail=60 r1=1.1 r2=1.1 v=0.55 dmg=53 down=420 react=[150, 50, 30, 100, 150, 50, 100]
bomb 14 'Tomahawk Bomb #B' type B n=1 prox=120
   G: aim=120 reload=90 fuse=0 speed=120 shots=[[0, 0, 260, 0]]
      E: N=1 gap=1 grow=12 tail=60 r1=0.99 r2=1.43 v=0.55 dmg=63 down=600 react=[140, 140, 20, 100, 140, 140, 100]
   A: aim=120 reload=90 fuse=0 speed=120 shots=[[0, 0, 100, 0]]
      E: N=1 gap=1 grow=10 tail=60 r1=0.99 r2=1.21 v=0.55 dmg=54 down=420 react=[140, 140, 20, 100, 140, 140, 100]
bomb 16 '<jp>' type B n=2 prox=100
   G: aim=105 reload=60 fuse=0 speed=160 shots=[[-250, 0, 60, -30], [250, 0, 60, 30]]
      E: N=1 gap=1 grow=16 tail=40 r1=0.61 r2=0.74 v=0.8 dmg=40 down=300 react=[135, 20, 20, 100, 135, 20, 100]
   A: aim=100 reload=60 fuse=0 speed=180 shots=[[0, -240, 60, 0], [0, 200, 100, 0]]
      E: N=1 gap=1 grow=16 tail=40 r1=0.61 r2=0.74 v=0.8 dmg=40 down=210 react=[135, 20, 20, 100, 135, 20, 100]
bomb 25 '<jp>' type J n=3 prox=40
   G: aim=50 reload=90 fuse=0 speed=240 shots=[[200, 200, 100, 0], [-200, 200, 100, 0], [0, 0, 220, 0]]
      E: N=1 gap=1 grow=7 tail=18 r1=0.25 r2=0.6 v=5.88 dmg=30 down=240 react=[150, 20, 30, 100, 150, 20, 100]
   A: aim=50 reload=90 fuse=0 speed=196 shots=[[200, 200, 100, 0], [-200, 200, 100, 0], [0, 0, 120, 0]]
      E: N=1 gap=1 grow=7 tail=18 r1=0.25 r2=0.6 v=5.88 dmg=21 down=240 react=[150, 20, 30, 100, 150, 20, 100]
bomb 27 '<jp>' type A n=1 prox=4500
   G: aim=20 reload=75 fuse=0 speed=1000 shots=[[0, 240, 300, 0]]
      E: N=1 gap=1 grow=1 tail=35 r1=1.21 r2=1.69 v=0.5 dmg=75 down=600 react=[180, 40, 30, 100, 180, 40, 100]
   A: aim=20 reload=90 fuse=0 speed=1000 shots=[[0, 240, 200, 0]]
      E: N=1 gap=1 grow=14 tail=18 r1=1.1 r2=1.54 v=0.5 dmg=53 down=420 react=[180, 40, 30, 100, 180, 40, 100]
bomb 28 '<jp>' type A n=2 prox=80
   G: aim=65 reload=80 fuse=240 speed=160 shots=[[-150, 0, 0, 0], [150, 0, 0, 0]]
      E: N=1 gap=1 grow=16 tail=20 r1=0.83 r2=1.16 v=0.59 dmg=30 down=100 react=[120, 70, 30, 100, 120, 70, 100]
   A: aim=65 reload=80 fuse=120 speed=180 shots=[[-300, 0, 0, 0], [300, 0, 0, 0]]
      E: N=1 gap=1 grow=14 tail=18 r1=0.83 r2=1.16 v=0.59 dmg=30 down=100 react=[120, 70, 30, 100, 120, 70, 100]
bomb 30 '<jp>' type N n=1 prox=100
   G: aim=100 reload=240 fuse=60 speed=25 shots=[[0, 0, 250, 0]]
      E: N=1 gap=1 grow=33 tail=42 r1=2.9 r2=4.05 v=0.59 dmg=18 down=2000 react=[140, 70, 30, 100, 140, 50, 100]
   A: aim=100 reload=240 fuse=60 speed=20 shots=[[0, 0, 150, 0]]
      E: N=1 gap=1 grow=33 tail=42 r1=2.9 r2=4.05 v=0.59 dmg=18 down=2000 react=[140, 70, 30, 100, 140, 50, 100]
```

Pods: max/per = max out / per press, spr = spread, hit = (R, H), life, fan = (ground, air) yaw pairs, gain/speed/hover = [unlocked, locked],
cone/range/pause, elev/lspeed/lframes = [ground, air], bounce, scale, E as above.

```
pod 0 'Lineal Pod' type A max=2 per=1 spr=0 hit=(100,100) life=90 fan=(0, 0),(0, 0) lock=1
   gain=(10, 30) speed=(90, 135) hover=(10, 10) cone=90 range=250 pause=0 elev=(0, 5) lspeed=(100, 50) lframes=(10, 50) bounce=0 scale=100
   E: N=1 gap=1 grow=9 tail=32 r1=0.8 r2=0.9 v=0.59 dmg=38 down=300 react=[100, 50, 20, 100, 100, 50, 100]
pod 1 'Seeker Pod #F' type I max=2 per=1 spr=0 hit=(70,300) life=240 fan=(0, 0),(0, 0) lock=1
   gain=(30, 30) speed=(60, 18) hover=(0, 0) cone=90 range=300 pause=5 elev=(0, 5) lspeed=(100, 100) lframes=(10, 30) bounce=0 scale=100
   E: N=1 gap=1 grow=5 tail=19 r1=0.75 r2=0.85 v=3.44 dmg=26 down=300 react=[150, 20, 20, 100, 150, 20, 100]
pod 2 '<jp>' type J max=2 per=1 spr=0 hit=(70,300) life=240 fan=(0, 0),(0, 0) lock=1
   gain=(30, 30) speed=(60, 18) hover=(0, 0) cone=90 range=300 pause=5 elev=(0, 5) lspeed=(100, 100) lframes=(10, 30) bounce=0 scale=100
   E: N=1 gap=1 grow=5 tail=19 r1=0.75 r2=0.85 v=3.44 dmg=26 down=300 react=[150, 90, 20, 100, 150, 90, 100]
pod 3 '<jp>' type F max=2 per=1 spr=0 hit=(70,300) life=45 fan=(0, 0),(0, 0) lock=1
   gain=(30, 10) speed=(250, 140) hover=(10, 10) cone=110 range=300 pause=0 elev=(0, 0) lspeed=(0, 0) lframes=(0, 45) bounce=1 scale=100
   E: N=1 gap=1 grow=15 tail=70 r1=0.8 r2=0.8 v=0.59 dmg=35 down=300 react=[80, 50, 20, 100, 80, 50, 100]
pod 7 '<jp>' type A max=2 per=1 spr=0 hit=(100,60) life=120 fan=(0, 0),(0, 0) lock=1
   gain=(5, 30) speed=(100, 150) hover=(10, 10) cone=120 range=300 pause=0 elev=(60, 30) lspeed=(200, 200) lframes=(10, 0) bounce=1 scale=100
   E: N=1 gap=1 grow=9 tail=32 r1=0.8 r2=1.1 v=0.59 dmg=35 down=300 react=[100, 50, 20, 100, 100, 50, 100]
pod 9 'Spider Pod' type A max=3 per=1 spr=0 hit=(100,100) life=380 fan=(0, 0),(0, 0) lock=1
   gain=(30, 10) speed=(5, 160) hover=(10, 10) cone=90 range=400 pause=5 elev=(0, 0) lspeed=(100, 10) lframes=(10, 50) bounce=0 scale=100
   E: N=1 gap=1 grow=9 tail=32 r1=1.0 r2=1.3 v=0.59 dmg=27 down=300 react=[150, 50, 20, 100, 150, 50, 100]
pod 19 '<jp>' type K max=1 per=1 spr=0 hit=(70,300) life=30 fan=(0, 0),(0, 0) lock=1
   gain=(25, 10) speed=(150, 140) hover=(10, 10) cone=110 range=300 pause=0 elev=(75, 5) lspeed=(100, 50) lframes=(5, 10) bounce=0 scale=100
   E: N=4 gap=8 grow=5 tail=19 r1=0.75 r2=0.85 v=2.12 dmg=14 down=300 react=[100, 60, 20, 100, 100, 60, 100]
pod 24 '<jp>' type J max=1 per=3 spr=25 hit=(60,300) life=15 fan=(0, 0),(0, 0) lock=1
   gain=(10, 10) speed=(5, 5) hover=(10, 10) cone=0 range=0 pause=0 elev=(0, -30) lspeed=(233, 233) lframes=(15, 15) bounce=0 scale=100
   E: N=1 gap=1 grow=10 tail=70 r1=0.45 r2=0.62 v=3.57 dmg=37 down=300 react=[150, 5, 20, 100, 150, 5, 100]
pod 25 'Ricochet Pod' type A max=1 per=3 spr=90 hit=(85,85) life=240 fan=(0, 0),(0, 0) lock=1
   gain=(0, 0) speed=(80, 50) hover=(10, 10) cone=250 range=350 pause=0 elev=(0, 5) lspeed=(100, 100) lframes=(30, 50) bounce=1 scale=100
   E: N=1 gap=1 grow=9 tail=34 r1=0.8 r2=0.9 v=0.59 dmg=35 down=300 react=[100, 50, 5, 100, 100, 50, 100]
pod 26 '<jp>' type M max=2 per=1 spr=0 hit=(100,400) life=120 fan=(180, 0),(180, 0) lock=1
   gain=(5, 30) speed=(60, 90) hover=(10, 10) cone=160 range=250 pause=0 elev=(200, 200) lspeed=(100, 100) lframes=(10, 10) bounce=0 scale=100
   E: N=1 gap=1 grow=5 tail=34 r1=1.0 r2=1.2 v=0.59 dmg=45 down=300 react=[60, 90, 30, 15, 60, 90, 15]
pod 30 '<jp>' type B max=1 per=3 spr=90 hit=(300,200) life=60 fan=(0, 0),(0, 0) lock=1
   gain=(10, 30) speed=(30, 90) hover=(180, 180) cone=300 range=300 pause=0 elev=(45, 0) lspeed=(200, 200) lframes=(15, 15) bounce=1 scale=100
   E: N=1 gap=1 grow=1 tail=26 r1=1.0 r2=1.2 v=0.3 dmg=35 down=300 react=[100, 50, 20, 100, 100, 50, 100]
pod 34 '<jp>' type A max=1 per=1 spr=0 hit=(100,100) life=60 fan=(0, 0),(0, 0) lock=1
   gain=(10, 30) speed=(40, 40) hover=(10, 10) cone=90 range=250 pause=0 elev=(0, 5) lspeed=(100, 50) lframes=(10, 50) bounce=0 scale=250
   E: N=1 gap=1 grow=180 tail=180 r1=2.4 r2=0.1 v=0.59 dmg=8 down=2000 react=[200, 50, 20, 100, 200, 50, 100]
```


---------------------------------------------------------------------------------------------------------------

## 2. Bomb behaviour (FUN_8003f248 = 0x8003f248 launch/aim, FUN_8003fe98 = 0x8003fe98 update, FUN_80041140 init)

State, per slot k (8 slots of 0xa78 at L+0x1c0 + k*0xa78; "slot"): +4 u8 owner copy (L+0x184); +5 state (0 free, 1 flying,
2 landed/fuse, 3 exploding); +6 shot index; +8 s8 hop counter; +0xc pos (3 f32); +0x18 vel (unit direction); +0x24 arc
origin; +0x30 target T; +0x4c s16 t; +0x4e s16 fuse counter (state 2) / burst age; +0x50 s16 Tflight; +0xa8 X (explosion
state, 2.5).

### 2.1 Aim (button 0x20 pressed, S+0x28ece (= L+0x18e) < 1) -- FUN_8003f248, modes 0/1
mode = 1 if the robot is airborne (S+0x139 == 2) else 0 (movement code 0x80049924; also called with 1 from action 10 entry).
L+0x18d = 1 (aiming), L+0x188 = pointer to S+0x104 (the stick vector), L+0x18e = s16 0x801c6c10 = 4.
Cursor C = L+0x194.. : C = own_pos*(1 - a/100) + opp_pos*(a/100), a = desc.aim (one value for x,y,z); then C.y = floor height
at (C.x, C.z) via FUN_8006207c(x, 50, z, 1); then a sweep (r 0.3, 80061a18) from C to the robot position S+0x20: if it hits
a surface with normal.y < 0.3 (a wall) the cursor snaps to the contact point. L+0x1b0 (float 0..1) is a HUD fade.

Each frame while L+0x18d == 1 (FUN_8003fe98 head): after L+0x18e <= 0 the cursor moves
`C += 0.1 * s16(0x801c6c12 = 175) * 0.01 * (stick.x, stick.y, -stick.z)` = 0.175 units/frame at full stick; a 0.3 sweep from
the new XZ to the old position stops it at walls; C.y re-floored (floor query from y 50); in stage id 0x18 only
(DAT_803756a1) x is clamped >= -6.7. So the bomb is aimed with the stick; release throws.

### 2.2 Throw -- FUN_8003f248 modes 2 (ground) / 3 (air), from actions 4 / 10 when button 0x20 is no longer held
Mode 2 if S+0x139 != 2 else 3 (desc 0 / desc 1). For shot k = 0..n-1 (stops if all 8 slots busy):
 * slot: state 1, owner byte, hop counter +8 = 0, +0x4c t = 0, desc pointer.
 * origin O = translation of the world matrix of the JObj at S+0x8f8 (+0x44); initial dir = its X column, normalised
   (the bomb launcher joint; CLAUDE.md maps bomb root 1 -> rig slot 4, not re-verified here). slot+0x24 = O.
 * forward f = horizontal part of dir, normalised (0,0,1 if zero); side s = f rotated 90 deg about Y.
 * target T = C + 0.01*lat*s + 0.01*fwd*f + (0, 0.0001, 0), (lat, fwd) = s16 desc[0x30+8k], desc[0x32+8k].
 * Tflight = int(10 * 100 * |T-O| / speed) = 1000*dist/speed frames (min 1) -> slot+0x50 (e.g. speed 160: 6.25 frames/unit).
 * A wall sweep (r 0.0001, 80061b90) from O to the matrix at S+0x1284 (translation) pulls O to the contact if there is one.
 * L+0x1b4 = -vel (HUD/recoil vector), effect spawned for k == 0.
After the loop: L+0x18d = 0 and, if a slot was used, L+0x18e = desc.reload.
The robot's own part: action 4 sets S+0x528 = 0 on the throw frame (see 4).

### 2.3 Flight (state 1), per frame -- parametric arc, no integrated gravity
Normal flight when hop counter == 0xff (or <= 0):
 * if t < Tflight: t++; d = normalise(T - O), dist = |T-O|; up' = d x (normalise(up x d)); side' = normalise(up x d);
   `pos = O + dist*(t/Tflight)*d + 0.003*dist*sin(pi*t/Tflight) * (A*up' + B*side')`, A = s16 desc[0x34+8k] (arc height,
   100 -> apex 0.3*dist above the line), B = s16 desc[0x36+8k] (sideways swing). vel = normalise(pos_new - pos_old).
   The arc is only drawn from O to T; air bombs (desc1) mostly have A = B = 0 = straight line.
 * if t >= Tflight (overshot): `pos += 0.001*speed * vel` (keeps flying along the last direction at `speed`); if L+0x5581
   (Double Mine): vel.y = const, vel.x,z *= 0.005 (effectively stops).
Hop branch (counter > 0, != 0xff): vel = (0,1,0), pos += 0.0005*speed*up, O = pos, counter--; at 0 -> 0xff. (see wall hit)
Collision each frame: sweep (0.0001) between old and new pos (80061b38). If it hits and the surface flag (0x8032c274 & 0x40)
is clear: pos = contact, n = normal:
 * if t > 5, or |n.y| > 0.5 (floor/ceiling), or the counter is already 0xff  -> state 2 (landed), slot+0x4e = 0;
 * else (wall within the first 5 frames, first time): pos += 0.1*n; counter = 3000/speed (u8; 18 for 160); Tflight *= 2;
   t = 0 -> a hop straight up for `counter` frames, then the arc restarts from the hop apex to the same T over 2*Tflight.
 * pos.y >= 30 -> state 2.
Detonation by proximity: for each enemy robot (mask DAT_8032a604[S+0x8d]) if |robot.S+0x20 - pos| < 0.01*L+0xaa (3D) [for
bomb 14 also if the point 1 unit below pos is within it] -> go to state 3 (2.4). Robots are tested at S+0x20 (feet), not at the hit centre.
Out of arena: dx^2+dz^2 from (0x8024e100, 0x8024e104) > 0x8024e108 -> release effect, state 0, NO explosion.
Only the bomb whose state-3 explosion is running can hurt anyone; a flying/landed bomb never damages by itself.

### 2.4 Landed (state 2)
Each frame: age = slot+0x4e++; if age < fuse: explode early if an enemy is within the proximity radius (same test); else
(age >= fuse) explode. fuse = desc[2]: 0 for most; Rogue/Idling 120 (ground) / 10 (air); Double Mine 240/120; Giant 60.
Nothing moves in state 2.

### 2.5 Explosion (state 3) -- entry in state 1/2 code, step FUN_80044a90, hit FUN_800443c0
Entry: vel.y = 0, vel = normalise(vel) (or (0,0,1) if zero) * 0.001*speed. If L+0x5582 (Smash): d = normalise(horizontal(T-O)),
pos = (translation of matrix S+0x1284) + 2.05*d, y += 2.45, i.e. a blast right in front of the owner. FUN_80044ea4(X, E, pos, vel):
X+0 = 1 (active), X+2 = t = 0, X+4 pos, X+0x10 vel (drift per frame), four burst slots cleared. X+0x9cc = L+0x5583 (1 only for label "Type R").
Each frame (called from the slot loop): if t > grow+(N-1)*gap+tail: X.active = 0 (slot freed by the caller when X.active==0).
Else if t <= (N-1)*gap and t % gap == 0: spawn burst k = t/gap at X.pos with y += 0.03 (burst: +0 active, +1 hit-mask, +2 age,
+4 radius, +0x24 pos). `X.pos += X.vel; t++`. Each active burst: r(age) (1.1), age++, dies after grow+tail. So multi-burst bombs
(Wave: N=3, gap 8..12) lay bursts spaced gap*0.001*speed along the throw direction.
Hit test (FUN_800443c0 @ 0x800443c0, run for every state-3 slot after the update, once per frame, using the burst age already
incremented by 44a90 that frame; power = 0.01*body[0x4d]):
for each burst and each enemy robot not already in burst+1's bitmask:
  v = robot_hit_centre(S_v+0x1dc..) - burst.pos; R = 0.01*u16(S+0x1c44) (body+0xac, 18..50; read from the global S = the OWNER);
  hit iff v.x^2 + v.z^2 < (R + r)^2  and  |v.y| < r*E.v + R.
Hit centre S+0x1dc = 0.5*(matrix S+0x10d4 translation + matrix S+0x1284 translation); if u16 S+0x1c46 != 0 it is pulled to
2/3 of the way from S+0x20 (0x8003b528).
Ranged hit sets (see 5).

### 2.6 Timers
L+0x18e: the bomb button works again when it is < 1 (movement code tests S+0x28ece). Set to 4 at aim start, to desc.reload
(usually 90) at the throw, -1 per frame at the end of FUN_8003fe98 (and a second -1 per frame while aiming, in the cursor block).

### 2.7 Special bombs (flags): Tomahawk 14 (two-point proximity), Smash 27 (blast at owner, prox 45.0, speed 1000, arc
0.9*dist), Double Mine 28 (stops at target, fuse 240).

---------------------------------------------------------------------------------------------------------------

## 3. Pod behaviour (FUN_8003d7e0 launch, FUN_8003dd38 update, FUN_8003f048 init)

Pod slot Q (3 slots of 0xa60 at P+0x194 + i*0xa60): +0 state (0 free, 1 flying, 2 exploding); +1 owner byte (P+0x17c);
+4 pos; +0x10 vel (units per frame, already 0.01-scaled); +0x1c heading (unit, horizontal); +0x28 previous pos;
+0x41 locked; +0x42 pause counter; +0x43 launch-phase counter; +0x44 age; +0x46 s16 lifetime; +0x4c scale; +0x50 target (S+0x238);
+0x90 X (explosion state, same layout as bombs).

### 3.1 Launch (FUN_8003d7e0(P, dir, airborne)), from action 5 / 11, once per press at the first frame
Requires P+0x188 (pods out, s8) < P+0xe9 (max). For i in 0..per-1 (stops if no free slot): pods-out++, state 1, Q+0x46 = life,
origin Q+4 = translation of matrix S+0x10d4 (robot torso joint; the same matrix other robots aim at), heading
Q+0x1c = (dir.x, 0, -dir.z) normalised (dir = stick direction or last facing). Spread yaw s_i (1.3) rotates a copy of the heading;
v = that heading pitched up by elev[air] degrees about (heading x up) (positive = up), normalised, * 0.1*lspeed[air] * 0.01
= `0.001*lspeed` units/frame. Launch counter Q+0x43 = lframes[air]. Then the heading Q+0x1c is reset to dir and rotated by
-yaw (fan angle, rec 0xec..0xf2). Lock/pause cleared.

### 3.2 Launch phase (Q+0x43 > 0)
`pos += vel` (constant velocity, NO gravity), Q+0x43--. Lifetime does not tick. If P+0x5001 == 0 the detonation test (3.5) runs.
### 3.3 Homing phase (Q+0x43 <= 0), per frame
 1. delta = target.S+0x20 - pos, y = 0. Lock: if lock enabled (rec 0xf4) and |delta| < 0.01*range and angle(delta, heading) < cone
    -> Q+0x41 = 1 (and on the 0 -> 1 edge Q+0x42 = pause); else Q+0x41 = 0.
 2. Steering (always, locked or not): signed angle e = atan2(dot(up x heading, delta), dot(heading, delta));
    heading rotated about Y by e * 0.001 * gain[lock] (gain = rec 0xf5 / 0xf6; with the old lock state).
 3. Q+0x42 > 0: Q+0x42--, vel = 0 (frozen). Else vel.xz = heading.xz * 0.001 * speed[lock] (rec 0xf8 / 0xfa).
 4. vel.y -= 0.003; pos += vel.
 5. Common to both phases: wall sweep r 0.3 (80061b90) from new to old pos; on a hit, pos = contact; if vel . n < 0:
    vel and heading -= c*(v.n)*n with c = 1 (slide) if rec 0x112 == 0 else 2 (reflect). Pods are never destroyed by walls.
 6. floor = FUN_8006207c(pos.x, pos.y+0.5, pos.z, 0.5); pos.y = max(pos.y, floor).
 7. After the launch phase only: `vel.y += 0.01*(0.01*hover[lock] + 0.6 + floor - pos.y); vel.y *= 0.9` (spring to a hover height).
 8. lifetime Q+0x46-- each frame in step 1-4; at 0 the pod explodes.
### 3.4 Detonation
State 1 -> 2 when: (a) target within horizontal 0.01*rec 0xe6 and vertical per 1.3 flag test (rec 0xe8, target at S+0x20,
pod hover ~0.6-0.7 above floor), (b) lifetime 0, (c) outside the arena radius (same test as bombs). Entry:
FUN_80044ea4(Q+0x90, E, pos, vel), X+0x9cc = P+0x5000. The pod slot is freed, and P+0x188 decremented, only when the explosion
X ends (so a pod "is out" until its explosion finished).
### 3.5 Hit test
FUN_800443c0(power = 0.01*body[0x4e], Q+0x90, E, 3, 1) -- identical rules to 2.5 (kind = 3).

---------------------------------------------------------------------------------------------------------------

## 4. Robot actions

Button masks: S+0x242 = pressed this frame, S+0x23e = held. Bomb 0x20, pod 0x1000. Entry code in 0x80049924 (also the
air-dash states 0x80048050.. for 10/11). s16 table at 0x801c6c00: 0x384, then 0x801c6c0c = 40, 0x6c0e = 60, 0x6c10 = 4, 0x6c12 = 175,
0x6c14 = 10, 0x6c16 = 250, 0x6c18 = 50, 0x6c1a = 100.

 * Action 4 (ground or air bomb aim/throw, FUN_800490c8 @ 0x800490c8). Press (S+0x28ece < 1 and pressed): S+0x13a = 0,
   S+0x528 = 4, S+0xe2 = S+0xe0 = 4, S+0x138 = 0, S+0x548 = 0, velocity S+0x10..0x18 = (0,0,0) (the robot stops dead),
   S+0x128 = 0, FUN_8003f248(L, S+0x104, airborne). Each frame: if S+0xe2 >= 1 it just counts down (4 frames minimum
   hold); then, when the button is NOT held, throw (mode 2, or 3 if S+0x139 == 2) and S+0x528 = 0 on that very frame (no
   recovery frames; the cost is the reload L+0x18e). While held the robot faces the cursor (S+0x164 = cursor - pos, z negated),
   cannot move, and if airborne (S+0x139 == 2) S+0x14 = 0.1 * -40 = -4.0 (slow sink). Ground: a turn animation rotates the
   facing 0.10472 rad (6 deg)/frame for a +-30 frame window set when the stick points away from the facing.
 * Action 5 (pod, FUN_80048e7c @ 0x80048e7c). Press (pressed and P+0x188 < P+0xe9): S+0x13a = 0, S+0x528 = 5, S+0xe2 = S+0xe0 = 10,
   S+0x548 = 0. Each frame while S+0xe2 >= 1: facing = normalised stick (or the last facing); airborne: S+0x14 = -4.0;
   on the first frame (S+0xe0 == S+0xe2) FUN_8003d7e0(P, facing, airborne) launches; S+0xe2--; when S+0xe2 < 1 -> S+0x528 = 0.
   So the pod action lasts 10 frames and launches on its first. Ground velocity is NOT zeroed by this action code
   (not settled whether the generic code stops the robot).
 * Action 10 (bomb while sky-dashing, FUN_8004752c @ 0x8004752c). Entered from the sky-dash state 8 on press (S+0xe2 = 4,
   FUN_8003f248(L, S+0x104, 1)). Keeps the sky-dash steering/speed (S+0x13a counts the dash down, as in action 8). After 4
   frames, on button release: FUN_8003f248(L, dir, 3) (air bomb, desc1), S+0x528 = 8 (back to the sky dash). If the dash ends
   first (S+0x13a == 0): S+0x528 = 4 and S+0xe4 = S+0x33430 (landing lag stat 10).
 * Action 11 (pod while sky-dashing, FUN_80046f58 @ 0x80046f58). S+0xe2 = 10 on entry; on the first frame
   FUN_8003d7e0(P, normalised stick or last dir, airborne flag); counts down; at 0 -> action 8. The sky dash continues
   steering throughout; if the dash ends (S+0x13a == 0) -> action 0 with S+0xe4 = S+0x33430.
 * Timer summary: S+0xe0 = initial, S+0xe2 = remaining frames of the current action (-1/frame). Bomb reload: L+0x18e (per
   part record, usually 90 frames = 1.5 s, 60..240). Pods have no reload; the only limit is P+0xe9 pods out.

---------------------------------------------------------------------------------------------------------------

## 5. What a hit passes (FUN_800443c0 writes the victim's hit record H = S_v+0x18a0; consumer = 0x8004b0a0)

| H+ | type | content from explosions | notes |
|----|------|-------------------------|-------|
| 0x01 | u8 | hit counter ++ | |
| 0x02 | s16 | kind: 2 = bomb, 3 = pod (1 gun, 4 melee, 5 other) | param_4 of 443c0 |
| 0x04 | ptr | attacker S (global S at hit time) | |
| 0x08..0x10 | 3 x f32 | (v.x, 0, -v.z) normalised, v = victim hit centre - burst pos | only used when H+0x26 == 0 |
| 0x14 | s16 | damage: += int(E14 * power + old); power = 0.01*body[0x4d] (bomb) / body[0x4e] (pod) of the OWNER | |
| 0x16 | s16 | down value: += E16 | melee/gun-style hits set 0xffff (always down) |
| 0x18..0x24 | 7 x u16 | copy of E+0x18..E+0x24 | |
| 0x26 | u8 | = X+0x9cc: 1 only when the explosion-type label is "Type R" (bomb 6, pod 27) | H+0x26 != 0 -> the push direction is
  victim.S+0x20 - attacker.S+0x20 (z negated), i.e. away from the OWNER; every other type (0) pushes away from the burst centre via H+8/H+0x10 |

The consumer (0x8004b0a0 lines ~1130..1480 of bp_a_move.c; victim body B = S_v+0x1b98, H as above) -- verified from the decompile,
semantics of the body bytes guessed from their use:
 * Damage applies only while the victim's opponent (S+0x238) is alive (its HP >= 1) or flag 0x100000 of _DAT_80321d40 is set.
 * HP S+0x74 (starts at u16 0x801c5bdc + 16*slot = 1000): grounded victim: `HP -= 1000*(dmg*B[0x42]/900)/100` = dmg*B[0x42]/90;
   victim in a knocked/airborne action (0xe/0xf): dmg is first scaled by B[0x47+kind]/100 (body 0: 50 for bombs and pods) and
   `HP -= dmg*scale*B[0x42]/9000`. HP < 1 forces the knocked-down branch (S+0x144 = 0xffff).
 * Down gauge S+0x144 (s16) += H+0x16. If the result < u16 body+0x40 (Ray 1000, Agilia 200, Bronzia 1800) the victim only
   flinches (action 0xd): stun S+0xe2 = H+0x1c * B[0x44]/100 frames; velocity = dir_h * cos(H+0x1a deg) *
   H+0x18*B[0x43]/1000 horizontally and sin(H+0x1a deg)*same vertically, S+0x139 = 2. Otherwise gauge := 0xffff and the victim is
   launched (action 0xe, stun B[0x46] frames): v = (cos(H+0x22) * dir_h, sin(H+0x22)) * H+0x20 * B[0x43]/1000.
   (Velocity in the robot's units, hundredths per frame; angles in degrees.) H+0x1e and H+0x24 are not read here.
   Example: bomb 0 hits for 68 damage, 600 down, flinch speed 120 at 70 deg for 30 frames, launch 120 at 70 deg.

---------------------------------------------------------------------------------------------------------------

## 6. Things not settled

1. Which rig slot S+0x8f8 (bomb joint) and the matrices S+0x10d4 / S+0x1284 correspond to (0x1284 = 0x10d4 + 9*0x30). Judging
   from other code S+0x10d4 is the torso/root matrix and the hit centre is the midpoint of the two.
2. What S+0x104 is exactly (stick vector in velocity convention; magnitude may exceed 1?) and the range of its length.
3. Whether the radius R in the explosion hit test really comes from the OWNER (global S, body+0xac) -- as decompiled, yes; the
   robot position tested is the VICTIM'S S+0x1dc.
4. E+0x1e / E+0x24 purpose (stored into H+0x1e / H+0x24, not read by the code examined); the byte at rec+0x113 (pod scale);
   record tails (bomb 0x144.., pod 0x144..), the bomb byte rec+0xa0, desc u16 at +6; the u8 at slot+4 (L+0x184).
5. Whether action 5 or the generic code zeroes the ground velocity during the 10 pod frames; the exact facing-turn behaviour
   in action 4.
6. Mask semantics (DAT_8032a604 / DAT_8032a5f4 / _DAT_80321d40 bits) are only assumed to mean "enemy robots" in 1v1.
7. Gun (kind 1) and melee (kind 4/5) hit parameters are out of scope. Units of the reaction velocity beyond "hundredths per
   frame like S+0x10" were not cross-checked against battle.c.
8. In bomb flight the branch `S+0x1d0 != 0x1e || counter in {-1,0}` (S+0x1d0 = stage-related u16 from 0x801c5bd6 table) was not analysed; arena id 0x1e would behave differently while the hop counter is positive.
9. Pod launch spread when rec 0xe4 < 0 (code path exists, 0x168 = 360 - no record uses it).

## Tooling note
tools/ghidra/decompile.ps1 -Proj is broken: `$proj = Join-Path ...` overwrites the `$Proj` parameter (PowerShell variable names are
case-insensitive), so every call uses build\ghidra\proj and the copy is never made. I did NOT edit it (outside build/); this
session used build/bp_decompile.ps1 (private copy build\ghidra_bp) instead. The first run (bp_hit.c) used the shared project and
succeeded before the wrapper existed.

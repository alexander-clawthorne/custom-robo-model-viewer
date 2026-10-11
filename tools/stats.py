#!/usr/bin/env python3
"""Per-part gameplay stats, read out of main.dol.

    python tools/stats.py                 # body and leg tables, combined stats
    python tools/stats.py --cook          # -> romfs/stats/parts.bin (src/battle.c)

The five part tables are compiled into the DOL, not shipped as files. A save
slot holds one index per part and the robot builder at 0x80005da4 copies the
indexed record whole into the robot struct S (= *(r13-0x5078)):

  part   table       stride  count  copied to   flag byte (illegal part)
  body   0x801c6c44  0x134   34     S+0x1b98    +0x45
  gun    0x801ca560  0x28c   64     S+0x1e28    +0xa0
  bomb   0x801d4860  0x17c   52     S+0x28d48
  pod    0x801d9590  0x174   57     S+0x2e2e8
  leg    0x801c952c  0x8f    29     S+0x33308   +0x80

A body record opens with its Japanese name (32 bytes, Shift-JIS) and US name
(32 bytes); a leg record with four 32-byte strings (name, display name, short
label such as "speed" or "grip", boost effect name). The 170 chara.BIN models
are the 34 bodies in table order, five colour variants each.

0x80006658 then combines body and leg into 15 movement stats (s16 at
S+0x3341c). Each body holds a (min, normal, max) triple per stat and the leg a
percentage p, 100 being neutral:
    p <= 100:  min + p * (normal - min) / 100
    p >  100:  normal + (p - 100) * (max - normal) / 100
truncated to an integer. Stat 14 is the body's own value, the leg plays no part.

What each stat does, from the code that reads it (movement 0x80049924, air
dash 0x80046f58, gravity 0x8004b0a0). Velocity is in hundredths of a world
unit per frame -- the position update is pos += 0.01 * vel (z negated) -- and
the game runs at 60 Hz:
  0  ground speed: target 0.1 * s0 * stick
  1  ground acceleration: speed moves 0.001 * s1 per frame toward the target
  2  turn rate on the ground: 0.1 * s2 degrees per frame
  3  jump height with the button held throughout
  4  frames the button may be held to keep rising
  5  air speed cap: 0.1 * s5
  6  air acceleration: 0.003 * s6 per frame
  7  landing lag: frames at full impact (a 6-unit fall), scaled by impact
  8  gravity: 0.01 * 0.3 * 2.0 * s8 per frame (2.0 is the float at 0x801c6c04)
  9  air-dash wind-up: frames the robot hangs still first (S+0xe2)
  10 landing lag owed after an air dash (S+0xe4)
  11 air-jump height (dash type 1): vy = sqrt(2 g s11)
  12 air-dash / sky-dash length in frames
  13 air-dash / sky-dash speed: 0.1 * s13
  14 sky-dash steering: 0.1 * s14 degrees per frame
The press itself (in 0x80049924) picks one of three actions by the body's
dash type at +0x56, from the action table at 0x801f8eb8 indexed by S+0x528:
  type 0  action 7, air dash   0x80048708  a straight line
  type 1  action 6, air jump   0x80048b10  a second jump straight up
  type 2  action 8, sky dash   0x80048050  steerable, long; gun / bomb / pod
                                           presses during it are actions 9-11
Combat bytes of a body, from the hit code in 0x8004b0a0. Correction: +0x40
was first read as HP; every robot has 1000 HP (the float at S+0x74) and
+0x40 is the knockdown threshold the down gauge is compared with.
  +0x40 u16 down threshold  +0x42 damage taken %  +0x43 knock-back %
  +0x44 flinch time %       +0x46 knockdown frames
  +0x47+kind  damage % while knocked down (kind 1 gun, 2 bomb, 3 pod, 4 melee)
  +0x4c/4d/4e gun/bomb/pod power %   +0x4f gun recovery %   +0xac hit radius x100
The weapons (gun 0x801ca560, bomb 0x801d4860, pod 0x801d9590) are summarised
at cook_gun, cook_bomb and cook_pod below; the part models in romfs/gun, bomb
and pod share the table order (64, 52 and 57 of each).

The wiki's model pages agree (Ray 01 "Air Dash" x2, Peregrine "Air Jump" x3,
the Lightning Sky models sky dash), which is a cross-check, not the source.
The legs' labels agree: High Jump raises 3, Wide Jump 5, the G+/G- pair 8,
the long and short verniers 12.
"""
import argparse, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from dol import Dol
import muzzle
import motion

BODY, BODY_STRIDE, BODIES = 0x801c6c44, 0x134, 34
LEG, LEG_STRIDE, LEGS = 0x801c952c, 0x8f, 29
GUN, GUN_STRIDE, GUNS = 0x801ca560, 0x28c, 64
BOMB, BOMB_STRIDE, BOMBS = 0x801d4860, 0x17c, 52
POD, POD_STRIDE, PODS = 0x801d9590, 0x174, 57
GUN_SLOTS = 0x801f9081     # u8 per gun, stride 0x14: bullets in flight at most
POD_LOW_LIST = 0x801f9980  # 10 u16 pod ids with a one-sided vertical trigger
OUT = Path(__file__).resolve().parents[1] / 'romfs/stats'

# (format, offset) of each stat's (min, normal, max) in a body record, in
# stat order; the leg percentage for stat k is leg byte 0x81 + k.
TRIPLES = [('h', 0x70), ('h', 0x76), ('h', 0x7c), ('h', 0x82), ('h', 0x88), ('h', 0x8e),
           ('h', 0x94), ('h', 0x9a), ('H', 0xa0), ('B', 0x50), ('B', 0x53), ('H', 0x58),
           ('B', 0x61), ('H', 0x64)]
NAMES = ['run speed', 'run accel', 'turn', 'jump height', 'jump hold', 'air speed',
         'air accel', 'land lag', 'gravity', 'dash wind-up', 'after dash', 'air jump',
         'dash frames', 'dash speed', 'dash steer']

def _text(b):
    return b.split(b'\0')[0].decode('cp932', 'replace')

def body(i, dol=None):
    r = (dol or Dol()).read(BODY + BODY_STRIDE * i, BODY_STRIDE)
    triples = []
    for fmt, off in TRIPLES:
        size = struct.calcsize('>' + fmt)
        triples.append(tuple(struct.unpack_from('>' + fmt, r, off + size * k)[0] for k in range(3)))
    return {
        'name_jp': _text(r[0:32]), 'name': _text(r[32:64]),
        'down': struct.unpack_from('>H', r, 0x40)[0],
        'combat': list(r[0x42:0x50]),
        'hit_radius': struct.unpack_from('>H', r, 0xac)[0],
        'illegal': r[0x45],
        'triples': triples,
        'steer': struct.unpack_from('>h', r, 0x6c)[0],
        # Air dashes per jump (move code at 0x80049924): the count is byte
        # 0x57 when the dash type at 0x56 is 1, byte 0x5e for types 0 and 2.
        'dash_type': r[0x56],
        'dashes': r[0x57] if r[0x56] == 1 else r[0x5e],
        # The dash is turned this many degrees up from horizontal (0x80046f58).
        'dash_pitch': struct.unpack_from('>b', r, 0x60)[0],
    }

def leg(i, dol=None):
    r = (dol or Dol()).read(LEG + LEG_STRIDE * i, LEG_STRIDE)
    return {'name': _text(r[0:32]), 'label': _text(r[64:96]), 'illegal': r[0x80],
            'pct': list(r[0x81:0x8f])}

def combine(b, l):
    """0x80006658: the 15 movement stats for body b with leg l."""
    out = []
    for (lo, mid, hi), p in zip(b['triples'], l['pct']):
        if p > 100:
            v = mid + (p - 100) * (hi - mid) / 100
        else:
            v = lo + p * (mid - lo) / 100
        out.append(int(v))
    return out + [b['steer']]

def ascii_name(s, fallback):
    return s if s and all(32 <= ord(c) < 127 for c in s) else fallback

def _blast(r, o):
    """Explosion descriptor, shared by bombs (+8 into a throw mode) and pods
    (+0x114): bursts, frames between them, frames to grow to r1, then on to
    r2; the radii and the height factor; then the hit: damage, down value,
    flinch push, angle, frames and gravity %, launch speed, angle and gravity
    % (the gravity words are read by the stagger and knockdown actions
    0x800468b8 / 0x80045fec), pad."""
    n, gap, grow, tail = struct.unpack_from('>BBHH', r, o)
    r1, r2, v = struct.unpack_from('>3f', r, o + 8)
    return struct.pack('<BBHHH3fHh7hh', n, gap, grow, tail, 0, r1, r2, v,
                       *struct.unpack_from('>Hh7h', r, o + 0x14), 0)

def cook_gun(d, g):
    """char name[24]; u8 cooldown, home-only-ahead, slots, gun index; then the
    ground and the air pattern: u8 wind-up, volleys, per volley, fan axis,
    interval, recovery, fan phase, pad; s16 cone, fan start, fan step, pad
    (65536 = 360 deg); five phases of u16 frames, u8 trail, pad, f32 speed,
    yaw and pitch homing (rad/frame), radius, and the hit as in _blast;
    last the muzzle (tools/muzzle.py): f32 point[3], axis[3] in the part
    root's frame, part units."""
    r = d.read(GUN + GUN_STRIDE * g, GUN_STRIDE)
    out = ascii_name(_text(r[0x20:0x40]), f'gun {g}').encode('ascii')[:23].ljust(24, b'\0')
    out += struct.pack('<4B', r[0xa1], r[0xa2], d.read(GUN_SLOTS + 0x14 * g, 1)[0], g)
    for base in (0xdc, 0x1b4):
        P = r[base:base + 0xd8]
        out += struct.pack('<8B', P[0], P[1], P[2], P[3], P[4], P[5], P[8], 0)
        out += struct.pack('<4h', struct.unpack_from('>h', P, 6)[0], struct.unpack_from('>h', P, 0xa)[0],
                           struct.unpack_from('>h', P, 0xc)[0], 0)
        for k in range(5):
            o = 0x10 + 0x28 * k
            dur, = struct.unpack_from('>H', P, o)
            sp, yaw, pitch, rad = struct.unpack_from('>4f', P, o + 4)
            out += struct.pack('<HBB4fHh7hh', dur, P[o + 0x14], 0, sp, yaw, pitch, rad,
                               *struct.unpack_from('>Hh7h', P, o + 0x16), 0)
    return out + _muzzle('gun', g)

def _muzzle(kind, index):
    got = muzzle.muzzle(kind, index) or ((0.0, 0.0, 0.0), (1.0, 0.0, 0.0))
    return struct.pack('<6f', *got[0], *got[1])

def cook_bomb(d, b):
    """char name[24]; u8 count, flags (1 Tomahawk, 2 Double Mine, 4 Smash,
    8 type R), pad[2]; u16 trigger radius x100, pad; then the ground and the
    air mode: u8 aim %, reload, fuse, pad; u16 speed x1000, pad; s16
    shot[4][4] (side, forward x100, arc height, swing); the blast; then the
    throw origin as the gun's muzzle (bomb root 1, spawn entry 1)."""
    r = d.read(BOMB + BOMB_STRIDE * b, BOMB_STRIDE)
    flags = {14: 1, 28: 2, 27: 4}.get(b, 0) | (8 if _text(r[0x40:0x60]).endswith('Ｒ') else 0)
    out = ascii_name(_text(r[0x20:0x40]), f'bomb {b}').encode('ascii')[:23].ljust(24, b'\0')
    out += struct.pack('<4BHH', r[0xa1], flags, 0, 0, struct.unpack_from('>H', r, 0xa2)[0], 0)
    for o in (0xa4, 0xf4):
        out += struct.pack('<4BHH', r[o], r[o + 1], r[o + 2], 0, struct.unpack_from('>H', r, o + 4)[0], 0)
        out += struct.pack('<16h', *struct.unpack_from('>16h', r, o + 0x30))
        out += _blast(r, o + 8)
    return out + _muzzle('bom', b)

def cook_pod(d, p, low):
    """char name[24]; u8 max out, per press, lock-on, reflect, gain [free,
    locked], flags (1 no trigger while launching, 2 one-sided vertical
    trigger, 4 type R), pad; s16 spread; u16 trigger range across and up,
    life; s16 yaw [ground, air][last, first]; u16 speed [free, locked]
    x1000, hover x100, lock cone deg, lock range x100; s16 pause, elevation
    [ground, air], launch speed x1000, launch frames, pad; the blast."""
    r = d.read(POD + POD_STRIDE * p, POD_STRIDE)
    flags = (1 if p == 30 else 0) | (2 if p in low else 0) | (4 if _text(r[0x60:0x80]).endswith('Ｒ') else 0)
    out = ascii_name(_text(r[0x20:0x40]), f'pod {p}').encode('ascii')[:23].ljust(24, b'\0')
    out += struct.pack('<8B', r[0xe1], r[0xe2], r[0xf4], r[0x112], r[0xf5], r[0xf6], flags, 0)
    out += struct.pack('<hHHH', *struct.unpack_from('>hHHH', r, 0xe4))
    out += struct.pack('<4h', *struct.unpack_from('>4h', r, 0xec))
    out += struct.pack('<6H', *struct.unpack_from('>6H', r, 0xf8))
    out += struct.pack('<8h', *struct.unpack_from('>7h', r, 0x104), 0)
    out += _blast(r, 0x114)
    return out

def cook():
    """romfs/stats/parts.bin, little-endian:
      'STA5', u32 body, leg, gun, bomb, pod counts
      body: char name[24], u16 down threshold, s16 triples[14][3], s16 steer,
            u8 dash type, u8 dashes, s8 dash pitch, u8 pad,
            u8 combat[14] (body bytes 0x42-0x4f), u16 hit radius x100,
            then the whole 0x134-byte record as the DOL has it (big-endian),
            char tackle motion[32], char win motion[32]
      leg:  char name[24], u8 pct[14], u8 pad[2]
      gun, bomb, pod: see cook_gun, cook_bomb and cook_pod.
    Names are ASCII; a part without one gets its table index."""
    d = Dol()
    names = motion.motion_names()
    out = bytearray(b'STA5' + struct.pack('<5I', BODIES, LEGS, GUNS, BOMBS, PODS))
    for i in range(BODIES):
        b = body(i, d)
        out += ascii_name(b['name'], f'body {i}').encode('ascii')[:23].ljust(24, b'\0')
        out += struct.pack('<H', b['down'])
        for t in b['triples']:
            out += struct.pack('<3h', *t)
        out += struct.pack('<hBBbB', b['steer'], b['dash_type'], b['dashes'], b['dash_pitch'], 0)
        out += bytes(b['combat']) + struct.pack('<H', b['hit_radius'])
        out += d.read(BODY + BODY_STRIDE * i, BODY_STRIDE)   # the record itself, big-endian
        # The body's own clips: motion index 0x23 of its table is the tackle,
        # global ID 203 + body; the win poses are 101 + 2 body and the next.
        for ident in (203 + i, 101 + 2 * i):
            out += names.get(ident, '').encode('ascii')[:31].ljust(32, b'\0')
    for i in range(LEGS):
        l = leg(i, d)
        out += f'leg {i}'.encode('ascii').ljust(24, b'\0')
        out += bytes(l['pct']) + b'\0\0'
    for g in range(GUNS):
        out += cook_gun(d, g)
    for b in range(BOMBS):
        out += cook_bomb(d, b)
    low = set(struct.unpack('>10H', d.read(POD_LOW_LIST, 20)))
    for p in range(PODS):
        out += cook_pod(d, p, low)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'parts.bin').write_bytes(bytes(out))
    print(f'{BODIES} bodies, {LEGS} legs, {GUNS} guns, {BOMBS} bombs, {PODS} pods, '
          f'{len(out)} bytes -> {OUT / "parts.bin"}')

def main():
    a = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    a.add_argument('--cook', action='store_true')
    args = a.parse_args()
    if args.cook:
        cook()
        return
    d = Dol()
    std = leg(0, d)
    print('body               down  dashes  ' + '  '.join(n.split()[-1][:5] for n in NAMES))
    for i in range(BODIES):
        b = body(i, d)
        print(f'{i:2d} {ascii_name(b["name"], b["name_jp"])[:16]:16s} {b["down"]:5d}  {b["dashes"]}  ',
              ' '.join(f'{v:5d}' for v in combine(b, std)))
    print('\nleg percentages (100 = neutral)')
    for i in range(LEGS):
        l = leg(i, d)
        print(f'{i:2d} {l["label"]:10s}', ' '.join(f'{p:3d}' for p in l['pct']))

if __name__ == '__main__':
    main()

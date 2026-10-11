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
The wiki's model pages agree (Ray 01 "Air Dash" x2, Peregrine "Air Jump" x3,
the Lightning Sky models sky dash), which is a cross-check, not the source.
The legs' labels agree: High Jump raises 3, Wide Jump 5, the G+/G- pair 8,
the long and short verniers 12.
"""
import argparse, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from dol import Dol

BODY, BODY_STRIDE, BODIES = 0x801c6c44, 0x134, 34
LEG, LEG_STRIDE, LEGS = 0x801c952c, 0x8f, 29
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
        'hp': struct.unpack_from('>H', r, 0x40)[0],
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

def cook():
    """romfs/stats/parts.bin, little-endian:
      'STA1', u32 body count, u32 leg count
      body: char name[24], u16 hp, s16 triples[14][3], s16 steer,
            u8 dash type, u8 dashes, s8 dash pitch, u8 pad
      leg:  char name[24], u8 pct[14], u8 pad[2]
    Names are ASCII; a body or leg without one gets its table index."""
    d = Dol()
    out = bytearray(b'STA1' + struct.pack('<II', BODIES, LEGS))
    for i in range(BODIES):
        b = body(i, d)
        out += ascii_name(b['name'], f'body {i}').encode('ascii')[:23].ljust(24, b'\0')
        out += struct.pack('<H', b['hp'])
        for t in b['triples']:
            out += struct.pack('<3h', *t)
        out += struct.pack('<hBBbB', b['steer'], b['dash_type'], b['dashes'], b['dash_pitch'], 0)
    for i in range(LEGS):
        l = leg(i, d)
        out += f'leg {i}'.encode('ascii').ljust(24, b'\0')
        out += bytes(l['pct']) + b'\0\0'
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'parts.bin').write_bytes(bytes(out))
    print(f'{BODIES} bodies, {LEGS} legs, {len(out)} bytes -> {OUT / "parts.bin"}')

def main():
    a = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    a.add_argument('--cook', action='store_true')
    args = a.parse_args()
    if args.cook:
        cook()
        return
    d = Dol()
    std = leg(0, d)
    print('body                 hp  dashes  ' + '  '.join(n.split()[-1][:5] for n in NAMES))
    for i in range(BODIES):
        b = body(i, d)
        print(f'{i:2d} {ascii_name(b["name"], b["name_jp"])[:16]:16s} {b["hp"]:5d}  {b["dashes"]}  ',
              ' '.join(f'{v:5d}' for v in combine(b, std)))
    print('\nleg percentages (100 = neutral)')
    for i in range(LEGS):
        l = leg(i, d)
        print(f'{i:2d} {l["label"]:10s}', ' '.join(f'{p:3d}' for p in l['pct']))

if __name__ == '__main__':
    main()

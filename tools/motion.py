#!/usr/bin/env python3
"""Custom Robo motion banks (motion.DBN, snr_motion.BIN entries).

    python tools/motion.py list             # every robot motion with name and length
    python tools/motion.py show 0 64        # one motion's 26 nodes

Bank layout (offsets relative to the bank start):
  u32 N, then N x (u32 A, u32 X, u32 B)
  B -> 78 bytes: key count per channel (26 nodes x 3 components; 0 = no track)
  A -> 78 x (u32 times, u32 values)
       times  -> u16 frame numbers
       values -> u16 codes: low 14 bits index the bank's f32 table at X,
                 top 2 bits are a per-key tag (1 is always index 0, i.e. 0.0)
The game's evaluator (main.dol 0x8001eb48) reads the table the same way.
Rotations are in degrees, positions in model units.

Names come from the game's motion name table (main.dol 0x80206dbc, 12-byte
records): records 0-167 name bank 0, 168-261 bank 1; bank 2 only has
placeholder names (battle_dummy_046 ...).
"""
import struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = ROOT / 'data/extracted/files'
NODES, CHANNELS = 26, 78
# Node kinds measured over every key in motion.DBN (see docs/progress.html).
NODE_KIND = ['pos', 'rot', 'rot', 'pos', 'rot', 'pos', 'rot', 'rot', 'pos', 'rot', 'rot', 'rot', 'pos',
             'rot', 'rot', 'rot', 'pos', 'rot', 'rot', 'rot', 'pos', 'rot', 'rot', 'pos', 'pos', 'pos']
NAME_TABLE, NAME_RECORD = 0x80206dbc, 12
BANK_NAME_START = [0, 168, None]

class Motion:
    def __init__(self, name, tracks):
        self.name = name
        self.tracks = tracks  # 78 entries: None or (times, values, tags)
        self.length = max((t[0][-1] for t in tracks if t), default=0)

def read_bank(blob, base=0):
    n = struct.unpack_from('>I', blob, base)[0]
    motions = []
    for i in range(n):
        a, x, b = struct.unpack_from('>3I', blob, base + 4 + 12 * i)
        tracks = []
        for c in range(CHANNELS):
            k = blob[base + b + c]
            if not k:
                tracks.append(None)
                continue
            tp, vp = struct.unpack_from('>2I', blob, base + a + 8 * c)
            times = struct.unpack_from(f'>{k}H', blob, base + tp)
            codes = struct.unpack_from(f'>{k}H', blob, base + vp)
            values = tuple(struct.unpack_from('>f', blob, base + x + 4 * (v & 0x3FFF))[0] for v in codes)
            tracks.append((times, values, tuple(v >> 14 for v in codes)))
        motions.append(Motion(f'motion_{i:03d}', tracks))
    return motions

def dbn_banks(blob):
    """motion.DBN / snr_motion entries: u32 count, then count+1 offsets (last = end)."""
    count = struct.unpack_from('>I', blob, 0)[0]
    return list(struct.unpack_from(f'>{count}I', blob, 4))

def motion_names():
    """Names from the game's own table, if the DOL is there."""
    try:
        sys.path.insert(0, str(Path(__file__).parent))
        from dol import Dol
        dol = Dol()
    except Exception:
        return []
    names = []
    for k in range(436):
        p = struct.unpack('>I', dol.read(NAME_TABLE + NAME_RECORD * k, 4))[0]
        raw = dol.read(p, 64)
        names.append(raw[:raw.index(0)].decode('ascii', 'replace') if 0 in raw else f'motion_{k}')
    return names

def robot_motions():
    """[(bank, [Motion])] for motion.DBN, named where the game names them."""
    blob = (FILES / 'motion.DBN').read_bytes()
    names = motion_names()
    out = []
    for bank, base in enumerate(dbn_banks(blob)):
        motions = read_bank(blob, base)
        start = BANK_NAME_START[bank]
        if start is not None and names:
            for i, m in enumerate(motions):
                m.name = names[start + i]
        else:
            for i, m in enumerate(motions):
                m.name = f'bank{bank}_{i:03d}'
        out.append((bank, motions))
    return out

def sample(track, frame):
    """Linear interpolation between keys (placeholder until the evaluator's
    per-tag curve rules are confirmed from main.dol 0x8001eb48)."""
    times, values, _ = track
    if frame <= times[0]:
        return values[0]
    for k in range(1, len(times)):
        if frame <= times[k]:
            t0, t1 = times[k - 1], times[k]
            u = (frame - t0) / (t1 - t0) if t1 != t0 else 0.0
            return values[k - 1] + (values[k] - values[k - 1]) * u
    return values[-1]

if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'list'
    banks = robot_motions()
    if cmd == 'list':
        for bank, motions in banks:
            for i, m in enumerate(motions):
                print(f'{bank}:{i:03d} {m.length:4d} frames  {m.name}')
    elif cmd == 'show':
        bank, idx = int(sys.argv[2]), int(sys.argv[3])
        m = banks[bank][1][idx]
        print(m.name, m.length, 'frames')
        for n in range(NODES):
            trip = m.tracks[3 * n:3 * n + 3]
            if any(trip):
                print(f'node {n:2d} {NODE_KIND[n]}', [None if t is None else [round(v, 3) for v in t[1]] for t in trip])

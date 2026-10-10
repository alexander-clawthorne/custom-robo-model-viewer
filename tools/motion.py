#!/usr/bin/env python3
"""Custom Robo motion banks (motion.DBN, snr_motion.BIN entries).

    python tools/motion.py list             # robot motions: bank:index, id, length, name
    python tools/motion.py story            # unique story-character motions
    python tools/motion.py show 0 64        # one robot motion's 26 nodes

Bank layout (offsets relative to the bank start):
  u32 N, then N x (u32 A, u32 X, u32 B), then N x u16 ID
  B -> 78 bytes: key count per channel (26 nodes x 3 components; 0 = no track)
  A -> 78 x (u32 times, u32 values)
       times  -> u16 frame numbers
       values -> u16 codes: low 14 bits index the bank's f32 table at X,
                 top 2 bits are a per-key tag (1 means 0.0)
  ID -> low 15 bits: the motion's global number; bit 15: the motion has the
        root nodes 23-25 (set on every motion on the disc).
The loader at main.dol 0x80074dcc reads the IDs and fills a pointer table
(0x8032adc8) and a gate table (0x8032a8c8) indexed by them; the evaluator at
0x8001eb48 reads the keys. Rotations are in degrees, positions in model units.

Names: record ID of the table at 0x80206db0 (12-byte records) names motion ID
(the game's debug motion viewer at 0x800a0e64 prints them this way).
  1-168     robot motions, motion.DBN bank 0
  169-262   robot motions, motion.DBN bank 1
  418-1094  story-character motions: motion.DBN bank 2 (the shared walk,
            wait and talk set) plus snr_motion.BIN, one bank per scenario
            script (snr_t_01-20, snr_f_01-30) holding the motions it uses.
"""
import struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = ROOT / 'data/extracted/files'
NODES, CHANNELS = 26, 78
NODE_KIND = ['pos', 'rot', 'rot', 'pos', 'rot', 'pos', 'rot', 'rot', 'pos', 'rot', 'rot', 'rot', 'pos',
             'rot', 'rot', 'rot', 'pos', 'rot', 'rot', 'rot', 'pos', 'rot', 'rot', 'pos', 'rot', 'pos']
NAME_TABLE, NAME_RECORD = 0x80206db0, 12
ROBOT_BANKS = (0, 1)   # motion.DBN banks holding robot motions; bank 2 is story
SNR_BANK_START = 0x20  # each snr_motion.BIN entry: u32 1, u32 0x20, u32 end, pad

class Motion:
    def __init__(self, name, tracks, ident=0):
        self.name = name
        self.id = ident
        self.tracks = tracks  # 78 entries: None or (times, values, tags)
        self.length = max((t[0][-1] for t in tracks if t), default=0)

def bank_ids(blob, base=0):
    n = struct.unpack_from('>I', blob, base)[0]
    return struct.unpack_from(f'>{n}H', blob, base + 4 + 12 * n)

def read_bank(blob, base=0, names=None):
    n = struct.unpack_from('>I', blob, base)[0]
    ids = bank_ids(blob, base)
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
            values = tuple(0.0 if v >> 14 == 1 else struct.unpack_from('>f', blob, base + x + 4 * (v & 0x3FFF))[0]
                           for v in codes)
            tracks.append((times, values, tuple(v >> 14 for v in codes)))
        ident = ids[i] & 0x7FFF
        name = names.get(ident) if names else None
        motions.append(Motion(name or f'motion_{ident:04d}', tracks, ident))
    return motions

def dbn_banks(blob):
    """motion.DBN: u32 count, then the bank offsets."""
    count = struct.unpack_from('>I', blob, 0)[0]
    return list(struct.unpack_from(f'>{count}I', blob, 4))

def snr_banks():
    """snr_motion.BIN and the base of each entry's bank, in script order."""
    sys.path.insert(0, str(Path(__file__).parent))
    import hsd
    blob = (FILES / 'snr_motion.BIN').read_bytes()
    return blob, [off + SNR_BANK_START for off, _ in hsd.sfd_entries(blob)]

def motion_names():
    """{motion ID: name} from the game's own table, if the DOL is there."""
    try:
        sys.path.insert(0, str(Path(__file__).parent))
        from dol import Dol
        dol = Dol()
    except Exception:
        return {}
    names = {}
    for ident in range(1, 1095):
        p = struct.unpack('>I', dol.read(NAME_TABLE + NAME_RECORD * ident, 4))[0]
        raw = dol.read(p, 64)
        if 0 in raw:
            names[ident] = raw[:raw.index(0)].decode('ascii', 'replace')
    return names

def robot_motions(names=None):
    """[(bank, [Motion])] for the robot banks of motion.DBN."""
    names = motion_names() if names is None else names
    blob = (FILES / 'motion.DBN').read_bytes()
    bases = dbn_banks(blob)
    return [(bank, read_bank(blob, bases[bank], names)) for bank in ROBOT_BANKS]

def story_sources():
    """(blob, bank base) of every bank holding story motions: the shared set
    in motion.DBN first, then the scenario banks."""
    dbn = (FILES / 'motion.DBN').read_bytes()
    snr, bases = snr_banks()
    return [(dbn, dbn_banks(dbn)[2])] + [(snr, b) for b in bases]

def story_index():
    """{motion ID: (blob, bank base, index)} for the first copy of each story
    motion. Scenario banks repeat motions: 646 distinct IDs over 2362 copies,
    and only 9 IDs have copies whose keys differ."""
    out = {}
    for blob, base in story_sources():
        for i, ident in enumerate(bank_ids(blob, base)):
            out.setdefault(ident & 0x7FFF, (blob, base, i))
    return dict(sorted(out.items()))

if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'list'
    names = motion_names()
    if cmd == 'list':
        for bank, motions in robot_motions(names):
            for i, m in enumerate(motions):
                print(f'{bank}:{i:03d} id {m.id:4d} {m.length:4d} frames  {m.name}')
    elif cmd == 'story':
        for ident in story_index():
            print(f'id {ident:4d}  {names.get(ident, "?")}')
    elif cmd == 'show':
        bank, idx = int(sys.argv[2]), int(sys.argv[3])
        m = robot_motions(names)[bank][1][idx]
        print(m.name, m.length, 'frames')
        for n in range(NODES):
            trip = m.tracks[3 * n:3 * n + 3]
            if any(trip):
                print(f'node {n:2d} {NODE_KIND[n]}', [None if t is None else [round(v, 3) for v in t[1]] for t in trip])

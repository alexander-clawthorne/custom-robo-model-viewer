#!/usr/bin/env python3
"""Cook the motions into romfs/motion/ for the 3DS:
  robot.mot   robot motions (motion.DBN banks 0-1, IDs 1-262)
  story.mot   story-character motions (motion.DBN bank 2 and every
              snr_motion.BIN bank, one copy per ID, IDs 418-1094)

Layout, little-endian (read by src/anim.c):
  'MOT1', u32 motion count
  21 f32 default skeleton (main.dol 0x801f9b00): spine, neck,
      arm0 (base x, J1 x, J1 z, upper, fore), arm1 (same), hip,
      leg0 (J1 x, J1 z, thigh, shin), leg1 (same)
  per motion: char name[32], u16 length, u16 pad,
      78 x (u16 key count, then keys of f32 time, value, tangent in, tangent out)
Tangents are resolved from the per-key tags here, matching the evaluator at
0x8001eb48 (tag 1 = 0.0 flat, 2 = flat, 3 = shared tangent, 0 = split).
"""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import motion as mo
from pose import default_skeleton

OUT = Path(__file__).resolve().parents[1] / 'romfs/motion'

def cook_one(blob, base, i, name):
    a, x, b = struct.unpack_from('>3I', blob, base + 4 + 12 * i)
    table = lambda k: struct.unpack_from('>f', blob, base + x + 4 * k)[0]
    chans, length = bytearray(), 0
    for c in range(78):
        k = blob[base + b + c]
        chans += struct.pack('<H', k)
        if not k:
            continue
        tp, vp = struct.unpack_from('>2I', blob, base + a + 8 * c)
        times = struct.unpack_from(f'>{k}H', blob, base + tp)
        codes = struct.unpack_from(f'>{k}H', blob, base + vp)
        length = max(length, times[-1])
        for t, code in zip(times, codes):
            tag, idx = code >> 14, code & 0x3FFF
            value = 0.0 if tag == 1 else table(idx)
            tin = table(idx + 1) if tag in (0, 3) else 0.0
            tout = table(idx + (2 if tag == 0 else 1)) if tag in (0, 3) else 0.0
            chans += struct.pack('<4f', t, value, tin, tout)
    return name.encode('ascii')[:31].ljust(32, b'\0') + struct.pack('<HH', length, 0) + chans

def write(filename, entries, defaults):
    """entries: [(blob, bank base, index, name)]"""
    body = b''.join(cook_one(*e) for e in entries)
    data = b'MOT1' + struct.pack('<I', len(entries)) + struct.pack('<21f', *defaults) + body
    (OUT / filename).write_bytes(data)
    print(f'{len(entries)} motions, {len(data) // 1024} KB -> {OUT / filename}')

def main():
    names = mo.motion_names()
    ds = default_skeleton()
    defaults = [ds['spine'], ds['neck']]
    for base_x, j1, upper, fore in ds['arm']:
        defaults += [base_x, j1[0], j1[2], upper, fore]
    defaults.append(ds['hip'])
    for j1, thigh, shin in ds['leg']:
        defaults += [j1[0], j1[2], thigh, shin]
    OUT.mkdir(parents=True, exist_ok=True)

    dbn = (mo.FILES / 'motion.DBN').read_bytes()
    bases = mo.dbn_banks(dbn)
    robot = []
    for bank in mo.ROBOT_BANKS:
        for i, ident in enumerate(mo.bank_ids(dbn, bases[bank])):
            robot.append((dbn, bases[bank], i, names.get(ident & 0x7FFF, f'motion_{ident & 0x7FFF:04d}')))
    write('robot.mot', robot, defaults)

    story = [(blob, base, i, names.get(ident, f'motion_{ident:04d}'))
             for ident, (blob, base, i) in mo.story_index().items()]
    write('story.mot', story, defaults)

if __name__ == '__main__':
    main()

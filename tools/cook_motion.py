#!/usr/bin/env python3
"""Cook every robot motion into romfs/motion/robot.mot for the 3DS.

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

def main():
    blob = (mo.FILES / 'motion.DBN').read_bytes()
    names = mo.motion_names()
    ds = default_skeleton()
    body = bytearray()
    count = 0
    for bank, base in enumerate(mo.dbn_banks(blob)):
        n = struct.unpack_from('>I', blob, base)[0]
        start = mo.BANK_NAME_START[bank]
        for i in range(n):
            a, x, b = struct.unpack_from('>3I', blob, base + 4 + 12 * i)
            table = lambda k: struct.unpack_from('>f', blob, base + x + 4 * k)[0]
            name = names[start + i] if start is not None and names else f'bank{bank}_{i:03d}'
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
            body += name.encode('ascii')[:31].ljust(32, b'\0') + struct.pack('<HH', length, 0) + chans
            count += 1
    defaults = [ds['spine'], ds['neck']]
    for base_x, j1, upper, fore in ds['arm']:
        defaults += [base_x, j1[0], j1[2], upper, fore]
    defaults.append(ds['hip'])
    for j1, thigh, shin in ds['leg']:
        defaults += [j1[0], j1[2], thigh, shin]
    OUT.mkdir(parents=True, exist_ok=True)
    data = b'MOT1' + struct.pack('<I', count) + struct.pack('<21f', *defaults) + bytes(body)
    (OUT / 'robot.mot').write_bytes(data)
    print(f'{count} motions, {len(data) // 1024} KB -> {OUT / "robot.mot"}')

if __name__ == '__main__':
    main()

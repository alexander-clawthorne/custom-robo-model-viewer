#!/usr/bin/env python3
"""Cook decoded models into the viewer's .crm files (romfs/).

    python tools/cook.py            # every robot and arena
    python tools/cook.py --only chara:0 bg:4

.crm layout, little-endian (read by src/model.c):
  'CRM1', u32 texture count, u32 batch count, f32 centre[3], f32 radius
  per texture: u16 width, u16 height, u8 GPU_TEXCOLOR, u8 wrap_s, u8 wrap_t,
               u8 pad, u32 bytes, then PICA-tiled pixels
  per batch:   s32 texture (-1 none), u32 flags (1 lit, 2 blend, 4 no z
               write, 8 alpha test), u32 vertex count, then vertices:
               f32 pos[3], f32 uv[2], f32 normal[3], u8 rgba[4]
Batches are ordered opaque first, then blended.
"""
import argparse, json, math, struct, sys
from pathlib import Path
import numpy as np
from PIL import Image
sys.path.insert(0, str(Path(__file__).parent))
import gxtex, hsd
from models import FILES, load_model

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'romfs'
GPU_RGBA8, GPU_RGB565, GPU_RGBA5551, GPU_RGBA4 = 0, 3, 2, 4
WRAP = {0: 0, 1: 2, 2: 3}  # GX clamp/repeat/mirror -> GPU_CLAMP_TO_EDGE/REPEAT/MIRRORED_REPEAT
MAX_TEX = 256
LIT, BLEND, NO_ZWRITE, ALPHA_TEST = 1, 2, 4, 8

def pow2(n):
    """PICA textures need power-of-two sides (8..1024); cap at MAX_TEX for memory."""
    p = 8
    while p < n:
        p *= 2
    return min(p, MAX_TEX)

def morton(x, y):
    m = 0
    for i in range(3):
        m |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1)
    return m

MORTON = np.array([[morton(x, y) for x in range(8)] for y in range(8)])

def tile(img):
    """(h, w, c) image, top row first -> PICA 8x8 Morton tiles, bottom row first."""
    h, w = img.shape[:2]
    flipped = img[::-1]
    out = np.empty((h // 8, w // 8, 64) + img.shape[2:], img.dtype)
    tiles = flipped.reshape(h // 8, 8, w // 8, 8, *img.shape[2:]).swapaxes(1, 2)
    # Tile rows are stored bottom-up already; within a tile, Morton order
    # counts y upward from the tile's bottom row.
    for y in range(8):
        for x in range(8):
            out[:, :, MORTON[y, x]] = tiles[:, :, y, x]
    return out.reshape(-1, *img.shape[2:])

def encode_texture(rgba):
    h, w = rgba.shape[:2]
    tw, th = pow2(w), pow2(h)
    if (tw, th) != (w, h):
        rgba = np.asarray(Image.fromarray(rgba).resize((tw, th), Image.BILINEAR))
    a = rgba[..., 3]
    r, g, b = (rgba[..., i].astype(np.uint16) for i in range(3))
    if (a == 255).all():
        fmt, px = GPU_RGB565, (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
    elif np.isin(a, (0, 255)).all():
        fmt, px = GPU_RGBA5551, (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >> 7)
    else:
        fmt, px = GPU_RGBA4, (r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | (a.astype(np.uint16) >> 4)
    return tw, th, fmt, tile(px.astype('<u2')).tobytes()

def pick_texture(material):
    """First UV-mapped diffuse texture (TObj flags: coord bits 0-3, lightmap 0x10)."""
    for t in material['textures']:
        if t['flags'] & 0xF == 0 and t['flags'] & 0x10:
            return t
    return next((t for t in material['textures'] if t['flags'] & 0xF == 0), None)

def cook(archive, index):
    d, joints, meshes = load_model(archive, index)
    textures, tex_index, batches = [], {}, []
    for mesh in meshes:
        if not mesh['tris']:
            continue
        mat = mesh['material']
        mode = mat.get('render_mode', 0)
        tex = pick_texture(mat)
        ti = -1
        if tex is not None:
            key = (tex['data'], tex['wrap'])
            if key not in tex_index:
                tlut = None
                if 'tlut' in tex:
                    p, f, n = tex['tlut']
                    tlut = (d.data[p:p + n * 2], f, n)
                rgba = gxtex.decode(d.data[tex['data']:], tex['format'], tex['width'], tex['height'], tlut)
                tex_index[key] = len(textures)
                textures.append((encode_texture(rgba), tex['wrap']))
            ti = tex_index[key]
            rs = tex['repeat'][0] / (tex['scale'][0] or 1.0)
            rt = tex['repeat'][1] / (tex['scale'][1] or 1.0)
        diffuse = mat.get('diffuse', (255, 255, 255, 255))
        alpha = mat.get('alpha', 1.0)
        flags = (LIT if mode & 0x4 else 0)
        if mode & 0x40000000:
            flags |= BLEND | NO_ZWRITE
        elif ti >= 0:
            flags |= ALPHA_TEST
        verts = bytearray()
        for tri in mesh['tris']:
            for v in tri:
                c = v['clr'] if mode & 0x2 else (1.0, 1.0, 1.0, 1.0)
                if ti < 0 and not mode & 0x2:
                    c = tuple(x / 255 for x in diffuse[:3]) + (1.0,)
                a = c[3]
                if mode & 0x2000:
                    a = alpha if not mode & 0x4000 else a * alpha
                rgba = [int(max(0, min(1, x)) * 255 + 0.5) for x in (*c[:3], a)]
                u, t = v['uv']
                if ti >= 0:
                    u, t = u * rs, t * rt
                verts += struct.pack('<8f4B', *v['pos'], u, 1.0 - t, *v['nrm'], *rgba)
        batches.append((ti, flags, len(mesh['tris']) * 3, bytes(verts)))
    batches.sort(key=lambda b: bool(b[1] & BLEND))
    pts = np.array([v['pos'] for m in meshes for t in m['tris'] for v in t], np.float32)
    # Frame the play area, not the skybox: arenas span about +-8 units, skies +-500.
    if archive == 'bg':
        near = pts[np.abs(pts).max(1) < 50]
        pts = near if len(near) else pts
    centre = (pts.min(0) + pts.max(0)) / 2
    radius = float(np.linalg.norm(pts - centre, axis=1).max())
    out = bytearray(b'CRM1' + struct.pack('<II4f', len(textures), len(batches), *centre, radius))
    for (w, h, fmt, pixels), wrap in textures:
        out += struct.pack('<HHBBBBI', w, h, fmt, WRAP.get(wrap[0], 2), WRAP.get(wrap[1], 2), 0, len(pixels)) + pixels
    for ti, flags, count, verts in batches:
        out += struct.pack('<iII', ti, flags, count) + verts
    tris = sum(b[2] for b in batches) // 3
    return bytes(out), {'triangles': tris, 'textures': len(textures), 'radius': round(radius, 3)}

def main():
    a = argparse.ArgumentParser()
    a.add_argument('--only', nargs='*', help='archive:index pairs, e.g. chara:0 bg:4')
    args = a.parse_args()
    if args.only:
        targets = [(s.split(':')[0], int(s.split(':')[1])) for s in args.only]
    else:
        robots = len(hsd.sfd_entries((FILES / 'chara.BIN').read_bytes()))
        arenas = len(hsd.sfd_entries((FILES / 'bg_data.BIN').read_bytes())) // 2
        targets = [('chara', i) for i in range(robots)] + [('bg', i) for i in range(arenas)]
    index = {'chara': [], 'bg': []}
    for archive, i in targets:
        blob, info = cook(archive, i)
        folder = OUT / ('robo' if archive == 'chara' else 'arena')
        folder.mkdir(parents=True, exist_ok=True)
        (folder / f'{i:03d}.crm').write_bytes(blob)
        index[archive].append(i)
        print(f'{archive}:{i} {len(blob) // 1024} KB {info}', flush=True)
    # index.txt lists the cooked files, so a partial cook (--only) still loads.
    for folder in ('robo', 'arena'):
        if (OUT / folder).exists():
            names = sorted(p.name for p in (OUT / folder).glob('*.crm'))
            (OUT / folder / 'index.txt').write_bytes(b''.join(n.encode('ascii') + b'\n' for n in names))
            (OUT / folder / 'count.txt').unlink(missing_ok=True)

if __name__ == '__main__':
    main()

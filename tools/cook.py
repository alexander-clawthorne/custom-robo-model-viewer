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
    """(h, w, c) image, top row first -> PICA 8x8 Morton tiles in the same
    order tex3ds writes them (top row first; sampled with t = 1 at the top)."""
    h, w = img.shape[:2]
    out = np.empty((h // 8, w // 8, 64) + img.shape[2:], img.dtype)
    tiles = img.reshape(h // 8, 8, w // 8, 8, *img.shape[2:]).swapaxes(1, 2)
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

SKINNED = {'chara'}

def cook(archive, index):
    skinned = archive in SKINNED
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
        def vertex(v, skinned):
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
            pos, nrm = (v['lpos'], v['lnrm']) if skinned else (v['pos'], v['nrm'])
            return struct.pack('<8f4B', *pos, u, 1.0 - t, *nrm, *rgba)
        if not skinned:
            verts = b''.join(vertex(v, False) for tri in mesh['tris'] for v in tri)
            batches.append((ti, flags, len(mesh['tris']) * 3, verts, -1, b''))
            continue
        # Skinned: one batch per binding. Rigid triangles keep joint-local
        # vertices; envelope triangles keep model-space vertices plus up to
        # four (joint, weight) pairs per vertex.
        groups = {}
        for tri in mesh['tris']:
            kind, b = tri[0]['bind']
            key = b if kind == 'joint' else 'env'
            groups.setdefault(key, []).append(tri)
        for key, tris in groups.items():
            verts = b''.join(vertex(v, True) for tri in tris for v in tri)
            weights = b''
            if key == 'env':
                for tri in tris:
                    for v in tri:
                        kind, b = v['bind']
                        pairs = list(b) if kind == 'env' else [(b, 1.0)]
                        pairs = (pairs + [(0, 0.0)] * 4)[:4]
                        weights += struct.pack('<4B4f', *(j for j, _ in pairs), *(w for _, w in pairs))
            batches.append((ti, flags, len(tris) * 3, verts, -1 if key == 'env' else key, weights))
    batches.sort(key=lambda b: bool(b[1] & BLEND))
    pts = np.array([v['pos'] for m in meshes for t in m['tris'] for v in t], np.float32)
    # Frame the play area, not the skybox: arenas span about +-8 units, skies +-500.
    if archive == 'bg':
        near = pts[np.abs(pts).max(1) < 50]
        pts = near if len(near) else pts
    centre = (pts.min(0) + pts.max(0)) / 2
    radius = float(np.linalg.norm(pts - centre, axis=1).max())
    out = bytearray((b'CRM2' if skinned else b'CRM1') + struct.pack('<II4f', len(textures), len(batches), *centre, radius))
    for (w, h, fmt, pixels), wrap in textures:
        out += struct.pack('<HHBBBBI', w, h, fmt, WRAP.get(wrap[0], 2), WRAP.get(wrap[1], 2), 0, len(pixels)) + pixels
    if skinned:
        out += skeleton_block(archive, index, joints)
    for ti, flags, count, verts, joint, weights in batches:
        if skinned:
            out += struct.pack('<iIiI', ti, flags, joint, count) + verts + weights
        else:
            out += struct.pack('<iII', ti, flags, count) + verts
    tris = sum(b[2] for b in batches) // 3
    return bytes(out), {'triangles': tris, 'textures': len(textures), 'radius': round(radius, 3)}

def skeleton_block(archive, index, joints):
    """CRM2 skeleton: joints, rig slots and the robot's 32 dimension floats.
    Per joint: s16 parent, u8 IK type (1 JOINT1, 2 JOINT2, 3 EFFECTOR), u8 pad,
    f32 rot[3] (radians), scale[3], trans[3], IK bone length, inverse bind 3x4."""
    from pose import rig
    out = struct.pack('<I', len(joints))
    for j in joints:
        inv = np.linalg.inv(np.array(j.world))[:3, :].reshape(-1)
        out += struct.pack('<hBB', j.parent, j.ik, 0) + struct.pack('<10f', *j.rot, *j.scale, *j.trans, j.bone_length)
        out += struct.pack('<12f', *inv)
    dims, paths = rig(archive, index)
    d, _, _ = load_model(archive, index)
    by_off = {jj.off: jj.index for jj in joints}
    root = list(d.roots.values())[0]
    slots = []
    for path in paths:
        off = root
        for c in path:
            off = d.ptr(off + 8) if c == 2 else d.ptr(off + 12)
        slots.append(by_off.get(off, -1))
    out += struct.pack('<19h', *slots) + b'\x00\x00' + struct.pack('<32f', *dims)
    return out

# (archive, romfs folder, label shown in the viewer), in viewer order.
CATEGORIES = [('chara', 'robo', 'Robot'), ('gun', 'gun', 'Gun'), ('bom', 'bomb', 'Bomb'),
              ('pod', 'pod', 'Pod'), ('leg', 'leg', 'Legs'), ('bg', 'arena', 'Arena'),
              ('snrchara', 'snrchara', 'Story character'), ('snrobj', 'snrobj', 'Story object')]

def entry_count(archive):
    from models import ROBO_ARCHIVES, SCENE_ARCHIVES
    if archive in ROBO_ARCHIVES:
        return len(hsd.sfd_entries((FILES / ROBO_ARCHIVES[archive]).read_bytes()))
    name, stride = SCENE_ARCHIVES[archive]
    return len(hsd.sfd_entries((FILES / name).read_bytes())) // stride

def main():
    a = argparse.ArgumentParser()
    a.add_argument('--only', nargs='*', help='archive:index pairs, e.g. chara:0 bg:4')
    args = a.parse_args()
    folders = {arc: folder for arc, folder, _ in CATEGORIES}
    if args.only:
        targets = [(s.split(':')[0], int(s.split(':')[1])) for s in args.only]
    else:
        targets = [(arc, i) for arc, _, _ in CATEGORIES for i in range(entry_count(arc))]
    for archive, i in targets:
        blob, info = cook(archive, i)
        folder = OUT / folders[archive]
        folder.mkdir(parents=True, exist_ok=True)
        (folder / f'{i:03d}.crm').write_bytes(blob)
        print(f'{archive}:{i} {len(blob) // 1024} KB {info}', flush=True)
    # index.txt lists the cooked files, so a partial cook (--only) still loads;
    # categories.txt lists the folders the viewer cycles through with Y.
    present = []
    for _, folder, label in CATEGORIES:
        if (OUT / folder).exists():
            names = sorted(p.name for p in (OUT / folder).glob('*.crm'))
            (OUT / folder / 'index.txt').write_bytes(b''.join(n.encode('ascii') + b'\n' for n in names))
            if names:
                present.append(f'{folder}|{label}')
    (OUT / 'categories.txt').write_bytes(''.join(line + '\n' for line in present).encode('ascii'))

if __name__ == '__main__':
    main()

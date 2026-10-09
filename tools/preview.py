#!/usr/bin/env python3
"""Software-render a decoded model to PNG, to check the decoders on the PC.

    python tools/preview.py chara 0 build/preview/chara_000.png
    python tools/preview.py bg 0 build/preview/bg_000.png

Front, side and back views, textured, with simple lighting.
"""
import argparse, math, sys
from pathlib import Path
import numpy as np
from PIL import Image
sys.path.insert(0, str(Path(__file__).parent))
import gxtex
from models import load_model

def texture_image(d, tex, cache):
    key = tex['data']
    if key not in cache:
        try:
            tlut = None
            if 'tlut' in tex:
                p, f, n = tex['tlut']
                tlut = (d.data[p:p + n * 2], f, n)
            cache[key] = gxtex.decode(d.data[tex['data']:], tex['format'], tex['width'], tex['height'], tlut).astype(np.float32) / 255
        except Exception as e:  # keep rendering; report unknown formats
            print('texture', hex(tex['data']), 'format', tex['format'], 'failed:', e)
            cache[key] = None
    return cache[key]

def render(d, meshes, yaw, size=512, pitch=0.25):
    tris = [(m, t) for m in meshes for t in m['tris']]
    pts = np.array([v['pos'] for _, t in tris for v in t], np.float32)
    lo, hi = pts.min(0), pts.max(0)
    centre, radius = (lo + hi) / 2, float(np.linalg.norm(hi - lo)) / 2 or 1.0
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    R = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]], np.float32)
    R = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]], np.float32) @ R
    img = np.full((size, size, 3), (0.18, 0.2, 0.24), np.float32)
    zbuf = np.full((size, size), np.inf, np.float32)
    cache = {}
    light = np.array([0.4, 0.7, 0.6], np.float32); light /= np.linalg.norm(light)
    scale = size * 0.45 / radius
    for mesh, tri in tris:
        p = (np.array([v['pos'] for v in tri], np.float32) - centre) @ R.T
        sx = size / 2 + p[:, 0] * scale
        syy = size / 2 - p[:, 1] * scale
        z = -p[:, 2]
        x0, x1 = int(max(0, math.floor(sx.min()))), int(min(size - 1, math.ceil(sx.max())))
        y0, y1 = int(max(0, math.floor(syy.min()))), int(min(size - 1, math.ceil(syy.max())))
        if x0 > x1 or y0 > y1:
            continue
        area = (sx[1] - sx[0]) * (syy[2] - syy[0]) - (sx[2] - sx[0]) * (syy[1] - syy[0])
        if abs(area) < 1e-9:
            continue
        X, Y = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w0 = ((sx[1] - X) * (syy[2] - Y) - (sx[2] - X) * (syy[1] - Y)) / area
        w1 = ((sx[2] - X) * (syy[0] - Y) - (sx[0] - X) * (syy[2] - Y)) / area
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        depth = w0 * z[0] + w1 * z[1] + w2 * z[2]
        zb = zbuf[y0:y1 + 1, x0:x1 + 1]
        mask = inside & (depth < zb)
        if not mask.any():
            continue
        n = np.array([v['nrm'] for v in tri], np.float32) @ R.T
        nrm = n.sum(0); nrm /= np.linalg.norm(nrm) or 1
        shade = 0.45 + 0.55 * abs(float(nrm @ light))
        col = np.array([v['clr'][:3] for v in tri], np.float32)
        c = w0[..., None] * col[0] + w1[..., None] * col[1] + w2[..., None] * col[2]
        texs = mesh['material']['textures']
        tex = texture_image(d, texs[0], cache) if texs else None
        if tex is not None:
            uv = np.array([v['uv'] for v in tri], np.float32)
            u = w0 * uv[0, 0] + w1 * uv[1, 0] + w2 * uv[2, 0]
            v = w0 * uv[0, 1] + w1 * uv[1, 1] + w2 * uv[2, 1]
            th, tw = tex.shape[:2]
            tx = (np.floor(u * tw).astype(int)) % tw
            ty = (np.floor(v * th).astype(int)) % th
            texel = tex[ty, tx]
            mask &= texel[..., 3] > 0.3
            c = c * texel[..., :3]
        elif 'diffuse' in mesh['material']:
            c = c * np.array(mesh['material']['diffuse'][:3], np.float32) / 255
        zb[mask] = depth[mask]
        img[y0:y1 + 1, x0:x1 + 1][mask] = (c * shade)[mask]
    return (np.clip(img, 0, 1) * 255).astype(np.uint8)

def main():
    a = argparse.ArgumentParser()
    a.add_argument('archive'); a.add_argument('index', type=int); a.add_argument('out', type=Path)
    a.add_argument('--size', type=int, default=384)
    args = a.parse_args()
    d, joints, meshes = load_model(args.archive, args.index)
    ntri = sum(len(m['tris']) for m in meshes)
    print(f'{args.archive}[{args.index}]: {len(joints)} joints, {len(meshes)} meshes, {ntri} triangles')
    views = [render(d, meshes, yaw, args.size) for yaw in (0.0, math.pi / 2, math.pi)]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.concatenate(views, 1)).save(args.out)
    print('wrote', args.out)

if __name__ == '__main__':
    main()

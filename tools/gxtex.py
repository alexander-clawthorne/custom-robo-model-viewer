"""Decode GameCube GX textures to RGBA8 numpy arrays (height, width, 4).

Formats and tile layouts per the GX texture documentation (YAGCD, Dolphin
TextureDecoder): I4 0, I8 1, IA4 2, IA8 3, RGB565 4, RGB5A3 5, RGBA8 6,
CI4 8, CI8 9, CMPR 14. Palettes: IA8 0, RGB565 1, RGB5A3 2.
"""
import numpy as np

TILE = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16), 4: (4, 4, 16), 5: (4, 4, 16),
        6: (4, 4, 32), 8: (8, 8, 4), 9: (8, 4, 8), 14: (8, 8, 4)}

def size(fmt, w, h):
    tw, th, bpp = TILE[fmt]
    return ((w + tw - 1) // tw * tw) * ((h + th - 1) // th * th) * bpp // 8

def _rgb565(v):
    r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), np.full_like(v, 255)], -1)

def _rgb5a3(v):
    opaque = (v & 0x8000) != 0
    r = np.where(opaque, ((v >> 10) & 31) * 255 // 31, ((v >> 8) & 15) * 17)
    g = np.where(opaque, ((v >> 5) & 31) * 255 // 31, ((v >> 4) & 15) * 17)
    b = np.where(opaque, (v & 31) * 255 // 31, (v & 15) * 17)
    a = np.where(opaque, 255, ((v >> 12) & 7) * 255 // 7)
    return np.stack([r, g, b, a], -1)

def _ia8(v):
    i = v & 0xFF; a = v >> 8
    return np.stack([i, i, i, a], -1)

PALETTE = {0: _ia8, 1: _rgb565, 2: _rgb5a3}

def _untile(blocks, w, h, tw, th):
    """blocks: (n_tiles, th, tw, 4) in tile order -> (h, w, 4)."""
    bw, bh = (w + tw - 1) // tw, (h + th - 1) // th
    img = blocks.reshape(bh, bw, th, tw, 4).transpose(0, 2, 1, 3, 4).reshape(bh * th, bw * tw, 4)
    return img[:h, :w]

def decode(buf, fmt, w, h, tlut=None):
    tw, th, bpp = TILE[fmt]
    raw = np.frombuffer(buf, np.uint8, size(fmt, w, h))
    if fmt in (0, 8):  # 4-bit
        v = np.stack([raw >> 4, raw & 15], -1).reshape(-1).astype(np.int32)
    elif fmt in (1, 2, 9):
        v = raw.astype(np.int32)
    elif fmt in (3, 4, 5):
        v = raw.view('>u2').astype(np.int32)
    if fmt == 0:
        i = v * 17; px = np.stack([i, i, i, i], -1)
    elif fmt == 1:
        px = np.stack([v, v, v, v], -1)
    elif fmt == 2:
        i = (v & 15) * 17; a = (v >> 4) * 17; px = np.stack([i, i, i, a], -1)
    elif fmt == 3:
        px = _ia8(v)
    elif fmt == 4:
        px = _rgb565(v)
    elif fmt == 5:
        px = _rgb5a3(v)
    elif fmt in (8, 9):
        pal_data, pal_fmt, count = tlut
        pal = PALETTE[pal_fmt](np.frombuffer(pal_data, '>u2', count).astype(np.int32))
        px = pal[np.minimum(v, count - 1)]
    elif fmt == 6:
        t = raw.reshape(-1, 2, 16, 2)  # tile: 16 AR pairs, then 16 GB pairs
        a, r = t[:, 0, :, 0], t[:, 0, :, 1]
        g, b = t[:, 1, :, 0], t[:, 1, :, 1]
        px = np.stack([r, g, b, a], -1).reshape(-1, 4).astype(np.int32)
    elif fmt == 14:
        return _cmpr(raw, w, h)
    else:
        raise ValueError(f'texture format {fmt}')
    return _untile(px.reshape(-1, th, tw, 4), w, h, tw, th).astype(np.uint8)

def _cmpr(raw, w, h):
    sub = raw.reshape(-1, 8)  # 4x4 DXT1 sub-blocks, 4 per 8x8 tile (Z order)
    c0 = sub[:, 0].astype(np.int32) << 8 | sub[:, 1]
    c1 = sub[:, 2].astype(np.int32) << 8 | sub[:, 3]
    p0, p1 = _rgb565(c0), _rgb565(c1)
    four = (c0 > c1)[:, None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    p3[:, 3] = np.where(four[:, 0], 255, 0)
    pal = np.stack([p0, p1, p2, p3], 1)  # (n, 4, 4)
    bits = sub[:, 4:8].astype(np.int32)
    idx = np.stack([(bits >> s) & 3 for s in (6, 4, 2, 0)], -1)  # (n, 4 rows, 4 cols)
    px = pal[np.arange(len(sub))[:, None, None], idx]  # (n, row, col, rgba)
    px = px.reshape(-1, 2, 2, 4, 4, 4)  # tile, sy, sx, row, col, rgba
    tiles = px.transpose(0, 1, 3, 2, 4, 5).reshape(-1, 8, 8, 4)
    return _untile(tiles, w, h, 8, 8).astype(np.uint8)

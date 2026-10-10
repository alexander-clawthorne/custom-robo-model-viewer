"""HAL sysdolphin (HSD) data reader for Custom Robo's disc files.

Containers:
  SFD    'SFD\\0', u32 count, then count x (u32 offset, u32 size).
  HSD    standard archive (.dat, bg_data.BIN even entries): 0x20 header
         (file size, data size, reloc count, root count, ref count), data
         block, relocation table, root/ref tables, string table.
  Robo   Custom Robo's own wrapper, data block from 0x40, pointers are
         data-block offsets. Two header variants:
         body   (chara.BIN, snr_chara.BIN): [2] data offset of the root
                pointer table (relocated words -> ModelSets; [5] is not a
                reliable count: three characters say 3 with one root),
                [6] FILE offset of the relocation table, [7] its count.
         parts  (weapon_*.BIN, [5] == -1): [2] data offset of [3] root
                records of 16 bytes (word 0 -> a JObj), [6] DATA offset of
                the relocation table, [7] its count.
Object layouts follow the HSD structures documented by the Melee
decompilation (doldecomp/melee, src/sysdolphin/baselib) and HSDLib.
"""
import math
import struct

def sfd_entries(blob):
    if blob[:4] != b'SFD\0':
        raise ValueError('not an SFD archive')
    count = struct.unpack_from('>I', blob, 4)[0]
    return [struct.unpack_from('>II', blob, 8 + i * 8) for i in range(count)]

class Data:
    """A data block plus the set of offsets that hold pointers."""
    def __init__(self, data, relocs, roots):
        self.data, self.relocs, self.roots = data, relocs, roots
    def u8(self, o): return self.data[o]
    def u16(self, o): return struct.unpack_from('>H', self.data, o)[0]
    def u32(self, o): return struct.unpack_from('>I', self.data, o)[0]
    def f32(self, o): return struct.unpack_from('>f', self.data, o)[0]
    def vec3(self, o): return struct.unpack_from('>3f', self.data, o)
    def ptr(self, o):
        """Pointer stored at o, or None for a null (unrelocated) word."""
        return self.u32(o) if o in self.relocs else None

def read_hsd(blob):
    fs, ds, rc, rtc, refc = struct.unpack_from('>5I', blob, 0)
    if fs > len(blob) or ds + 0x20 > fs:
        raise ValueError('not an HSD archive')
    at = 0x20 + ds
    relocs = set(struct.unpack_from(f'>{rc}I', blob, at))
    tables = at + rc * 4
    strings = tables + (rtc + refc) * 8
    roots = {}
    for i in range(rtc):
        off, so = struct.unpack_from('>II', blob, tables + i * 8)
        s = strings + so
        roots[blob[s:blob.index(0, s)].decode('ascii', 'replace')] = off
    return Data(blob[0x20:at], relocs, roots)

def read_robo(blob):
    """Returns Data whose roots map 'jobjN' to JObj offsets."""
    h = struct.unpack_from('>16I', blob, 0)
    if h[0] != 0x40:
        raise ValueError('not a Custom Robo model archive')
    data = blob[0x40:]
    if h[5] == 0xFFFFFFFF:
        relocs = set(struct.unpack_from(f'>{h[7]}I', data, h[6]))
        roots = {f'jobj{i}': struct.unpack_from('>I', data, h[2] + 16 * i)[0] for i in range(h[3])}
    else:
        relocs = set(struct.unpack_from(f'>{h[7]}I', blob, h[6]))
        roots = {}
        for i in range(max(1, h[5])):
            if h[2] + 4 * i not in relocs:
                continue
            model_set = struct.unpack_from('>I', data, h[2] + 4 * i)[0]
            roots[f'jobj{i}'] = struct.unpack_from('>I', data, model_set)[0]
    return Data(data, relocs, roots)

# ---------------------------------------------------------------- math
def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]

def srt(s, r, t):
    """HSD joint matrix: T * Rz * Ry * Rx * S."""
    cx, sx = math.cos(r[0]), math.sin(r[0])
    cy, sy = math.cos(r[1]), math.sin(r[1])
    cz, sz = math.cos(r[2]), math.sin(r[2])
    R = [[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
         [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
         [-sy, sx * cy, cx * cy]]
    return [[R[i][0] * s[0], R[i][1] * s[1], R[i][2] * s[2], t[i]] for i in range(3)] + [[0, 0, 0, 1]]

def xform(m, v, w=1.0):
    return tuple(m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3] * w for i in range(3))

IDENTITY = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]

# ---------------------------------------------------------------- GX
GX_VA_PNMTXIDX, GX_VA_POS, GX_VA_NRM, GX_VA_CLR0, GX_VA_TEX0 = 0, 9, 10, 11, 13
GX_DIRECT, GX_INDEX8, GX_INDEX16 = 1, 2, 3
PRIMS = {0x80: 'quads', 0x90: 'triangles', 0x98: 'strip', 0xA0: 'fan'}
COMP = {0: ('B', 1), 1: ('b', 1), 2: ('H', 2), 3: ('h', 2), 4: ('f', 4)}

def read_component(d, o, attr, cnt, ctype, frac):
    if attr in (GX_VA_CLR0, GX_VA_CLR0 + 1):
        if ctype == 5:  # RGBA8
            return tuple(x / 255 for x in d.data[o:o + 4])
        if ctype in (1, 2):  # RGB8 / RGBX8
            return tuple(x / 255 for x in d.data[o:o + 3]) + (1.0,)
        if ctype == 3:  # RGBA4
            v = d.u16(o)
            return tuple(((v >> s) & 15) / 15 for s in (12, 8, 4, 0))
        if ctype == 0:  # RGB565
            v = d.u16(o)
            return ((v >> 11) / 31, ((v >> 5) & 63) / 63, (v & 31) / 31, 1.0)
        if ctype == 4:  # RGBA6
            v = (d.data[o] << 16) | (d.data[o + 1] << 8) | d.data[o + 2]
            return tuple(((v >> s) & 63) / 63 for s in (18, 12, 6, 0))
        raise ValueError(f'colour format {ctype}')
    n = {GX_VA_POS: 2 + cnt, GX_VA_NRM: 3}.get(attr, 1 + cnt)
    fmt, size = COMP[ctype]
    vals = struct.unpack_from('>' + fmt * n, d.data, o)
    if fmt != 'f':
        vals = tuple(v / (1 << frac) for v in vals)
    return vals

def vertex_attrs(d, at):
    attrs = []
    while True:
        attr, atype, cnt, ctype = struct.unpack_from('>4I', d.data, at)
        if attr == 0xFF:
            return attrs
        frac, stride = d.u8(at + 0x10), d.u16(at + 0x12)
        attrs.append((attr, atype, cnt, ctype, frac, stride, d.ptr(at + 0x14)))
        at += 0x18

def read_display_list(d, pobj):
    """Yield triangles; each vertex is a dict of attribute -> value."""
    attrs = vertex_attrs(d, d.ptr(pobj + 0x08))
    at = d.ptr(pobj + 0x10)
    end = at + d.u16(pobj + 0x0E) * 32
    while at < end:
        op = d.u8(at)
        if op == 0:
            at += 1
            continue
        kind = PRIMS.get(op & 0xF8)
        if not kind:
            break
        count = d.u16(at + 1)
        at += 3
        verts = []
        for _ in range(count):
            v = {}
            for attr, atype, cnt, ctype, frac, stride, base in attrs:
                if atype == GX_DIRECT:
                    if attr <= 8:
                        v[attr] = d.u8(at); at += 1
                        continue
                    raise ValueError('direct vertex data not supported')
                if atype == GX_INDEX8:
                    idx = d.u8(at); at += 1
                else:
                    idx = d.u16(at); at += 2
                v[attr] = read_component(d, base + idx * stride, attr, cnt, ctype, frac)
            verts.append(v)
        if kind == 'triangles':
            yield from (verts[i:i + 3] for i in range(0, len(verts) - 2, 3))
        elif kind == 'quads':
            for i in range(0, len(verts) - 3, 4):
                a, b, c, e = verts[i:i + 4]
                yield [a, b, c]; yield [a, c, e]
        elif kind == 'strip':
            for i in range(len(verts) - 2):
                tri = verts[i:i + 3]
                yield tri if i % 2 == 0 else [tri[1], tri[0], tri[2]]
        elif kind == 'fan':
            for i in range(1, len(verts) - 1):
                yield [verts[0], verts[i], verts[i + 1]]

# ---------------------------------------------------------------- scene
JOINT1, JOINT2, EFFECTOR = 1, 2, 3  # JObj flags bits 21-22

def local(d, off):
    return srt(d.vec3(off + 0x20), d.vec3(off + 0x14), d.vec3(off + 0x2C))

def ik_type(d, off):
    return (d.u32(off + 4) >> 21) & 3

def bone_length(d, off):
    """RObj IK hint {f32 bone_length, f32 rotate_x}, reached through the JObj's RObj."""
    robj = d.ptr(off + 0x3C)
    while robj is not None:
        if d.u32(robj + 4) & 0x70000000 == 0x40000000:
            return d.f32(d.ptr(robj + 8))
        robj = d.ptr(robj)
    return d.vec3(off + 0x2C)[0]

def effector_world(d, off, parent_off, parent_world):
    """HSD_JObjSetupMatrixSub, JOBJ_EFFECTOR case (Melee decomp jobj.c): when
    the parent carries an IK hint, the effector sits bone_length along the
    parent's X axis; its own translation is ignored. Chains are only solved
    (resolveIKJoint1/2) when the effector has a REFTYPE_JOBJ target, which
    Custom Robo's bind-pose models do not, so their stored rotations stand."""
    world = mat_mul(parent_world, local(d, off))
    robj = d.ptr(parent_off + 0x3C) if parent_off is not None else None
    if robj is None:
        return world
    x = _norm(tuple(parent_world[i][0] for i in range(3)))
    length = bone_length(d, parent_off)
    for i in range(3):
        world[i][3] = parent_world[i][3] + x[i] * length
    return world

def _norm(v):
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return tuple(x / n for x in v)

class Joint:
    def __init__(self, d, off, world, index, parent=-1):
        self.off, self.flags, self.world, self.index = off, d.u32(off + 4), world, index
        self.parent = parent
        self.rot, self.scale, self.trans = d.vec3(off + 0x14), d.vec3(off + 0x20), d.vec3(off + 0x2C)
        self.ik = ik_type(d, off)
        self.bone_length = bone_length(d, off) if d.ptr(off + 0x3C) is not None else 0.0

def walk_joints(d, root, mount=True):
    """All joints of a JObj tree, depth first, with bind-pose world matrices.
    mount=False drops the top-level joint's local matrix entirely. A weapon
    part stores there the rotation (and sometimes the offset) it is mounted
    at on the robot; the game never uses that joint's own SRT -- it writes
    the joint's world matrix itself and sets JOBJ_MTX_INDEPEND_PARENT|SRT
    (0x80039fac) -- so for assembly the mount matrix is applied separately
    (models.part_mounts) and viewing a part alone drops it."""
    joints, by_off = [], {}
    def visit(off, parent_off, parent_world):
        while off is not None:
            if ik_type(d, off) == EFFECTOR:
                world = effector_world(d, off, parent_off, parent_world)
            elif parent_off is None and not mount:
                world = IDENTITY
            else:
                world = mat_mul(parent_world, local(d, off))
            parent = by_off[parent_off].index if parent_off is not None else -1
            j = Joint(d, off, world, len(joints), parent)
            joints.append(j); by_off[off] = j
            child = d.ptr(off + 0x08)
            if child is not None:
                visit(child, off, j.world)
            off = d.ptr(off + 0x0C)
    visit(root, None, IDENTITY)
    return joints, by_off

def read_texture(d, tobj):
    image = d.ptr(tobj + 0x4C)
    if image is None:
        return None
    tlut = d.ptr(tobj + 0x50)
    tex = {'offset': tobj, 'data': d.ptr(image), 'width': d.u16(image + 4), 'height': d.u16(image + 6),
           'format': d.u32(image + 8), 'wrap': (d.u32(tobj + 0x34), d.u32(tobj + 0x38)),
           'repeat': (d.u8(tobj + 0x3C), d.u8(tobj + 0x3D)), 'flags': d.u32(tobj + 0x40),
           'scale': d.vec3(tobj + 0x1C)}
    if tlut is not None:
        tex['tlut'] = (d.ptr(tlut), d.u32(tlut + 4), d.u16(tlut + 0xC))
    return tex

def read_material(d, mobj):
    textures = []
    tobj = d.ptr(mobj + 0x08)
    while tobj is not None:
        t = read_texture(d, tobj)
        if t:
            textures.append(t)
        tobj = d.ptr(tobj + 0x04)
    out = {'render_mode': d.u32(mobj + 0x04), 'textures': textures}
    mat = d.ptr(mobj + 0x0C)
    if mat is not None:
        out['ambient'] = tuple(d.data[mat:mat + 4])
        out['diffuse'] = tuple(d.data[mat + 4:mat + 8])
        out['alpha'] = d.f32(mat + 0x0C)
    pe = d.ptr(mobj + 0x14)
    if pe is not None:
        out['pe'] = d.data[pe:pe + 12].hex()
    return out

def envelopes(d, at):
    out = []
    while at is not None and d.ptr(at) is not None:
        env, e = [], d.ptr(at)
        while d.ptr(e) is not None:
            env.append((d.ptr(e), d.f32(e + 4)))
            e += 8
        out.append(env)
        at += 4
    return out

# How single-joint envelope vertices are stored is checked against renders
# (tools/preview.py); 'model' leaves them untouched, 'joint' applies the joint.
SINGLE_ENVELOPE = 'model'

def read_meshes(d, root, mount=True):
    """Bind-pose triangles per DObj, with its material."""
    joints, by_off = walk_joints(d, root, mount)
    meshes = []
    for j in joints:
        if j.flags & 0x4030:  # JOBJ_SPLINE / JOBJ_PTCL reuse the DObj slot; 0x10 hidden
            continue
        dobj = d.ptr(j.off + 0x10)
        while dobj is not None:
            mobj, pobj = d.ptr(dobj + 0x08), d.ptr(dobj + 0x0C)
            material = read_material(d, mobj) if mobj is not None else {'textures': []}
            tris = []
            while pobj is not None:
                kind = d.u16(pobj + 0x0C) & 0x3000
                u = d.ptr(pobj + 0x14)
                envs = envelopes(d, u) if kind == 0x2000 else None
                bound = by_off.get(u, j) if kind == 0 and u is not None else j
                for tri in read_display_list(d, pobj):
                    out = []
                    for v in tri:
                        p = v[GX_VA_POS]
                        p = (p[0], p[1], p[2] if len(p) > 2 else 0.0)
                        n = v.get(GX_VA_NRM, (0.0, 1.0, 0.0))
                        # Skinning data for animated playback: rigid vertices keep
                        # their joint-local position; envelope vertices are in model
                        # space and blend sum(w * world_j * inverse_bind_j).
                        lp, ln = p, n
                        if envs is not None:
                            env = envs[v.get(GX_VA_PNMTXIDX, 0) // 3]
                            binding = ('env', tuple((by_off[jo].index, w) for jo, w in env))
                            if len(env) == 1 and SINGLE_ENVELOPE == 'joint':
                                m = by_off[env[0][0]].world
                                p, n = xform(m, p), xform(m, n, 0.0)
                        else:
                            binding = ('joint', bound.index)
                            m = bound.world
                            p, n = xform(m, p), xform(m, n, 0.0)
                        out.append({'pos': p, 'nrm': n, 'uv': v.get(GX_VA_TEX0, (0.0, 0.0)),
                                    'clr': v.get(GX_VA_CLR0, (1.0, 1.0, 1.0, 1.0)),
                                    'lpos': lp, 'lnrm': ln, 'bind': binding})
                    tris.append(out)
                pobj = d.ptr(pobj + 0x04)
            meshes.append({'joint': j.index, 'material': material, 'tris': tris})
            dobj = d.ptr(dobj + 0x04)
    return joints, meshes

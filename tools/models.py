"""Locate model archives on the extracted disc.

  chara N     robot body N (chara.BIN, Custom Robo wrapper)
  snrchara N  story character N (scenario_us/snr_chara.BIN, same wrapper)
  gun/bom/pod/leg N   weapon part N (weapon_<kind>.BIN, parts variant)
  bg N        arena N (bg_data.BIN entry 2N, standard HSD scene_data)
  atk/gun/bom/podfx N   effect N (*_effect.BIN; each SFD entry holds two
              standard HSD archives, so N is entry*2 + sub)
  map N       story map N (snr_map.BIN entry N's scene; collision.py reads
              the collision section of the same entry)
  rpgf/rpgt N field or town model N (rpg_f/t_models.BIN, flattened over the
              archive's entries)
  snrobj N    story map object N (scenario_us/snr_obj.BIN, HSD scene_data)
  legfx N     leg-part effect N (leg_effect.BIN, HSD scene_data)
  title       TITLE_MODEL.dat
"""
import struct
from pathlib import Path
import hsd

FILES = Path(__file__).resolve().parents[1] / 'data/extracted/files'

def entry(name, index):
    blob = (FILES / name).read_bytes()
    off, size = hsd.sfd_entries(blob)[index]
    return blob[off:off + size]

def scene_roots(d, scene):
    """scene_data -> every ModelSet's JObj (SceneDesc: ModelSet**, Camera, Lights, Fog)."""
    roots, at = [], d.ptr(scene)
    while at is not None and d.ptr(at) is not None:
        roots.append(d.ptr(d.ptr(at)))
        at += 4
    return roots

ROBO_ARCHIVES = {'chara': 'chara.BIN', 'snrchara': 'scenario_us/snr_chara.BIN',
                 'gun': 'weapon_gun.BIN', 'bom': 'weapon_bom.BIN', 'pod': 'weapon_pod.BIN', 'leg': 'weapon_leg.BIN'}
# Weapon parts. A part root joint stores an SRT (guns -90 deg about X, pods
# 90/0/90 and so on) that looks like a mount rotation and is not: the draw code
# at 0x80039fac writes the root's world matrix itself and sets JOBJ_USER_DEF_MTX
# | MTX_INDEPEND_PARENT | MTX_INDEPEND_SRT (0x3800000), so the stored SRT never
# reaches the screen. The geometry is already in the mount joint's frame, so
# these files are read with that joint's matrix dropped (hsd.walk_joints).
PART_ARCHIVES = ('gun', 'bom', 'pod', 'leg')

# Where each part root hangs off the body, read from that same function: it
# copies the robot's solved matrix out[k] into the part root's world matrix and
# scales the basis by PART_SCALE -- no rotation anywhere (the call that looked
# like one, 0x8001da70, scales each axis). Rig slot r is driven by out[r+1]
# (tools/pose.py), so slot = k - 1. Of a part's roots only these are drawn.
#   (archive, root index, body rig slot)
ATTACH = (('gun', 0, 8),    # out[9]  arm2 elbow: the gun is the forearm
          ('bom', 1, 4),    # out[5]  arm1 elbow
          ('pod', 0, 0),    # out[1]  spine, one pod each side
          ('pod', 1, 0),
          ('leg', 5, 12),   # out[13] leg1 knee
          ('leg', 3, 13),   # out[14] leg1 foot
          ('leg', 4, 16),   # out[17] leg2 knee
          ('leg', 1, 17))   # out[18] leg2 foot
# *(r2 - 0x7b18) in the same function: parts are modelled ten times robot size.
PART_SCALE = 0.1
SCENE_ARCHIVES = {'bg': ('bg_data.BIN', 2), 'snrobj': ('scenario_us/snr_obj.BIN', 1), 'legfx': ('leg_effect.BIN', 1)}
# Weapon and hit effects. Unlike leg_effect.BIN, each SFD entry is a small
# container of its own -- u32 count, then count+1 offsets, the last being the
# entry size -- holding two standard HSD archives: the projectile and its
# burst. Both carry scene_data, and their ModelSets have the AnimJoint and
# MatAnimJoint pointers the effect animates with.
# efc_common.dbn is not an SFD: the file itself is one such container.
EFFECT_ARCHIVES = {'atkfx': 'attack_effect.BIN', 'gunfx': 'gun_effect.BIN',
                   'bomfx': 'bomb_effect.BIN', 'podfx': 'pod_effect.BIN',
                   'efcfx': 'efc_common.dbn'}

def effect_subs(name, index):
    """Every HSD archive inside the container at entry `index`."""
    blob = (FILES / name).read_bytes() if name.endswith('.dbn') else entry(name, index)
    count = struct.unpack_from('>I', blob, 0)[0]
    offs = struct.unpack_from(f'>{count + 1}I', blob, 4)
    return [blob[offs[k]:offs[k + 1]] for k in range(count)]

# snr_map.BIN entry header: the collision section (collision.py), the map's
# own HSD archive, the x and z extents, then an optional trailing container
# of further HSD scenes. A zero offset means that section is absent.
def map_sections(index, name='scenario_us/snr_map.BIN'):
    blob = entry(name, index)
    coll, scene, tail = (*struct.unpack_from('>2I', blob, 0),
                         struct.unpack_from('>I', blob, 0x18)[0])
    bbox = struct.unpack_from('>4f', blob, 8)
    return {'blob': blob, 'collision': (coll, scene) if coll else None,
            'scene': (scene, tail or len(blob)), 'bbox': bbox,
            'extra': tail or None}

def map_extra_scenes(index):
    """The trailing container's further scenes, if the map has any."""
    s = map_sections(index)
    if not s['extra']:
        return []
    x = s['blob'][s['extra']:]
    table = struct.unpack_from('>I', x, 4)[0]
    out, k = [], 0
    while struct.unpack_from('>I', x, table + k * 4)[0]:
        out.append(hsd.read_hsd(x[struct.unpack_from('>I', x, table + k * 4)[0]:]))
        k += 1
    return out

# rpg_f/t_models.BIN: u32 count, then count (u32 id, u32 offset) pairs. An id
# with its high half set is a field or town model of its own; the rest are
# byte-for-byte copies of snr_chara.BIN entries, bundled per location, and the
# id IS the story-character index -- so they are already cooked as `snrchara`.
RPG_ARCHIVES = {'rpgf': 'scenario_us/rpg_f_models.BIN', 'rpgt': 'scenario_us/rpg_t_models.BIN'}

def rpg_parts(name):
    """(entry, id, blob) for every sub-resource in the archive."""
    blob = (FILES / name).read_bytes()
    out = []
    for i, (o, size) in enumerate(hsd.sfd_entries(blob)):
        d = blob[o:o + size]
        count = struct.unpack_from('>I', d, 0)[0]
        if not count or 4 + 8 * count > size:
            continue
        pairs = [struct.unpack_from('>2I', d, 4 + 8 * k) for k in range(count)]
        for k, (ident, off) in enumerate(pairs):
            end = pairs[k + 1][1] if k + 1 < count else size
            out.append((i, ident, d[off:end]))
    return out

def rpg_models(archive):
    """Just the field/town models, in archive order."""
    return [b for _, ident, b in rpg_parts(RPG_ARCHIVES[archive]) if ident >> 16]

def load_model(archive, index=0):
    if archive in ROBO_ARCHIVES:
        d = hsd.read_robo(entry(ROBO_ARCHIVES[archive], index))
        roots = list(d.roots.values())
    elif archive in SCENE_ARCHIVES:
        name, stride = SCENE_ARCHIVES[archive]
        d = hsd.read_hsd(entry(name, index * stride))
        roots = scene_roots(d, d.roots['scene_data'])
    elif archive in EFFECT_ARCHIVES:
        name = EFFECT_ARCHIVES[archive]
        per = 1 if name.endswith('.dbn') else 2
        subs = effect_subs(name, index // per if per > 1 else 0)
        d = hsd.read_hsd(subs[index % per if per > 1 else index])
        roots = scene_roots(d, d.roots['scene_data'])
    elif archive == 'map':
        lo, hi = map_sections(index)['scene']
        d = hsd.read_hsd(entry('scenario_us/snr_map.BIN', index)[lo:hi])
        roots = scene_roots(d, d.roots['scene_data'])
    elif archive in RPG_ARCHIVES:
        d = hsd.read_hsd(rpg_models(archive)[index])
        roots = scene_roots(d, d.roots['scene_data'])
    elif archive == 'title':
        d = hsd.read_hsd((FILES / 'TITLE_MODEL.dat').read_bytes())
        roots = scene_roots(d, d.roots['scene_data'])
    else:
        raise ValueError(archive)
    joints, meshes = [], []
    for model, root in enumerate(roots):
        j, m = hsd.read_meshes(d, root, mount=archive not in PART_ARCHIVES)
        for mesh in m:
            mesh['model'] = model
        if archive in EFFECT_ARCHIVES:
            # Effects have several ModelSets cooked into one CRM4 joint list:
            # walk_joints numbers each tree from 0, so shift to global indices.
            base = len(joints)
            for jj in j:
                jj.model = model
                jj.index += base
                if jj.parent >= 0:
                    jj.parent += base
            for mesh in m:
                mesh['joint'] += base
        joints += j; meshes += m
    return d, joints, meshes

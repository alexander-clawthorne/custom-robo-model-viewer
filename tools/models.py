"""Locate model archives on the extracted disc.

  chara N     robot body N (chara.BIN, Custom Robo wrapper)
  snrchara N  story character N (scenario_us/snr_chara.BIN, same wrapper)
  gun/bom/pod/leg N   weapon part N (weapon_<kind>.BIN, parts variant)
  bg N        arena N (bg_data.BIN entry 2N, standard HSD scene_data)
  snrobj N    story map object N (scenario_us/snr_obj.BIN, HSD scene_data)
  legfx N     leg-part effect N (leg_effect.BIN, HSD scene_data)
  title       TITLE_MODEL.dat
"""
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
SCENE_ARCHIVES = {'bg': ('bg_data.BIN', 2), 'snrobj': ('scenario_us/snr_obj.BIN', 1), 'legfx': ('leg_effect.BIN', 1)}

def load_model(archive, index=0):
    if archive in ROBO_ARCHIVES:
        d = hsd.read_robo(entry(ROBO_ARCHIVES[archive], index))
        roots = list(d.roots.values())
    elif archive in SCENE_ARCHIVES:
        name, stride = SCENE_ARCHIVES[archive]
        d = hsd.read_hsd(entry(name, index * stride))
        roots = scene_roots(d, d.roots['scene_data'])
    elif archive == 'title':
        d = hsd.read_hsd((FILES / 'TITLE_MODEL.dat').read_bytes())
        roots = scene_roots(d, d.roots['scene_data'])
    else:
        raise ValueError(archive)
    joints, meshes = [], []
    for model, root in enumerate(roots):
        j, m = hsd.read_meshes(d, root)
        for mesh in m:
            mesh['model'] = model
        joints += j; meshes += m
    return d, joints, meshes

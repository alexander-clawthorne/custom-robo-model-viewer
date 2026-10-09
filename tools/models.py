"""Locate model archives on the extracted disc.

  chara N   robot body N (chara.BIN entry N, Custom Robo wrapper)
  gun/bom/pod/leg N   weapon_<kind>.BIN entry N (same wrapper)
  bg N      arena N (bg_data.BIN entry 2N, a standard HSD scene_data)
  title     TITLE_MODEL.dat
"""
from pathlib import Path
import hsd

FILES = Path(__file__).resolve().parents[1] / 'data/extracted/files'

def entry(name, index):
    blob = (FILES / name).read_bytes()
    off, size = hsd.sfd_entries(blob)[index]
    return blob[off:off + size]

def scene_root(d, scene):
    """scene_data -> first ModelSet's JObj (SceneDesc: ModelSet**, Camera, Lights, Fog)."""
    models = d.ptr(scene)
    model = d.ptr(models)
    return d.ptr(model)

def load_model(archive, index=0):
    if archive == 'chara' or archive in ('gun', 'bom', 'pod', 'leg'):
        name = 'chara.BIN' if archive == 'chara' else f'weapon_{archive}.BIN'
        d = hsd.read_robo(entry(name, index))
        root = d.ptr(d.roots['root0'])  # root0 is a ModelSet: JObj first
    elif archive == 'bg':
        d = hsd.read_hsd(entry('bg_data.BIN', index * 2))
        root = scene_root(d, d.roots['scene_data'])
    elif archive == 'title':
        d = hsd.read_hsd((FILES / 'TITLE_MODEL.dat').read_bytes())
        root = scene_root(d, d.roots['scene_data'])
    else:
        raise ValueError(archive)
    joints, meshes = hsd.read_meshes(d, root)
    return d, joints, meshes

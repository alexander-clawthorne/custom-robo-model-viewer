# Step 13 handoff — effect playback (gun muzzle flash)

## Project
Custom Robo (GameCube GXCE01) assets rebuilt for 3DS. Read CLAUDE.md for full context.
Steps 1–4, 20, 21 done. Step 13 (effect playback) is the only carry-over.

## What exists
- `src/effects.c` — spawn/step/draw lifecycle, `efxWorldMats` builds per-joint world mats
- `src/battle.c` — hardcoded test: loads `romfs:/gunfx/022.crm` + `romfs:/gunfx/anim.efx`, spawns on gun fire
- `tools/cook_effects.py` — cooks HSD effect anims → EFX1 format; `cook_anim_v2` is current fn
- `tools/cook.py` — cooks HSD models → CRM4 format for effects (joint-local verts, per-batch joint index)
- Formats: CRM4 (joint-local geometry), EFX1 (animation tracks with joint index + channel + Hermite keys)

## Current bug — TWO issues

### Bug 1 (primary): wrong JObj root in cook_anim_v2
`cook_effects.py:119` does:
```python
jobj_root = list(d_hsd.roots.values())[0]
```
For effect archives, `d_hsd.roots == {'scene_data': <offset>}`. So `jobj_root` = scene_data offset, NOT a JObj. `walk_jobj` then traverses scene_data struct as if it were a JObj tree, building a garbage `by_off` map. Result: ALL animation tracks get assigned to joint_idx=0 (scene_data accidentally maps to 0). Every joint's scale/translation tracks pile onto the root joint → model renders enormous / wrong position.

**Fix for cook_effects.py lines 117–176:**
```python
    by_off = {}
    idx_counter = [0]
    def walk_jobj(off):
        if off is None: return
        by_off[off] = idx_counter[0]; idx_counter[0] += 1
        walk_jobj(d_hsd.ptr(off + 0x08))   # child
        walk_jobj(d_hsd.ptr(off + 0x0C))   # next
    # model_sets(d) returns ModelSet* offsets; slot 0 of ModelSet is JObj*
    # d == d_hsd for effect archives (same HSD file)
    for ms in model_sets(d):
        jobj_root = d_hsd.ptr(ms)
        if jobj_root is not None:
            try: walk_jobj(jobj_root)
            except Exception: pass

    duration = 0.0
    joint_blobs = []
    mat_blobs = []

    for ms in model_sets(d):
        for root in anim_roots(d, ms, ANIM_JOINT):
            def visit_anim(aj_off, jobj_off):
                if aj_off is None or jobj_off is None: return
                joint_idx = by_off.get(jobj_off, -1)
                p = d.ptr(aj_off + 8)
                a = aobj(d, p)
                if a and joint_idx >= 0:
                    _, end_frame, fobjs = a
                    nonlocal duration
                    duration = max(duration, end_frame)
                    for f in fobjs:
                        if f.track not in JOBJ_KEEP: continue
                        try: keys = fobj_keys(f)
                        except Exception: continue
                        if keys:
                            joint_blobs.append((joint_idx, f.track, keys))
                aj_child = d.ptr(aj_off)
                jobj_child = d_hsd.ptr(jobj_off + 0x08)
                visit_anim(aj_child, jobj_child)
                aj_next = d.ptr(aj_off + 4)
                jobj_next = d_hsd.ptr(jobj_off + 0x0C)
                visit_anim(aj_next, jobj_next)

            jobj_ms_root = d_hsd.ptr(ms)   # <-- was: list(d_hsd.roots.values())[0]
            visit_anim(root, jobj_ms_root)
```

### Bug 2 (secondary): wrong effect index
`src/battle.c` hardcodes index 22 (`gun_effect.BIN` SFD entry 11). Ray gun (default test gun) is type 0, so its effect is index 0 (SFD entry 0, sub 0). Change:
- Model path: `romfs:/gunfx/000.crm` (was `022.crm`)
- `efxSpawn` index arg: `0` (was `22`)

## How to verify fix
1. Apply both fixes
2. Recook: `PYTHONUTF8=1 python tools/cook_effects.py gunfx`
3. Recook model: `PYTHONUTF8=1 python tools/cook.py --only gunfx:0`
4. Rebuild + test in Citra autotest (AUTOCAT=battle or just run battle)
5. Expected: small muzzle flash appears at gun muzzle position, correct size, animated

## Key file locations
- `tools/cook_effects.py` — fix goes here (cook_anim_v2 function)
- `src/battle.c` — change hardcoded index 22→0 and path 022→000
- `src/effects.c` — efxWorldMats, efxSpawn, efxStepAll, efxDrawAll (no changes needed)
- `tools/effects.py` — model_sets(), anim_roots(), aobj(), fobj_keys() helpers
- `tools/models.py` — load_model(), EFFECT_ARCHIVES dict

## Context on HSD effect archive structure
`gun_effect.BIN`: SFD container, each entry = 2 sub-archives (HSD .dat format).
Index N → SFD entry N//2, sub-archive N%2.
Each sub-archive has one root: `scene_data` → ModelSet** array (null-terminated) → each ModelSet has [JObj*, AnimJoint**, MatAnimJoint**, ShapeAnimJoint**].
`model_sets(d)` returns each ModelSet* in order.
`d_hsd.ptr(ms)` = JObj* (slot 0 of ModelSet).

## Build commands (from CLAUDE.md)
```
PYTHONUTF8=1 python tools/cook_effects.py gunfx
PYTHONUTF8=1 python tools/cook.py --only gunfx:0
docker run --rm -v "C:/Users/Alexander/source/repos/custom-robo-3ds:/proj" -w /proj mkw3ds-build:2026-10-09 make
```
Citra: `C:\Users\Alexander\AppData\Roaming\EmuDeck\Emulators\citra\citra-qt.exe`
Autotest script: `tools/citra/autotest.ps1`

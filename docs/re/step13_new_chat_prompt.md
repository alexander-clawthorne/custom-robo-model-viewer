Continue the Custom Robo 3DS port. Read CLAUDE.md and docs/progress.html first.
Phases 1–4 and steps 20–21 (movement, camera, weapons) are done. Step 13 (effect
playback — gun muzzle flash) is a carry-over still marked in-progress. Fix and close it.

**Start immediately by editing the progress tracker artifact**
(https://claude.ai/artifact/F3a5jSYw1tkRNhA7vf9zog) — mark step 13 as "In
progress / fixing". This triggers the artifact token discount for the next 10
messages, so do it before any file reads or searches.

---

## What exists

- `src/effects.c` — spawn/step/draw lifecycle, `efxWorldMats` builds per-joint
  world matrices from the CRM4 joint tree + EFX1 animation tracks.
- `src/battle.c` — hardcoded test: loads `romfs:/gunfx/022.crm` +
  `romfs:/gunfx/anim.efx`, calls `efxSpawn` at the muzzle position on gun fire.
- `tools/cook_effects.py` — cooks HSD effect animations → EFX1 binary.
  `cook_anim_v2` is the current function.
- `tools/cook.py` — cooks HSD models → CRM4 (joint-local vertices, per-batch
  joint index).
- Formats: CRM4 (joint-local geometry, one batch per mesh), EFX1 (per-joint
  cubic Hermite animation tracks). Both are already loading and drawing.
- Recent fix (dd6aa9d): CRM4 batches now preserve their joint index instead of
  being reset to -1. The rendering is less broken but still wrong.

---

## Two bugs to fix

### Bug 1 (primary): wrong JObj root in cook_anim_v2

`tools/cook_effects.py` line ~119:
```python
jobj_root = list(d_hsd.roots.values())[0]
```
For effect archives, `d_hsd.roots == {'scene_data': <offset>}`. So `jobj_root`
is the scene_data struct offset, not a JObj. `walk_jobj` then traverses
scene_data as if it were a JObj tree, producing a garbage `by_off` map.
Result: ALL animation tracks get assigned to joint_idx=0. Every joint's
scale/translation tracks pile onto the root joint → model renders arena-sized.

**Fix** — replace the `by_off` build and `jobj_ms_root` assignment in
`cook_anim_v2`:

```python
    by_off = {}
    idx_counter = [0]
    def walk_jobj(off):
        if off is None: return
        by_off[off] = idx_counter[0]; idx_counter[0] += 1
        walk_jobj(d_hsd.ptr(off + 0x08))   # child
        walk_jobj(d_hsd.ptr(off + 0x0C))   # next
    # model_sets(d) returns ModelSet* offsets; d == d_hsd for effect archives.
    # Slot 0 of each ModelSet is JObj*.
    for ms in model_sets(d):
        jobj_root = d_hsd.ptr(ms)
        if jobj_root is not None:
            try: walk_jobj(jobj_root)
            except Exception: pass
```

And in the `for ms in model_sets(d)` loop below, change:
```python
    jobj_ms_root = list(d_hsd.roots.values())[0] if d_hsd.roots else None
```
to:
```python
    jobj_ms_root = d_hsd.ptr(ms)   # JObj* for this ModelSet
```

### Bug 2 (secondary): wrong effect index

`src/battle.c` hardcodes index 22 (`gun_effect.BIN` SFD entry 11). The Ray gun
(default test gun) is type 0, so its effect is index 0. Change:
- Model path: `"romfs:/gunfx/000.crm"` (was `"022.crm"`)
- `efxSpawn` index arg: `0` (was `22`)

---

## HSD effect archive structure (for reference)

`gun_effect.BIN` is an SFD container. Each SFD entry holds 2 HSD sub-archives
(projectile + burst). Index N → SFD entry N//2, sub-archive N%2.

Each sub-archive is a standard HSD .dat file with one named root: `scene_data`.
scene_data → ModelSet** array (null-terminated) → each ModelSet layout:
  `[JObj*, AnimJoint**, MatAnimJoint**, ShapeAnimJoint**]`

`model_sets(d)` (tools/effects.py) returns each ModelSet* offset in order.
`d_hsd.ptr(ms)` = the JObj* at slot 0.

---

## Build and test

```
PYTHONUTF8=1 python tools/cook_effects.py gunfx
PYTHONUTF8=1 python tools/cook.py --only gunfx:0
```
Then rebuild (docker `make`) and run:
```
tools/citra/autotest.ps1 -Fast -Shots 0
```
Expected: small muzzle flash appears at the muzzle position, correct scale,
plays out over ~20–30 frames, then disappears. No arena-filling artefact.

---

## Working rules

- Plan first, code second. Opus maps the fix; delegate the actual edits to a
  subagent (Sonnet) so heavy-model tokens go to reasoning, not boilerplate.
- Use the artifact discount: every time you update the tracker artifact, the
  next 10 messages cost 50% less. Update it at start AND when step 13 closes.
- Run `/compact` if context reaches ~50%. Prefer subagents for wide searches
  (tools/effects.py helpers, hsd.py DFS order) so raw output stays out of
  main context.
- Verify numbers with Ghidra (`tools/ghidra/decompile.ps1`) before trusting them.
- Never steal window focus; use `autotest.ps1 -Fast -Shots 0` for battle tests.
- Commit to main as you go. When step 13 is closed, republish the tracker
  artifact at https://claude.ai/artifact/F3a5jSYw1tkRNhA7vf9zog.
- I copy the .3dsx to my 3DS myself — just give the path.

# custom-robo-3ds: working notes for Claude

Custom Robo (GameCube, GXCE01) assets rebuilt for the 3DS. Progress tracker:
`docs/progress.html` (published at https://claude.ai/artifact/F3a5jSYw1tkRNhA7vf9zog;
edit the file, then republish to that URL).

## Build

```
python tools/extract_disc.py            # once: disc -> data/ (git-ignored)
PYTHONUTF8=1 python tools/cook.py       # models -> romfs/ (--only chara:0 snrchara:3 ...)
PYTHONUTF8=1 python tools/cook_motion.py  # romfs/motion/robot.mot + story.mot
docker run --rm -v "C:/Users/Alexander/source/repos/custom-robo-3ds:/proj" -w /proj \
  mkw3ds-build:2026-10-09 make          # -> custom-robo-viewer.3dsx
  ... make AUTOTEST=1 TARGET=custom-robo-viewer-autotest BUILD=build3ds_auto
  ... make AUTOTEST=1 AUTOCAT=collide AUTOSTEP=5 ...   # one category only
```
- Mount the repo at **/proj**: the build dirs' dependency files hold that path.
- `make` does not notice romfs changes; delete the `.3dsx` to force a relink.

## Test

- Citra: `C:\Users\Alexander\AppData\Roaming\EmuDeck\Emulators\citra\citra-qt.exe`.
  Its only input profile is an Xbox pad, so test with the AUTOTEST build, which
  steps every 10th model of every category, 2 s each (robot shots ~0-16,
  story characters ~44-49). A full sweep is now over 72 shots, so when only
  one category changed build with `AUTOCAT=<romfs folder>` (and `AUTOSTEP=<n>`
  for the stride) and pass a small `-Shots` -- the run ends when it runs out
  of models. `make` does not see those two change, so clear the autotest build
  first, every time:
  `rm -rf build3ds_auto custom-robo-viewer-autotest.elf custom-robo-viewer-autotest.3dsx`
- **Never take focus from the user's desktop.** Launch, push Citra to the back,
  capture with PrintWindow flag 2. All done by `tools/citra/autotest.ps1 -Dir build\shots`.
- Review runs with `tools/contact_sheet.py` (one small image), not single shots.
- The user copies the `.3dsx` to the 3DS with FileZilla; just give the path.

## Formats (details in the tool docstrings)

- HAL sysdolphin (HSD) engine, as in Melee: the Melee decomp `jobj.c` is the
  reference for joints and IK. Robot/story-character/weapon files use a custom
  0x40-byte wrapper (`tools/hsd.py read_robo`).
- PICA textures: store like tex3ds (top row first, Morton 8x8), sample t = 1 - v.
- Motions (`tools/motion.py`): banks of 26 nodes x 3 Hermite channels, ending in a
  u16 ID list; name = DOL record 0x80206db0 + 12*ID. Robot IDs 1-262, story IDs
  418-1094 (motion.DBN bank 2 + snr_motion.BIN, one bank per scenario script).
- Posing (`tools/pose.py`, ported to `src/anim.c`): root nodes 0x80056104,
  mirror 0x80053a20, two-pass solve 0x8005aee8, blend 0x80052884.
- Scenario scripts (`scenario/snr_t_01-20.bin`, `snr_f_01-30.bin`, magic `SCR`)
  are big-endian u16 bytecode; `66, actor, motionID, flag` appears to play a
  motion (IDs 418+ match story motion names). Not decoded beyond that.
- Arena collision (`tools/collision.py`): bg_data.BIN odd entry 2N+1, blocks of
  convex faces with precomputed planes, 4-gons everywhere but arena 7 (3-gons);
  the stride comes from the gap to the next block, not from a field. The game
  sweeps a sphere per block sphere, then per face. The entry's trailing section
  is runtime scratch, not data. Cooked to `romfs/collide` as a viewer category.
- Effects (`tools/effects.py`): each `*_effect.BIN` SFD entry is a container of
  its own (u32 count, then count+1 offsets) holding two standard HSD archives;
  `efc_common.dbn` is that container without the SFD, `leg_effect.BIN` is plain
  HSD. A ModelSet's slots 1-3 are NULL-terminated *arrays* of AnimJoint /
  MatAnimJoint / ShapeAnimJoint roots, one per model, not single pointers.
  Animation is stock HSD AObj/FObj, not Custom Robo's own motion format.
  Values inside a keyframe stream are little-endian and waits are LSB-first
  varints, though the file around them is big-endian. Opcode KEY reads a wait
  (the Melee decomp is right, HSDLib is not). No effect file contains an HSD
  particle object: joint track 40 is a bare trigger with a null object id, so
  what it spawns lives in the DOL, not the file.
- Robots and story characters share the 19-slot rig + 32 dimension floats and
  must be cooked as CRM2 (`cook.py SKINNED`).

## Keeping token use down

- Hand wide searches to subagents on a cheaper model (Sonnet or Haiku), for
  example "find every structure in snr_map.BIN" or "which functions read
  bg_data odd entries". Only their conclusion comes back into this context,
  not pages of hex dumps or disassembly. Keep the main model for design and
  for checking what the subagents report.
- Look at test runs as one contact sheet, not individual screenshots.
- Start a fresh session per phase; this file and the tracker carry the state.

## Reverse engineering

- `tools/dol.py` (xref / dis / func) for quick looks at main.dol.
- `tools/ghidra/decompile.ps1 -Out build\ghidra\x.c 0xADDR ...` for C output
  (headless Ghidra 12.1.4 at C:\Tools; needs JAVA_HOME jdk21). REA rejects
  PowerPC, so use Ghidra directly.
- `build/motion_RE_notes.md` (not committed) has the motion research with
  instruction addresses.
- Keep tool output small: grep decompiles, do not dump whole functions.

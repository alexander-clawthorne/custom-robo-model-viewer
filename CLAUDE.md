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
```
- Mount the repo at **/proj**: the build dirs' dependency files hold that path.
- `make` does not notice romfs changes; delete the `.3dsx` to force a relink.

## Test

- Citra: `C:\Users\Alexander\AppData\Roaming\EmuDeck\Emulators\citra\citra-qt.exe`.
  Its only input profile is an Xbox pad, so test with the AUTOTEST build, which
  steps every 10th model of every category, 2 s each (robot shots ~0-16,
  story characters ~44-49).
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
- Robots and story characters share the 19-slot rig + 32 dimension floats and
  must be cooked as CRM2 (`cook.py SKINNED`).

## Reverse engineering

- `tools/dol.py` (xref / dis / func) for quick looks at main.dol.
- `tools/ghidra/decompile.ps1 -Out build\ghidra\x.c 0xADDR ...` for C output
  (headless Ghidra 12.1.4 at C:\Tools; needs JAVA_HOME jdk21). REA rejects
  PowerPC, so use Ghidra directly.
- `build/motion_RE_notes.md` (not committed) has the motion research with
  instruction addresses.
- Keep tool output small: grep decompiles, do not dump whole functions.

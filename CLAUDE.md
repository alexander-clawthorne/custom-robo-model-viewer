# custom-robo-3ds: working notes for Claude

Custom Robo (GameCube, GXCE01) assets rebuilt for the 3DS. Progress tracker:
`docs/progress.html` (published at https://claude.ai/artifact/F3a5jSYw1tkRNhA7vf9zog;
edit the file, then republish to that URL).

## Build

```
python tools/extract_disc.py            # once: disc -> data/ (git-ignored)
PYTHONUTF8=1 python tools/cook.py       # models -> romfs/ (--only chara:0 snrchara:3 ...)
PYTHONUTF8=1 python tools/cook_motion.py  # romfs/motion/robot.mot + story.mot
PYTHONUTF8=1 python tools/stats.py --cook  # romfs/stats/parts.bin (part stats)
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
- **Battle tests log, they don't screenshot.** The battle autotest
  (`AUTOCAT=battle`) runs every movement once per arena -- run, held jump,
  hop, all air dashes, a turn, a wall -- checks each against the stats and
  writes the results to `sdmc:/cr_autotest.log`. Run it with
  `tools/citra/autotest.ps1 -Fast -Shots 0`: Citra unthrottled, no shots,
  closed when the log's `# DONE` line appears, and the log copied to
  `build/cr_autotest.log` (`# arena ...` lines are the verdicts, the rest
  one line per frame). Fast-forward does not change results: the game steps
  per frame, not per second. One robot: `AUTOBODY=<0-33> AUTOLEG=<0-28>
  AUTOARENA=<0-39>` (4 s); all bodies' dash types: `AUTOSTEP=4` (~40 s).
  Screenshots only for things that must be seen.
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
- Scenario scripts (`tools/scr.py`, `scenario/snr_*.bin`, magic `SCR`). Every
  offset in the file is relative to 0x40, where the entry table of
  (code ptr, name ptr) pairs starts. The game shipped its own symbol names for
  both the 7092 scripts and the 300 opcodes: the opcode names are 300 char* at
  `0x8021bf44` in main.dol, index-aligned with the handlers at `0x8021c3f4`
  (`opcode_names()` reads them). There is no dispatch loop -- the interpreter
  is threaded, each handler advancing the pc by its own hardcoded length, so
  the instruction sizes live in `OPCODE_BYTES`, not in any table.
  Instructions are 4-byte aligned: u16 opcode, u16 at +2 (actor or flag id),
  then u32 operands -- pointers (marked by the relocation table; the operand
  points at the *entry-table record*, so ptr/8 is the script index), floats,
  or packed u16 pairs. `CHAR_TALK` (59) is actor + BMG slot, `CHAR_MOT_SET`
  (66) is actor + motion id + flag (flag 1 = global motion id, flag 0 = an
  index into the actor's own table), `F_CHK_BIT` (248) is the if/else.
  `snr_f_25-28` are four byte-identical copies of a stale 2003 build and
  `snr_f_29` another; they use different opcode numbering and do not decode.
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
- Story maps (`tools/models.py map_sections`): `snr_map.BIN` entry N starts with
  the offset of a collision section, the offset of the map's own HSD archive,
  the x and z extents, and the offset of an optional trailing container of
  further HSD scenes. A zero offset means that section is absent. The collision
  is the same format as the arenas', one block per map (`collision.py --maps`).
- `rpg_f/t_models.BIN`: u32 count, then count (id, offset) pairs. An id with its
  high half set is a field or town model (138 of them); the other 441 are
  byte-for-byte copies of `snr_chara.BIN` entries bundled per location, and the
  id IS the story-character index, so they need no cooking of their own.
- Text (`tools/bmg.py`): Nintendo BMG, INF1 (u32 offset per message slot, 0 =
  empty) + DAT1. Shift-JIS. **Escape parameters contain 0x00**, so a message
  ends at the first NUL that a length-aware walk reaches, not the first NUL.
  Escapes: `fe 0006` speaker/portrait, `fe 0003` end, `fe 0008` pause,
  `ff 0002` ruby, `ff 0000` colour, `ff 0001` delay, `01 0000` sound.
  915 of the 13233 US messages are still Japanese (snr_t_20 almost entirely).
- Font (`tools/font.py`): `message/font.out` is the effect-style container
  holding four `FONTbfn1` banks (JUTResFont). GLY1 is one 768x1024 I4 sheet of
  24x24 cells, 32 across, GameCube-tiled so gxtex reads it; the GLY1 header
  gives columns before rows. WID1's pair is (left bearing, width) and a glyph
  is the slice [left, left+width) -- slicing from 0 clips letters. MAP1 type 2
  is a u16 glyph index per code. `message/font.bmc` is CLT1, 256 RGBA with
  only six distinct, which is what the `ff 0000` escape picks from.
- Sound (`tools/sound.py`): all Nintendo DSP-ADPCM. `*.hps` music is
  ' HALPST\0', 73 stereo streams chained by blocks whose next-block offset is
  ABSOLUTE; inside a block each channel's bytes are contiguous halves, not
  frame-interleaved. Each block stores the decoder history it should start
  from, so decoding a block and comparing with the next block's stored history
  is a self-check (`--check`: 438 matched, 0 mismatched). 64 of the 73 loop.
  `*.ssm` banks: u32 data offset RELATIVE TO 0x10, size, count, then records of
  0x48 bytes plus 0x40 per extra channel (first field is the channel count) --
  read that way all 59 banks tile exactly. Record start/end are NIBBLE
  addresses and every start is 2 mod 16, because nibbles 0-1 are the frame's
  header byte: slice at the frame, not at start//2, or the decode is garbage.
  A stereo sample's second channel follows the first, same length again.
  libctru takes this codec as-is (`NDSP_ENCODING_ADPCM` is "DSPADPCM (GameCube
  format)", `ndspChnSetAdpcmCoefs` wants the same 16 coefficients), so
  converting is repackaging, not re-encoding -- but ADPCM there is MONO ONLY,
  so a stereo stream needs two NDSP channels panned apart.
- Menu and HUD art (`tools/tpl.py`): plain Nintendo TPL, 590 files and 1632
  images in formats gxtex already decodes (RGB565, RGBA32, CI4, CI8, CMPR).
  The image header is HEIGHT then WIDTH -- the other order still decodes
  without error, so it was settled by checking each image's data against the
  gap to the next, which height-first accounts for and width-first does not.
  The three 299x200 CMPR images in `custom.tpl` really are static: every data
  offset and size in that file matches exactly, so it is the art, not a bug.
- Robots and story characters share the 19-slot rig + 32 dimension floats and
  must be cooked as CRM2 (`cook.py SKINNED`).
- Robot assembly (`tools/assemble.py`, `models.ATTACH`, `attach[]` in
  `src/main.c`): the draw code at `0x80039fac` copies the solved matrix
  `out[k]` straight into a weapon part root's world matrix and sets
  `JOBJ_USER_DEF_MTX | MTX_INDEPEND_PARENT | MTX_INDEPEND_SRT` (0x3800000),
  so **the root joint's stored SRT never reaches the screen** even though it
  looks like a mount rotation (guns -90 deg X, pods 90/0/90). The only thing
  applied is a uniform 0.1 scale of the basis (`*(r2-0x7b18)`; parts are
  modelled ten times robot size). The nearby call that looks like a rotation,
  `0x8001da70`, scales each axis instead. Rig slot r is driven by `out[r+1]`,
  so a part sits at `slot_world * scale(0.1)` and nothing else. Mapping:
  gun root 0 -> slot 8, bomb root 1 -> slot 4, pod roots 0/1 -> slot 0,
  leg roots 5/3 -> slots 12/13 and 4/1 -> slots 16/17; the other roots in a
  part file are never drawn. Parts cook as CRM3 (per-batch root index).
- Part stats (`tools/stats.py`): compiled into main.dol, not files. Five
  tables indexed by the save slot's part ids -- body 0x801c6c44 (0x134 x 34),
  gun 0x801ca560 (0x28c), bomb 0x801d4860 (0x17c), pod 0x801d9590 (0x174),
  leg 0x801c952c (0x8f x 29) -- copied whole into the robot struct by
  0x80005da4. 0x80006658 combines body and leg into 15 s16 movement stats
  at S+0x3341c: the body has a (min, normal, max) triple per stat, the leg a
  percentage (100 = normal, 200 = max). chara.BIN is the 34 bodies x 5
  colour variants. The per-player structs are 0x33520 apart from 0x8024e120
  (correction: this said 0x8024e140).
- Movement (`src/battle.c`): update 0x80049924, air dash 0x80048708
  (correction: this said 0x80046f58, which is action 11, pod in a sky dash),
  gravity/landing/position 0x8004b0a0. Velocity is in hundredths of a world
  unit per frame (pos += 0.01 * vel, z negated). The jump rises only while
  held, for stat 4 frames, and peaks at stat 3; landing lag is stat 7 x the
  impact (vs a 6-unit fall) plus stat 10 after an air dash.
  The body's dash type (+0x56) picks the action a mid-air press starts,
  from the table at 0x801f8eb8 indexed by S+0x528: type 0 air dash
  (action 7, 0x80048708), 1 air jump (6, 0x80048b10), 2 sky dash (8,
  0x80048050, the Lightning Sky models). All hang still for stat 9 frames
  first; gravity is applied only by action 0. 0x801c6c00 is a
  block of mixed s16/float tuning constants (2.0 at +4, 6.0 at +8, a
  3-entry floor table per surface kind at +0x1c: slip threshold, slide
  step, grip). On the ground the robot takes facing x speed outright
  unless that is more than 0.02/frame from its velocity; then it slides
  at 0.002/frame -- the skid after landing a moving jump. The floor is
  found by a downward query, 0x8006207c; walls by a routine not yet
  traced. Per-body dimensions (30 floats, stride 0x78 from 0x801c5c10,
  copied to S+0x17f4): word 0 is the height (Ray 1.31).
- Battle camera (`battleCamera` in src/battle.c): camera modes are an
  (init, update) pair per mode at 0x801f7ff0, names at 0x801f88a4
  (0x800333dc is RESULT's init, not the battle camera). Battle is mode 2,
  CAMMODE_MIDDLE, update 0x80035bcc: fit 0x8002b3c4, blend 0x8002e04c
  (factor 1.0, so it snaps). Fixed direction, yaw 0 looking down -Z,
  pitch 30 (r13-0x7f98); every robot is fitted inside planes 25.2 either
  side and 20 below / 10 above the view axis (so: a 30-degree view aimed
  35 down), padded 0.85 / 0.546 x 1.9 for two robots, min 5.7 away.
  r2 = 0x804581a0, r13 = 0x804543c0.
- Part files carry two path lists the loader at `0x80073cb8` walks (`02` =
  child, `01` = next, as in the rig paths): header word 4 is one path per
  root and word 8 (or -1) is, per root, a count byte then that many paths to
  effect-spawn joints. Their counts tile exactly against the runtime arrays
  at `0x80377c60`, which is what confirmed the layout.
- Weapons (`src/weapon.c`, records cooked by `tools/stats.py`; full notes
  in build/weapons_RE_{gun,bomb_pod,damage}.md, not committed). Actions:
  3/4/5 gun/bomb/pod on the ground (0x8004949c, 0x800490c8, 0x80048e7c),
  9/10/11 the same in a sky dash, 12 tackle (button 0x40), 13 stagger,
  14 knocked down, 15 magma, 16 rebirth (30 frames + 60 invulnerable via
  S+0xe6), 17/18/19 the 0x80 move, 20/21 win/lose. Per frame the player
  loop 0x8003b528 runs the action, then 0x8004b0a0 (which reads last
  frame's hit record S+0x18a0), the pose, the weapon updates (gun
  0x80042ab8, bomb 0x8003fe98, pod 0x8003dd38) and the hit tests
  0x80041358 (bullets) and 0x800443c0 (blasts). S+0x1074 is out[0..19]:
  homing aims at out[1], the hit sphere sits midway between out[2] and
  out[11], radius body+0xac/100. HP is the float S+0x74, 1000 for every
  body; body+0x40 is the down-gauge threshold, not HP. Damage taken =
  dmg x body+0x42 / 90 (the 900 at 0x801c6c00). Wall sphere 1.4 x
  body+0xa6 / 100. Bombs fly a parametric arc to a stick-aimed cursor,
  not gravity; pods home and hover on a spring. Not done yet: muzzle
  joints (shots leave the mount), air-dash weapons, tackle, the 0x80 move,
  the arena circle at 0x8024e100 (set in 0x80010220).

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
  (parallel runs: add `-Copy build\ghidra_<name>`; Ghidra locks its project)
  (headless Ghidra 12.1.4 at C:\Tools; needs JAVA_HOME jdk21). REA rejects
  PowerPC, so use Ghidra directly.
- `build/motion_RE_notes.md` (not committed) has the motion research with
  instruction addresses.
- Keep tool output small: grep decompiles, do not dump whole functions.

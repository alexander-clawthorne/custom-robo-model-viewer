"""Feed this repo's main.dol reverse engineering into the matching decomp (~/Decomp/main, decomp.wtf).

    PYTHONUTF8=1 python tools/decomp_seed.py            # apply, build, resync the app, post notes
    PYTHONUTF8=1 python tools/decomp_seed.py scan       # main.dol addresses our notes mention but the tables don't

The knowledge lives in three hand-kept tables next to this script; everything in the decomp tree is
generated from them, so re-run after adding a row:

- decomp_names.tsv    address, name, meaning: functions and data objects that START at the address.
- decomp_notes.tsv    address, symbol+offset, note: instructions / fields inside a function or object.
- decomp_structs.tsv  struct and field rows (layout-faithful: offset + type), padded by the generator.

Naming convention: functions are snake_case with a subsystem prefix (robot_, weapon_, gun_, bomb_, pod_,
hit_, cam_, coll_, motion_, scr_, part_, battle_, input_, hsd_), data objects are gCamelCase, struct
fields snake_case. Only rows the RE has established go in; an address is better than a wrong name.

What it writes in the decomp (all idempotent):

1. config/<ver>/symbols.txt: renames the symbol at each address, but only one still named by dtk
   (fn_/lbl_XXXXXXXX) or one this script named before (config/<ver>/seed_names.json); SDK names
   dtk recognised are left alone. The old name is replaced word-for-word in src/ and include/.
2. include/custom_robo.h: the structs, with size checks that fail the compile if a layout is off;
   included from common.h, so every unit (and every agent compile) sees them.
3. docs/custom_robo_re.md: every name and inside-function note, plus CLAUDE.md's main.dol bullets
   with each address annotated by its name. AGENTS.md (which decomp.wtf tells its agents to read)
   gets a short generated section pointing at both.
4. ninja (the byte-for-byte check), then the app's re-measure (POST /api/fork/check) so its function
   list takes the new names, then one note per named function through `decomp mcp` (add_note),
   skipping notes already there.

decomp.wtf keys a fork's functions on their symbol name, so renaming a function the app already has
attempts for leaves that history on a 'nosource' row under the old name. Name functions before
agents work on them.
"""

import argparse
import glob
import json
import os
import re
import sqlite3
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HERE = Path(__file__).resolve().parent
DECOMP = Path.home() / "Decomp" / "main"
APPDIR = Path.home() / "Decomp" / ".decomp"
PREFIX = "[custom-robo-3ds]"
AUTO = re.compile(r"(fn|lbl|func)_[0-9A-Fa-f]{8}$")
SYM = re.compile(r"(\S+) = (\.\w+):0x([0-9A-Fa-f]+);(.*)$")
ADDR = re.compile(r"0x(80[0-9a-fA-F]{6})\b")
SOURCES = ["CLAUDE.md", "src/*.c", "src/*.h", "tools/*.py", "docs/re/*.md"]

TYPES = {"s8": (1, 1), "u8": (1, 1), "s16": (2, 2), "u16": (2, 2), "s32": (4, 4), "u32": (4, 4),
         "f32": (4, 4), "void*": (4, 4), "Vec3": (12, 4), "Mtx34": (0x30, 4)}


def tsv(path: Path) -> list[list[str]]:
    if not path.exists():
        return []
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip() and not line.startswith("#"):
            rows.append([c.strip() for c in line.split("\t")])
    return rows


def load_names() -> dict[int, tuple[str, str]]:
    out, seen = {}, {}
    for r in tsv(HERE / "decomp_names.tsv"):
        a, name, meaning = int(r[0], 16), r[1], (r[2] if len(r) > 2 else "")
        if name == "-":  # a known role, not enough for a name: a note only
            out[a] = (None, meaning)
            continue
        if not re.fullmatch(r"[A-Za-z_]\w*", name):
            sys.exit(f"decomp_names.tsv: {name!r} is not a C identifier")
        if name in seen:
            sys.exit(f"decomp_names.tsv: {name} named twice ({seen[name]:#x}, {a:#x})")
        seen[name] = a
        out[a] = (name, meaning)
    return out


def load_notes() -> list[tuple[int, str, str]]:
    return [(int(r[0], 16), r[1], r[2]) for r in tsv(HERE / "decomp_notes.tsv") if len(r) > 2]


def mentions() -> dict[int, list[str]]:
    """Every main.dol address our notes and code mention -> where."""
    out: dict[int, list[str]] = {}
    for pat in SOURCES:
        for f in sorted(glob.glob(str(REPO / pat))):
            rel = Path(f).relative_to(REPO).as_posix()
            for i, line in enumerate(Path(f).read_text(encoding="utf-8", errors="replace").splitlines()):
                for a in ADDR.findall(line):
                    out.setdefault(int(a, 16), []).append(f"{rel}:{i + 1}")
    return out


# ---------------------------------------------------------------- symbols.txt

def read_symbols(path: Path):
    lines = path.read_text(encoding="utf-8").splitlines()
    at: dict[int, int] = {}       # address -> line index (first symbol there that isn't a label)
    names: dict[str, int] = {}
    for i, line in enumerate(lines):
        m = SYM.match(line)
        if not m:
            continue
        names[m[1]] = i
        a = int(m[3], 16)
        if a not in at or "type:label" in lines[at[a]]:
            at[a] = i
    return lines, at, names


def containing(lines, at, a: int) -> str | None:
    """The function/object holding address a, as name+0xoff."""
    best = None
    for b, i in at.items():
        if b <= a and (best is None or b > best[0]):
            best = (b, i)
    if best is None:
        return None
    m = SYM.match(lines[best[1]])
    size = re.search(r"size:0x([0-9A-Fa-f]+)", m[4])
    if size and a >= best[0] + int(size[1], 16):
        return None
    return m[1] if a == best[0] else f"{m[1]}+0x{a - best[0]:x}"


def rename_symbols(root: Path, ver: str, names, report) -> dict[str, str]:
    path = root / "config" / ver / "symbols.txt"
    manifest_path = root / "config" / ver / "seed_names.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
    lines, at, taken = read_symbols(path)
    renames: dict[str, str] = {}
    for a, (new, _) in sorted(names.items()):
        if new is None:
            continue
        i = at.get(a)
        if i is None:
            report["no symbol at address"].append(f"{a:#x} {new}")
            continue
        old = SYM.match(lines[i])[1]
        if old == new:
            report["already named"].append(new)
            manifest[f"{a:#010x}"] = new
            continue
        if not (AUTO.match(old) or manifest.get(f"{a:#010x}") == old):
            report["kept (not ours to rename)"].append(f"{a:#x} {old} (wanted {new})")
            continue
        if new in taken:
            report["name already used elsewhere"].append(f"{a:#x} {new}")
            continue
        lines[i] = new + lines[i][len(old):]
        del taken[old]
        taken[new] = i
        renames[old] = new
        manifest[f"{a:#010x}"] = new
        report["renamed"].append(f"{old} -> {new}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    manifest_path.write_text(json.dumps(dict(sorted(manifest.items())), indent=1) + "\n", encoding="utf-8")
    if renames:
        word = re.compile(r"\b(" + "|".join(map(re.escape, renames)) + r")\b")
        for pat in ("src/**/*.c", "src/**/*.cpp", "src/**/*.h", "include/**/*.h"):
            for f in glob.glob(str(root / pat), recursive=True):
                text = Path(f).read_text(encoding="utf-8")
                new_text = word.sub(lambda m: renames[m[1]], text)
                if new_text != text:
                    Path(f).write_text(new_text, encoding="utf-8")
                    report["sources rewritten"].append(Path(f).relative_to(root).as_posix())
    return renames


# ---------------------------------------------------------------- include/custom_robo.h

def ctype(t: str, structs) -> tuple[str, str, int, int]:
    """'s16[15]' -> ('s16', '[15]', size, align)."""
    m = re.fullmatch(r"([\w*]+)\s*(?:\[(\w+)\])?(?:\[(\w+)\])?", t)
    if not m:
        sys.exit(f"decomp_structs.tsv: can't read type {t!r}")
    base, dims = m[1], [int(d, 0) for d in (m[2], m[3]) if d]
    if base in TYPES:
        size, align = TYPES[base]
    elif base in structs:
        size, align = structs[base]["size"], structs[base]["align"]
    else:
        sys.exit(f"decomp_structs.tsv: unknown type {base!r}")
    n = 1
    for d in dims:
        n *= d
    return base, "".join(f"[{d:#x}]" if d > 9 else f"[{d}]" for d in dims), size * n, align


def gen_header(report) -> str:
    structs: dict[str, dict] = {}
    for r in tsv(HERE / "decomp_structs.tsv"):
        if r[0] == "struct":
            structs[r[1]] = {"size": int(r[2], 16), "desc": r[3] if len(r) > 3 else "", "fields": [], "align": 1}
        elif r[0] == "field":
            structs[r[1]]["fields"].append((int(r[2], 16), r[3], r[4], r[5] if len(r) > 5 else ""))
    out = ["/* GENERATED by custom-robo-3ds tools/decomp_seed.py from tools/decomp_structs.tsv -- edit the TSV",
           " * there and re-run, not this file. Offsets come from tracing main.dol; gaps are pad_XXX bytes that",
           " * nobody has traced yet. Fields are only listed where both offset and type are established. */",
           "#ifndef CUSTOM_ROBO_H", "#define CUSTOM_ROBO_H", "",
           "typedef f32 Vec3[3];", "typedef f32 Mtx34[3][4];", ""]
    for name, s in structs.items():
        fields = sorted(s["fields"])
        body, pos, align = [], 0, 1
        for off, t, fname, meaning in fields:
            base, dims, size, al = ctype(t, structs)
            if off % al:
                report["struct fields dropped"].append(f"{name}.{fname} at {off:#x}: not {al}-aligned for {t}")
                continue
            if off < pos:
                report["struct fields dropped"].append(f"{name}.{fname} at {off:#x}: overlaps the field before")
                continue
            if off + size > s["size"]:
                report["struct fields dropped"].append(f"{name}.{fname} at {off:#x}: runs past the struct")
                continue
            if off > pos:
                body.append(f"    u8 pad_{pos:X}[{off - pos:#x}];")
            body.append(f"    {base} {fname}{dims}; /* {off:#x} */" + (f" /* {meaning} */" if meaning else ""))
            pos, align = off + size, max(align, al)
        if s["size"] > pos:
            body.append(f"    u8 pad_{pos:X}[{s['size'] - pos:#x}];")
        s["align"] = align
        real = (s["size"] + align - 1) // align * align
        if real != s["size"]:
            report["struct size rounded by alignment"].append(f"{name} {s['size']:#x} -> {real:#x}")
        s["size"] = real
        report["structs"].append(f"{name} ({len(body)} lines, {len(fields)} fields)")
        if s["desc"]:
            out.append(f"/* {s['desc']} */")
        out += [f"typedef struct {name} {{"] + body + [f"}} {name};",
                f"typedef char {name}_size_check[sizeof({name}) == {real:#x} ? 1 : -1];", ""]
    out += ["#endif", ""]
    return "\n".join(out)


# ---------------------------------------------------------------- docs

def annotate(text: str, names) -> str:
    def sub(m):
        a = int(m[1], 16)
        return f"{m[0]} ({names[a][0]})" if names.get(a, (None,))[0] else m[0]
    return ADDR.sub(sub, text)


def claude_bullets(names) -> list[str]:
    """CLAUDE.md's bullets (with their continuation lines) that cite main.dol addresses."""
    out, cur = [], []
    for line in (REPO / "CLAUDE.md").read_text(encoding="utf-8").splitlines():
        if line.startswith("- ") or line.startswith("#") or not line.strip():
            if cur and any(ADDR.search(l) for l in cur):
                out += cur
            cur = [line] if line.startswith("- ") else []
        elif cur:
            cur.append(line)
    if cur and any(ADDR.search(l) for l in cur):
        out += cur
    return [annotate(l, names) for l in out]


def gen_docs(root: Path, ver: str, names, notes, sym_lines, sym_at) -> str:
    out = ["# Custom Robo: what is already known about main.dol", "",
           "GENERATED by `tools/decomp_seed.py` in the custom-robo-3ds repo (a 3DS port built from months of",
           "tracing this game); edit the tables there and re-run, not this file. Names in the table below are",
           "already in `config/" + ver + "/symbols.txt`; structs are in `include/custom_robo.h` (included by",
           "`common.h`). The behaviour is traced from the game's code, but the port reimplements it in its own",
           "style, so treat the C there as a description, not as matching source.", "",
           "## Named functions and data", "", "| address | name | what it is |", "|---|---|---|"]
    for a, (n, meaning) in sorted(names.items()):
        n = n or containing(sym_lines, sym_at, a) or "?"
        out.append(f"| `{a:#010x}` | `{n}` | {annotate(meaning, names).replace('|', '/')} |")
    out += ["", "## Notes on instructions and fields inside functions", "",
            "| address | in | note |", "|---|---|---|"]
    for a, _, note in sorted(notes):
        where = containing(sym_lines, sym_at, a) or "?"
        out.append(f"| `{a:#010x}` | `{where}` | {annotate(note, names).replace('|', '/')} |")
    out += ["", "## Research notes (from the port's CLAUDE.md, addresses annotated)", ""]
    out += claude_bullets(names)
    return "\n".join(out) + "\n"


AGENTS_BEGIN = "<!-- custom-robo-3ds seed: begin (generated by tools/decomp_seed.py) -->"
AGENTS_END = "<!-- custom-robo-3ds seed: end -->"


def agents_section(n_names: int, n_notes: int, ver: str) -> str:
    return "\n".join([
        AGENTS_BEGIN,
        "## Prior reverse engineering (Custom Robo 3DS port)",
        "",
        f"- `docs/custom_robo_re.md`: {n_names} named functions/tables with what each does, {n_notes} notes on",
        "  instructions inside functions, and the port's research notes. Read the rows for your function's",
        "  address and its callees before writing C; `search_code` over docs/ finds an address quickly.",
        "- `include/custom_robo.h` (included by `common.h`): layout-checked structs -- `Robot` (the per-player",
        "  struct, 0x33520 bytes, players at 0x8024e120 + n*0x33520) and the part stat tables. Use them instead",
        "  of raw pointer arithmetic; add fields there (keep the offset comments) as you learn them.",
        "  Robot code reads its fields through `gCurRobot` (a `Robot*`, set per player by robot_update_all):",
        "  `lwz rX, gCurRobot@sda21` then `lha r0, 0xe2(rX)` is `gCurRobot->timer`.",
        f"- Names follow `config/{ver}/symbols.txt`: functions snake_case with a subsystem prefix",
        "  (robot_, gun_, hit_, cam_, coll_, motion_, scr_ ...), data gCamelCase.",
        "- Compiler: mwcc GC/1.3.2 at `-opt level=0,peephole,schedule,speed` (no -O4): every named local gets",
        "  its own nonvolatile register even in leaf functions, dead expressions survive, and table loads are",
        "  addi/add/lwz 0 rather than lwzx. Write the C as literally as the assembly reads.",
        AGENTS_END])


def write_agents(root: Path, section: str):
    path = root / "AGENTS.md"
    text = path.read_text(encoding="utf-8") if path.exists() else "# Working on this decompilation\n\n"
    if AGENTS_BEGIN in text:
        text = re.sub(re.escape(AGENTS_BEGIN) + r".*?" + re.escape(AGENTS_END), lambda _: section, text, flags=re.S)
    else:
        text = text.rstrip("\n") + "\n\n" + section + "\n"
    path.write_text(text, encoding="utf-8")


def include_from_common(root: Path):
    path = root / "include" / "common.h"
    text = path.read_text(encoding="utf-8")
    line = '#include "custom_robo.h"'
    if line not in text:
        i = text.rindex("#endif")
        text = text[:i] + line + "\n\n" + text[i:]
        path.write_text(text, encoding="utf-8")


# ---------------------------------------------------------------- build and app

def ninja(root: Path) -> bool:
    exe = Path(os.environ.get("APPDATA", "")) / "uv" / "tools" / "decompnet" / "Scripts" / "ninja.exe"
    r = subprocess.run([str(exe) if exe.exists() else "ninja"], cwd=root, capture_output=True, text=True)
    tail = (r.stdout + r.stderr).strip().splitlines()[-6:]
    print("ninja:", *tail, sep="\n  ")
    return r.returncode == 0


def app_check(slug: str) -> bool:
    try:
        server = json.loads((APPDIR / "server.json").read_text(encoding="utf-8"))
    except OSError:
        print("decomp.wtf isn't running: skipped the app resync and notes")
        return False
    base, host = f"http://127.0.0.1:{server['port']}", f"127.0.0.1:{server['port']}"
    headers = {"Cookie": f"decomp_live={server['secret']}", "Host": host, "x-decomp": "1",
               "Content-Type": "application/json"}

    def call(path, data=None):
        req = urllib.request.Request(base + path, data=json.dumps(data).encode() if data is not None else None,
                                     headers=headers, method="POST" if data is not None else "GET")
        with urllib.request.urlopen(req, timeout=60) as resp:
            return json.loads(resp.read())
    try:
        job = call("/api/fork/check", {"slug": slug})["job"]
    except OSError as e:
        print(f"decomp.wtf didn't answer ({e}): skipped the app resync and notes")
        return False
    for _ in range(600):
        st = call(f"/api/job?id={job}")
        if st.get("state") != "running":
            print("app re-measure:", st.get("error") or (st.get("log") or ["done"])[-1])
            return st.get("state") == "done"
        time.sleep(1)
    print("app re-measure still running after 10 min")
    return False


class Mcp:
    """Minimal stdio MCP client for `decomp mcp`."""

    def __init__(self):
        self.p = subprocess.Popen(["decomp", "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, encoding="utf-8")
        self.n = 0
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                "clientInfo": {"name": "decomp_seed", "version": "1"}})
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def send(self, msg):
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()

    def rpc(self, method, params):
        self.n += 1
        self.send({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params})
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("decomp mcp exited")
            msg = json.loads(line)
            if msg.get("id") == self.n:
                if "error" in msg:
                    raise RuntimeError(msg["error"])
                return msg["result"]

    def tool(self, name, **args) -> str:
        res = self.rpc("tools/call", {"name": name, "arguments": args})
        text = "".join(c.get("text", "") for c in res.get("content", []))
        if res.get("isError"):
            raise RuntimeError(text)
        return text

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=30)


def existing_notes(slug: str) -> set[tuple[str | None, str]]:
    db = sqlite3.connect(f"file:{APPDIR / 'decomp.db'}?mode=ro", uri=True)
    rows = db.execute("SELECT f.name, n.text FROM notes n JOIN projects p ON p.id=n.project_id "
                      "LEFT JOIN functions f ON f.id=n.function_id WHERE p.slug=? AND n.text LIKE ?",
                      (slug, PREFIX + "%")).fetchall()
    db.close()
    return {(f, t) for f, t in rows}


def post_notes(slug: str, names, notes, sym_lines, sym_at, report):
    per_fn: dict[str, list[str]] = {}
    for a, (n, meaning) in sorted(names.items()):
        i = sym_at.get(a)
        if i is not None and "type:function" in sym_lines[i] and meaning:
            per_fn.setdefault(SYM.match(sym_lines[i])[1], []).append(meaning)
    for a, _, note in sorted(notes):
        where = containing(sym_lines, sym_at, a)
        if where:
            fn = where.split("+")[0]
            i = next((j for b, j in sym_at.items() if SYM.match(sym_lines[j])[1] == fn), None)
            if i is not None and "type:function" in sym_lines[i]:
                per_fn.setdefault(fn, []).append(f"{a:#010x}: {note}")
    have = existing_notes(slug)
    todo = []
    for fn, parts in per_fn.items():
        text = (f"{PREFIX} " + " | ".join(parts))[:3990]
        if (fn, text) in have:
            report["function notes already there"].append(fn)
        else:
            todo.append((fn, text))
    if not todo:
        return
    m = Mcp()
    try:
        m.tool("open_project", project=slug)
        for fn, text in todo:
            try:
                m.tool("add_note", function=fn, text=text)
                report["function notes posted"].append(fn)
            except RuntimeError as e:
                report["function notes refused"].append(f"{fn}: {str(e)[:120]}")
    finally:
        m.close()


# ---------------------------------------------------------------- main

def scan(names, notes):
    known = set(names) | {a for a, _, _ in notes}
    lines, at, _ = read_symbols(DECOMP / "config" / "GXCE01" / "symbols.txt")
    todo = {a: w for a, w in mentions().items() if a not in known}
    print(f"{len(todo)} main.dol addresses in our sources have no row in decomp_names/decomp_notes:")
    for a, where in sorted(todo.items()):
        kind = "starts " if a in at else "inside "
        print(f"  {a:#010x} {kind}{containing(lines, at, a) or '-':32} {where[0]} (+{len(where) - 1})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", nargs="?", default="apply", choices=["apply", "scan"])
    ap.add_argument("--root", type=Path, default=DECOMP)
    ap.add_argument("--version", default="GXCE01")
    ap.add_argument("--slug", default="main", help="decomp.wtf project")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--no-app", action="store_true", help="don't resync decomp.wtf or post notes")
    args = ap.parse_args()
    names, notes = load_names(), load_notes()
    if args.cmd == "scan":
        return scan(names, notes)

    report: dict[str, list[str]] = {k: [] for k in (
        "renamed", "already named", "kept (not ours to rename)", "no symbol at address",
        "name already used elsewhere", "sources rewritten", "structs", "struct fields dropped",
        "struct size rounded by alignment", "function notes posted", "function notes already there",
        "function notes refused")}
    root = args.root
    rename_symbols(root, args.version, names, report)
    (root / "include" / "custom_robo.h").write_text(gen_header(report), encoding="utf-8")
    include_from_common(root)
    sym_lines, sym_at, _ = read_symbols(root / "config" / args.version / "symbols.txt")
    (root / "docs").mkdir(exist_ok=True)
    (root / "docs" / "custom_robo_re.md").write_text(
        gen_docs(root, args.version, names, notes, sym_lines, sym_at), encoding="utf-8")
    write_agents(root, agents_section(len(names), len(notes), args.version))

    ok = True
    if not args.no_build:
        ok = ninja(root)
        if not ok:
            print("ninja FAILED: the decomp no longer builds byte for byte; not touching the app")
    if ok and not args.no_app and app_check(args.slug):
        post_notes(args.slug, names, notes, sym_lines, sym_at, report)

    for k, v in report.items():
        if v:
            shown = v if k in ("no symbol at address", "kept (not ours to rename)", "name already used elsewhere",
                               "struct fields dropped", "struct size rounded by alignment",
                               "function notes refused", "structs") else v[:3] + (["..."] if len(v) > 3 else [])
            print(f"{k}: {len(v)}" + "".join(f"\n  {x}" for x in shown))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

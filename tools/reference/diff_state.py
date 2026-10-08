"""Name the first differing fields of two game-state files.

    python tools/reference/diff_state.py A B [--all] [--max N] [--cells N] [--types mc_types.h]

A / B can be
  * save states / desync dumps (.mcs, "MCPSTATE": savegame.h savestate_save_file; the dumps of the
    MC_NET_SYNC parts exchange are <save dir>/desync_<exchange>_p<player>.mcs),
  * raw GameState images (0x38d03 bytes: movie/gamNNNNN.dat, the reference dumps tick%05d.gam - the
    original's pointer layout compares fine against itself),
  * a raw GameState followed by the maps (net_test MC_NET_DUMP: type, height, light, flags, u16 cell heads).

The field tables are generated at run time from src/mcengine/mc_types.h (and mode.h for the MODE chunk):
every `struct` between the #pragma pack(1) markers is parsed (scalars, arrays, nested structs, the
PlayerRec union - the typed PlayerBlock is used), offsets are computed and checked against the header's
`static_assert(sizeof(X) == N)` lines. So the tables follow the header; nothing is retyped here.

Output: the first difference in the state net_state_checksum hashes ("hashed"), then every differing field
grouped by area - Things by slot with class / type / state, players, the stacks, the maps by cell (x, y),
the globals (GLOB), the mode block, the pool extension. Fields the checksum leaves out on purpose (per
instance: Config, GameState.local_player, PlayerRec tick / messages / camera log / name, start_tick and the
HUD flash counters, Thing.flags bit 0 of the local flyer / spells, ...) are listed as "not hashed" (only
counted unless --all). Exit code: 0 = no hashed difference, 1 = differences, 2 = error.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TYPES_H = ROOT / "src" / "mcengine" / "mc_types.h"
MODE_H = ROOT / "src" / "mcengine" / "mode.h"

CLASSES = ["free", "c1", "scenery", "player", "c4", "creature", "c6", "c7", "c8", "projectile", "effect",
           "switch", "spell", "c13", "c14", "c15"]
SCALARS = {
    "uint8_t": (1, "<B"), "int8_t": (1, "<b"), "char": (1, "<b"), "bool": (1, "<B"),
    "uint16_t": (2, "<H"), "int16_t": (2, "<h"),
    "uint32_t": (4, "<I"), "int32_t": (4, "<i"), "float": (4, "<f"),
    "uint64_t": (8, "<Q"), "int64_t": (8, "<q"),
}
GAMESTATE_SIZE = 0x38D03
MAP_CELLS = 0x10000


# ---------------------------------------------------------------------------------------------------
# a small parser for the exact-layout headers
# ---------------------------------------------------------------------------------------------------
class Field:
    __slots__ = ("name", "off", "type", "dims", "size", "elem")

    def __init__(self, name, off, typ, dims, elem):
        self.name, self.off, self.type, self.dims, self.elem = name, off, typ, dims, elem
        n = 1
        for d in dims:
            n *= d
        self.size = elem * n


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def eval_expr(expr: str, structs) -> int | None:
    expr = expr.strip()
    expr = re.sub(r"sizeof\s*\(\s*(\w+)\s*\)", lambda m: str(structs[m.group(1)]["size"]) if m.group(1) in structs
                  else (str(SCALARS[m.group(1)][0]) if m.group(1) in SCALARS else "None"), expr)
    if "None" in expr or not re.fullmatch(r"[0-9a-fA-FxX+\-*/() ]+", expr):
        return None
    try:
        return int(eval(expr.replace("/", "//"), {"__builtins__": {}}))
    except Exception:
        return None


def expand_templates(src: str) -> str:
    """Round 11: `template <class C> struct XT { ... };` + `using A = XT<arg>;` -> a plain `struct A { ... };`
    appended to the source (the parameter replaced, nested `YT<arg>` uses replaced by their alias)."""
    tmpl = {}
    for m in re.finditer(r"template\s*<\s*class\s+(\w+)\s*>\s*struct\s+(\w+)\s*\{", src):
        body, _ = brace_body(src, m.end() - 1)
        tmpl[m.group(2)] = (m.group(1), body)
    if not tmpl:
        return src
    # each plain struct is inserted right after its `using` line, so the structs after it can use it
    inst = {}
    pieces, pos = [], 0
    for m in re.finditer(r"\busing\s+(\w+)\s*=\s*(\w+)\s*<\s*(\w+)\s*>\s*;", src):
        alias, name, arg = m.groups()
        if name not in tmpl:
            continue
        param, body = tmpl[name]
        body = re.sub(rf"\b{param}\b", arg, body)
        body = re.sub(r"\b(\w+)\s*<\s*(\w+)\s*>", lambda k: inst.get((k.group(1), k.group(2)), k.group(0)), body)
        inst[(name, arg)] = alias
        pieces.append(src[pos:m.end()])
        pieces.append(f"\nstruct {alias} {{{body}}};\n")
        pos = m.end()
    pieces.append(src[pos:])
    out = "".join(pieces)
    # the template definitions themselves are not structs of any layout
    for name in tmpl:
        out = re.sub(rf"template\s*<\s*class\s+\w+\s*>\s*struct\s+{name}\s*\{{", f"struct __template_{name} {{", out)
    return out


# Round 11: the port's in-memory Thing / Pos / GameState have int32 coordinates; save states, desync dumps and
# reference dumps hold the original's byte image (mc_image.h), so the tables use the image layouts under the
# canonical names.
IMAGE_ALIASES = {"ThingImage": "Thing", "PosImage": "Pos", "GameStateImage": "GameState"}


def use_image_layouts(structs):
    if not all(k in structs for k in IMAGE_ALIASES):
        return structs
    for s in structs.values():
        for f in s["fields"]:
            f.type = IMAGE_ALIASES.get(f.type, f.type)
    for img, name in IMAGE_ALIASES.items():
        structs[name] = structs[img]
    return structs


def parse_headers(paths):
    structs = {}       # name -> {"size": n, "fields": [Field]}
    asserts = []
    for path in paths:
        src = expand_templates(strip_comments(Path(path).read_text(encoding="utf-8", errors="replace")))
        pos = 0
        for m in re.finditer(r"static_assert\s*\(\s*sizeof\s*\(\s*(\w+)\s*\)\s*==\s*([^;]+?)\)\s*;", src):
            asserts.append((m.group(1), m.group(2)))
        while True:
            m = re.compile(r"\bstruct\s+(\w+)\s*\{").search(src, pos)
            if not m:
                break
            name = m.group(1)
            body, end = brace_body(src, m.end() - 1)
            pos = end
            fields, size = parse_body(body, structs)
            if fields is not None:
                structs[name] = {"size": size, "fields": fields}
    for name, expr in asserts:
        if name in structs:
            want = eval_expr(expr, structs)
            if want is not None and want != structs[name]["size"]:
                raise SystemExit(f"diff_state: parsed size of {name} = {structs[name]['size']:#x}, header says {want:#x}")
    return structs


def brace_body(src, open_at):
    depth = 0
    for i in range(open_at, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[open_at + 1:i], i + 1
    raise SystemExit("diff_state: unbalanced braces")


def split_statements(body):
    """Top-level statements of a struct body; a `union { ... };` stays one statement."""
    out, cur, depth = [], [], 0
    for ch in body:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        if ch == ";" and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    return [s for s in out if s]


def parse_member(stmt, structs):
    """'type a[2][3] = x, b' -> [(type, name, dims, elemsize)] or None when not understood."""
    stmt = re.sub(r"=\s*[^,]*", "", stmt)                 # default initialisers
    stmt = re.sub(r"\b(const|volatile|mutable)\b", "", stmt).strip()
    m = re.match(r"([A-Za-z_]\w*)\s+(.*)$", stmt, re.S)
    if not m:
        return None
    typ, rest = m.group(1), m.group(2)
    if typ in SCALARS:
        elem = SCALARS[typ][0]
    elif typ in structs:
        elem = structs[typ]["size"]
    else:
        return None
    res = []
    for decl in rest.split(","):
        decl = decl.strip()
        dm = re.match(r"(\w+)\s*((?:\[[^\]]*\]\s*)*)$", decl)
        if not dm:
            return None
        dims = []
        for d in re.findall(r"\[([^\]]*)\]", dm.group(2)):
            v = eval_expr(d, structs)
            if v is None:
                return None
            dims.append(v)
        res.append((typ, dm.group(1), dims, elem))
    return res


def parse_body(body, structs):
    fields, off = [], 0
    for stmt in split_statements(body):
        if not stmt.startswith("union") and (
                stmt.startswith(("static_assert", "static ", "constexpr", "enum", "using", "typedef")) or "(" in stmt):
            continue
        if stmt.startswith("union"):
            inner = stmt[stmt.index("{") + 1:stmt.rindex("}")]
            alts = []
            for s in split_statements(inner):
                mem = parse_member(s, structs)
                if mem is None:
                    return None, 0
                alts.extend(mem)
            if not alts:
                continue
            # the typed alternative names the bytes (PlayerRec: blk over the raw p[])
            best = max(alts, key=lambda a: (a[0] in structs, a[3] * prod(a[2])))
            size = max(a[3] * prod(a[2]) for a in alts)
            fields.append(Field(best[1], off, best[0], best[2], best[3]))
            off += size
            continue
        mem = parse_member(stmt, structs)
        if mem is None:
            return None, 0
        for typ, name, dims, elem in mem:
            f = Field(name, off, typ, dims, elem)
            fields.append(f)
            off += f.size
    return fields, off


def prod(dims):
    n = 1
    for d in dims:
        n *= d
    return n


# ---------------------------------------------------------------------------------------------------
# state files
# ---------------------------------------------------------------------------------------------------
def load_state(path):
    data = Path(path).read_bytes()
    chunks = {}
    if data[:8] == b"MCPSTATE":
        hsize = struct.unpack_from("<I", data, 12)[0]
        pos = hsize
        while pos + 8 <= len(data):
            tag = data[pos:pos + 4].decode("latin-1")
            n = struct.unpack_from("<I", data, pos + 4)[0]
            chunks[tag] = data[pos + 8:pos + 8 + n]
            pos += 8 + n
        chunks["_header"] = data[:hsize]
        return chunks
    if len(data) >= GAMESTATE_SIZE:
        chunks["GAME"] = data[:GAMESTATE_SIZE]
        rest = data[GAMESTATE_SIZE:]
        if len(rest) >= 4 * MAP_CELLS + 2 * MAP_CELLS:
            chunks["MAPS"] = rest[:6 * MAP_CELLS]
        return chunks
    raise SystemExit(f"diff_state: {path}: neither a save state nor a GameState image ({len(data)} bytes)")


class Diff:
    def __init__(self, path, a, b, hashed, note=""):
        self.path, self.a, self.b, self.hashed, self.note = path, a, b, hashed, note


def scalar(buf, off, typ):
    size, fmt = SCALARS[typ]
    if off + size > len(buf):
        return None
    return struct.unpack_from(fmt, buf, off)[0]


def fmt_val(v, typ):
    if isinstance(v, int) and typ.startswith("uint") and v > 9:
        return f"{v} ({v:#x})"
    return str(v)


class Differ:
    def __init__(self, structs, show_all):
        self.S = structs
        self.show_all = show_all
        self.diffs: list[Diff] = []

    # Generic struct walk. hashed(path) -> bool decides whether the field is in net_state_checksum.
    def walk(self, sname, a, b, base, prefix, hashed):
        for f in self.S[sname]["fields"]:
            off = base + f.off
            if a[off:off + f.size] == b[off:off + f.size]:
                continue
            path = f"{prefix}.{f.name}" if prefix else f.name
            n = prod(f.dims)
            if f.type in self.S:
                for i in range(n):
                    eo = off + i * f.elem
                    if a[eo:eo + f.elem] == b[eo:eo + f.elem]:
                        continue
                    p = path + index_suffix(f.dims, i) if f.dims else path
                    self.on_struct(f.type, a, b, eo, p, hashed)
            elif f.type == "char" and f.dims:
                sa = a[off:off + f.size].split(b"\0")[0].decode("latin-1")
                sb = b[off:off + f.size].split(b"\0")[0].decode("latin-1")
                self.add(path, repr(sa), repr(sb), hashed(path))
            else:
                for i in range(n):
                    eo = off + i * f.elem
                    if a[eo:eo + f.elem] == b[eo:eo + f.elem]:
                        continue
                    p = path + index_suffix(f.dims, i) if f.dims else path
                    self.add(p, fmt_val(scalar(a, eo, f.type), f.type), fmt_val(scalar(b, eo, f.type), f.type), hashed(p))

    def on_struct(self, sname, a, b, off, path, hashed):
        if sname == "Thing":
            self.thing(a, b, off, path, hashed)
        else:
            self.walk(sname, a, b, off, path, hashed)

    def add(self, path, va, vb, is_hashed, note=""):
        self.diffs.append(Diff(path, va, vb, is_hashed, note))

    def thing_desc(self, buf, off):
        cls = buf[off + self.toff("cls")]
        return f"{CLASSES[cls] if cls < 16 else cls} type {buf[off + self.toff('type')]} state {buf[off + self.toff('state')]}"

    def toff(self, name):
        for f in self.S["Thing"]["fields"]:
            if f.name == name:
                return f.off
        raise KeyError(name)

    def thing(self, a, b, off, path, hashed):
        da, db = self.thing_desc(a, off), self.thing_desc(b, off)
        note = f"({da})" if da == db else f"({da} / {db})"
        cls_a, type_a = a[off + self.toff("cls")], a[off + self.toff("type")]
        start = len(self.diffs)
        self.walk("Thing", a, b, off, path, hashed)
        for d in self.diffs[start:]:
            d.note = note
            # flags bit 0 of the local flyer / the spells is per instance (net.cpp)
            if d.path.endswith(".flags") and ((cls_a == 3 and type_a == 0) or cls_a == 12):
                fa = struct.unpack_from("<I", a, off + self.toff("flags"))[0]
                fb = struct.unpack_from("<I", b, off + self.toff("flags"))[0]
                if (fa ^ fb) == 1:
                    d.hashed = False


def index_suffix(dims, i):
    idx = []
    for d in reversed(dims):
        idx.append(i % d)
        i //= d
    return "".join(f"[{x}]" for x in reversed(idx))


# What net_state_checksum hashes (net.cpp state_checksum).
LOCAL_P = ("start_tick", "castle_hit_flash", "hit_flash", "damage_flash", "spell_flash")
HASHED_HEAD = ("win_timer", "status", "active", "index", "is_computer", "thing")


def game_hashed(path):
    top = re.match(r"(\w+)", path).group(1)
    if top in ("rng", "free_top", "free_list", "active_top", "active_list", "things"):
        return True
    if top == "players":
        m = re.match(r"players\[\d+\]\.(\w+)(?:\.(\w+))?", path)
        if not m:
            return False
        if m.group(1) in HASHED_HEAD:
            return True
        if m.group(1) == "blk":
            return m.group(2) not in LOCAL_P
        return False
    return False


MAP_NAMES = ["map.type", "map.height", "map.light", "map.flags"]
GLOB_NAMES = ["version", "g_rng16", "g_snapshot_things_base", "g_projectile_null_hit_index", "unused_943c4",
              "g_ai_human_wizard", "g_ai_rand_seed", "g_terrain_nearly_flat", "g_timer_ticks", "g_video_mode_flags",
              "dummy_block_size"]
GLOB_HASHED = {"g_rng16"}


def diff_maps(d: Differ, a, b, max_cells):
    for k, name in enumerate(MAP_NAMES + ["map.cells"]):
        if k < 4:
            sa, sb, w = a[k * MAP_CELLS:(k + 1) * MAP_CELLS], b[k * MAP_CELLS:(k + 1) * MAP_CELLS], 1
        else:
            sa, sb, w = a[4 * MAP_CELLS:6 * MAP_CELLS], b[4 * MAP_CELLS:6 * MAP_CELLS], 2
        if sa == sb:
            continue
        cells = [c for c in range(MAP_CELLS) if sa[c * w:(c + 1) * w] != sb[c * w:(c + 1) * w]]
        for c in cells[:max_cells]:
            va = int.from_bytes(sa[c * w:(c + 1) * w], "little")
            vb = int.from_bytes(sb[c * w:(c + 1) * w], "little")
            d.add(f"{name}[cell {c & 0xff},{c >> 8}]", str(va), str(vb), True)
        if len(cells) > max_cells:
            d.add(f"{name}", f"... {len(cells)} cells differ in all", "", True, "(more cells not listed)")
    rest_a, rest_b = a[6 * MAP_CELLS:], b[6 * MAP_CELLS:]
    if rest_a != rest_b:
        n = sum(1 for x, y in zip(rest_a, rest_b) if x != y)
        d.add("corner_tex_table", f"{n} bytes differ", "", False)


def diff_glob(d: Differ, a, b):
    for i, name in enumerate(GLOB_NAMES):
        if 4 * i + 4 > min(len(a), len(b)):
            break
        va, vb = struct.unpack_from("<I", a, 4 * i)[0], struct.unpack_from("<I", b, 4 * i)[0]
        if va != vb:
            d.add(f"GLOB.{name}", fmt_val(va, "uint32_t"), fmt_val(vb, "uint32_t"), name in GLOB_HASHED,
                  "(ai_seed part, not in the total)" if name == "g_ai_rand_seed" else "")
    base = 4 * len(GLOB_NAMES)
    if a[base:] != b[base:]:
        n = sum(1 for x, y in zip(a[base:], b[base:]) if x != y)
        d.add("GLOB.dummy_player_block", f"{n} bytes differ", "", False)


def diff_pool(d: Differ, a, b):
    if len(a) < 4 or len(b) < 4:
        d.add("POOL", f"{len(a)} bytes", f"{len(b)} bytes", True)
        return
    sa, sb = struct.unpack_from("<I", a, 0)[0], struct.unpack_from("<I", b, 0)[0]
    if sa != sb:
        d.add("POOL.slots", str(sa), str(sb), True)
        return
    tsz = d.S["Thing"]["size"]
    ext = sa - 1000
    for i in range(ext):
        off = 4 + i * tsz
        if a[off:off + tsz] != b[off:off + tsz]:
            d.thing(a, b, off, f"things[{1000 + i}]", lambda p: True)
    stacks = 4 + ext * tsz
    for k, name in enumerate(("ext_free_stack", "ext_active_stack")):
        o = stacks + k * ext * 4
        for i in range(ext):
            va, vb = a[o + 4 * i:o + 4 * i + 4], b[o + 4 * i:o + 4 * i + 4]
            if va != vb:
                d.add(f"POOL.{name}[{i}]", str(struct.unpack("<i", va)[0]), str(struct.unpack("<i", vb)[0]), True)


def line(x: Diff) -> str:
    head = f"{x.path} {x.note}" if x.note else x.path
    return f"{head}: {x.a} -> {x.b}" if x.b != "" else f"{head}: {x.a}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--all", action="store_true", help="list the differences the checksum leaves out as well")
    ap.add_argument("--max", type=int, default=60, help="list at most this many differences per section (default 60)")
    ap.add_argument("--cells", type=int, default=20, help="list at most this many cells per map (default 20)")
    ap.add_argument("--types", default=str(TYPES_H), help="mc_types.h to take the layouts from")
    args = ap.parse_args(argv)

    headers = [args.types] + ([str(MODE_H)] if MODE_H.exists() else [])
    S = use_image_layouts(parse_headers(headers))
    for need in ("GameState", "Thing", "PlayerRec", "Config"):
        if need not in S:
            print(f"diff_state: {need} not found in {args.types}", file=sys.stderr)
            return 2
    A, B = load_state(args.a), load_state(args.b)
    d = Differ(S, args.all)

    if "_header" in A and "_header" in B:
        ha, hb = A["_header"], B["_header"]
        la, lb = struct.unpack_from("<iI", ha, 20), struct.unpack_from("<iI", hb, 20)
        na = ha[52:].split(b"\0")[0].decode("latin-1")
        nb = hb[52:].split(b"\0")[0].decode("latin-1")
        print(f"A: {args.a}: level {la[0]} tick {la[1]}, '{na}'")
        print(f"B: {args.b}: level {lb[0]} tick {lb[1]}, '{nb}'")
    for tag in ("GAME", "CONF", "MAPS", "GLOB", "RULE", "MODE", "POOL", "CAMP"):
        ca, cb = A.get(tag), B.get(tag)
        if ca is None and cb is None:
            continue
        if (ca is None) != (cb is None):
            d.add(f"{tag} chunk", "present" if ca is not None else "missing", "present" if cb is not None else "missing",
                  tag in ("MODE", "POOL"))
            continue
        if ca == cb:
            continue
        if tag == "GAME":
            if len(ca) != S["GameState"]["size"] or len(cb) != len(ca):
                d.add("GAME", f"{len(ca)} bytes", f"{len(cb)} bytes", True)
            else:
                d.walk("GameState", ca, cb, 0, "", game_hashed)
        elif tag == "CONF":
            if len(ca) == S["Config"]["size"] == len(cb):
                d.walk("Config", ca, cb, 0, "Config", lambda p: False)
        elif tag == "MAPS":
            diff_maps(d, ca, cb, args.cells)
        elif tag == "GLOB":
            diff_glob(d, ca, cb)
        elif tag == "MODE":
            if "ModeState" in S and len(ca) == len(cb) == 4 + S["ModeState"]["size"]:
                d.walk("ModeState", ca[4:], cb[4:], 0, "mode", lambda p: True)
            else:
                d.add("MODE", f"{len(ca)} bytes", f"{len(cb)} bytes", True)
        elif tag == "POOL":
            diff_pool(d, ca, cb)
        elif tag == "RULE":
            d.add("RULE", ca.hex(), cb.hex(), True, "(gameplay rules differ)")
        else:
            d.add(tag, "differs", "", False)

    hashed = [x for x in d.diffs if x.hashed]
    local = [x for x in d.diffs if not x.hashed]
    if hashed:
        f = hashed[0]
        print(f"first difference (hashed): {line(f)}")
    else:
        print("no difference in the hashed state" + (f" ({len(local)} per-instance fields differ)" if local else ""))
    if hashed:
        print(f"\nhashed differences ({len(hashed)}):")
        for x in hashed[:args.max]:
            print(f"  {line(x)}")
        if len(hashed) > args.max:
            print(f"  ... {len(hashed) - args.max} more")
    if local:
        print(f"\nnot hashed (per instance / outside the checksum): {len(local)} field(s)" + ("" if args.all else " (--all lists them)"))
        if args.all:
            for x in local[:args.max]:
                print(f"  {line(x)}")
            if len(local) > args.max:
                print(f"  ... {len(local) - args.max} more")
    return 1 if hashed else 0


if __name__ == "__main__":
    sys.exit(main())

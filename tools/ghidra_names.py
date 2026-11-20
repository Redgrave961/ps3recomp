#!/usr/bin/env python3
import argparse
import json
import os
import re
import sys
from collections import defaultdict

GENERIC_RE = re.compile(r'^(FUN_|LAB_|SUB_|DAT_|thunk_FUN_|caseD_|switchD_|s_|u_)', re.I)
HEX_NAME_RE = re.compile(r'^(func_)?[0-9A-Fa-f]{6,8}$')
HINTY = re.compile(r'(::|\.cpp|\.c\b|\.h\b|/|\\|[A-Z][a-z]+[A-Z])')


def norm_addr(a):
    if isinstance(a, int):
        return f"0x{a & 0xFFFFFFFF:08X}"
    s = str(a)
    return f"0x{int(s, 16) & 0xFFFFFFFF:08X}"


def sanitize_ident(s, maxlen=48):
    s = s.strip()
    s = re.sub(r'\(.*$', '', s)
    s = s.replace('::', '__')
    s = re.sub(r'[^A-Za-z0-9_]', '_', s)
    s = re.sub(r'_+', '_', s).strip('_')
    if not s:
        return ""
    if s[0].isdigit():
        s = "_" + s
    return s[:maxlen]


def is_meaningful_name(name):
    if not name:
        return False
    base = name.rsplit(".", 1)[-1] if name.startswith(".") else name
    if GENERIC_RE.match(base):
        return False
    if HEX_NAME_RE.match(base):
        return False
    if re.search(r'(FUN_[0-9A-Fa-f]{6,}|LAB_[0-9A-Fa-f]{6,}|SUB_[0-9A-Fa-f]{6,})', name):
        return False
    return True


STOPWORDS = {
    "float", "int", "char", "bool", "void", "double", "short", "long", "uint",
    "string", "xml", "data", "name", "type", "value", "size", "count", "index",
    "output", "input", "error", "warning", "true", "false", "null", "none",
    "library", "binarydata", "animation", "scale", "weights", "buffer", "object",
    "node", "list", "array", "matrix", "vector", "color", "texture", "shader",
}


def string_hint(val):
    v = val.strip()
    if len(v) < 5 or len(v) > 200:
        return (0, "")

    m = re.search(r'([A-Za-z_]\w*(?:::[A-Za-z_]\w*)+)', v)
    if m:
        return (3, sanitize_ident(m.group(1)))

    if HINTY.search(v) and re.search(r'[/\\]', v):
        base = re.split(r'[/\\]', v)[-1]
        base = re.sub(r'\.(cpp|cxx|cc|c|h|hpp)$', '', base, flags=re.I)
        frag = sanitize_ident(base)
        if len(frag) >= 4 and frag.lower() not in STOPWORDS:
            return (2, frag)

    if re.fullmatch(r'[A-Za-z_]\w*', v) and "_" in v:
        if not v.isupper() and v.lower() not in STOPWORDS and len(v) >= 5:
            return (2, sanitize_ident(v))

    return (0, "")


def load(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser(description="Build name map from Ghidra JSON")
    ap.add_argument("ghidra_dir", help="Dir with functions.json/symbols.json/strings.json")
    ap.add_argument("-o", "--output", default=None, help="Output names.json")
    ap.add_argument("--min-string-len", type=int, default=4)
    args = ap.parse_args()

    fdir = args.ghidra_dir
    funcs = load(os.path.join(fdir, "functions.json"))
    func_addrs = {norm_addr(f["addr"]) for f in funcs}
    try:
        syms = load(os.path.join(fdir, "symbols.json"))
    except FileNotFoundError:
        syms = []
    try:
        strings = load(os.path.join(fdir, "strings.json"))
    except FileNotFoundError:
        strings = []

    sym_name = {}
    SRC_RANK = {"IMPORTED": 3, "USER_DEFINED": 3, "ANALYSIS": 2, "DEFAULT": 0}
    sym_rank = defaultdict(int)
    for s in syms:
        a = norm_addr(s["addr"])
        if a not in func_addrs:
            continue
        nm = s.get("name", "")
        if not is_meaningful_name(nm):
            continue
        rank = SRC_RANK.get(s.get("source", ""), 1) + (1 if s.get("primary") else 0)
        if rank > sym_rank[a]:
            sym_rank[a] = rank
            sym_name[a] = nm

    str_hint = defaultdict(list)
    for st in strings:
        conf, hint = string_hint(st.get("val", ""))
        if conf < 2 or not hint:
            continue
        refs = st.get("refs", [])
        if len(refs) > (8 if conf >= 3 else 3):
            continue
        for ref in refs:
            ra = norm_addr(ref)
            if ra in func_addrs:
                str_hint[ra].append((conf, hint, st.get("val", "")))

    ghidra_name = {norm_addr(f["addr"]): f.get("name", "") for f in funcs}

    names = {}
    used_idents = set()
    counts = defaultdict(int)

    def uniq(cident, addr):
        base = cident or "fn"
        cand = base
        if cand in used_idents:
            cand = f"{base}_{addr[2:]}"
        used_idents.add(cand)
        return cand

    for addr in sorted(func_addrs):
        label = None
        src = None
        if addr in sym_name:
            label, src = sym_name[addr], "symbol"
        elif is_meaningful_name(ghidra_name.get(addr, "")):
            label, src = ghidra_name[addr], "ghidra"
        elif addr in str_hint:
            cand = sorted(str_hint[addr], key=lambda t: (-t[0], len(t[1])))[0]
            label, src = cand[1], "string"
        if not label:
            continue
        cident = sanitize_ident(label)
        if not cident:
            continue
        cident = uniq(cident, addr)
        names[addr] = {"label": label, "cident": cident, "src": src}
        counts[src] += 1

    out = args.output or os.path.join(fdir, "names.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(names, f, indent=1, sort_keys=True)
    total = len(func_addrs)
    named = len(names)
    print(f"Functions: {total}")
    print(f"Named:     {named} ({100*named//max(total,1)}%)")
    for k in ("symbol", "ghidra", "string"):
        print(f"  via {k}: {counts[k]}")
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()

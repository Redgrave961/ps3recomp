#!/usr/bin/env python3
import argparse
import os
import re
import sys


PRIORITY_BUCKETS = [
    ("audio", ["audio"]),
    ("fs", ["fs", "savedata"]),
    ("gcm_video", ["gcm", "video", "rsx"]),
    ("sysutil", ["sysutil", "sysmodule", "userinfo"]),
]

SILENCING_CALL_RE = re.compile(r"\b(?:ps3_bswap(?:16|32|64)|vm_write(?:8|16|32|64))\s*\(")

SCALAR_PTR_TYPES = ("u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64",
                    "b8", "ps3_addr_t", "float", "double",
                    "unsigned char", "unsigned short", "unsigned int", "unsigned long long",
                    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
                    "int8_t", "int16_t", "int32_t", "int64_t")

EXCLUDED_PTR_TYPES = ("char", "void", "FILE", "u8")

STRUCT_PTR_RE = re.compile(r"^[A-Z][A-Za-z0-9_]*$")

DEFAULT_WHITELIST = os.path.join("tools", "endian_audit_whitelist.txt")


def strip_comments_and_strings(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            if j == -1:
                j = n
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            if j == -1:
                j = n
                span = text[i:j]
            else:
                j += 2
                span = text[i:j]
            out.append("".join(ch if ch == "\n" else " " for ch in span))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    j += 2
                else:
                    j += 1
            j = min(j + 1, n)
            span = text[i:j]
            out.append("".join(ch if ch == "\n" else " " for ch in span))
            i = j
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                if text[j] == "\\":
                    j += 2
                else:
                    j += 1
            j = min(j + 1, n)
            span = text[i:j]
            out.append("".join(ch if ch == "\n" else " " for ch in span))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


FUNC_HEAD_RE = re.compile(
    r"""^(?P<ret>[A-Za-z_][A-Za-z0-9_ \t]*?[ \t*])
        (?P<name>[A-Za-z_][A-Za-z0-9_]*)
        \s*\((?P<params>[^;{}]*?)\)
        \s*
        \{""",
    re.MULTILINE | re.VERBOSE | re.DOTALL,
)


def iter_toplevel_functions(clean_text):
    for m in FUNC_HEAD_RE.finditer(clean_text):
        ret = m.group("ret").strip()
        name = m.group("name")
        params = m.group("params")

        if ret.split()[-1] if ret.split() else "" in ("return", "if", "while", "for", "switch"):
            pass
        first_ret_tok = ret.split()[0] if ret.split() else ""
        if first_ret_tok in ("return", "if", "while", "for", "switch", "sizeof", "else"):
            continue
        if "static" in ret.split():
            continue
        if name in ("if", "for", "while", "switch", "sizeof"):
            continue

        brace_start = m.end() - 1
        depth = 0
        i = brace_start
        n = len(clean_text)
        body_end = None
        while i < n:
            ch = clean_text[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    body_end = i
                    break
            i += 1
        if body_end is None:
            continue

        yield name, ret, params, brace_start, body_end


def split_params(params_str):
    parts = []
    depth = 0
    cur = []
    for ch in params_str:
        if ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


PARAM_RE = re.compile(
    r"""^(?P<quals1>(?:const|volatile|struct|unsigned|signed)\s+)*
        (?P<base>[A-Za-z_][A-Za-z0-9_]*)
        \s*
        (?P<stars>\**)
        \s*
        (?P<qual_after>(?:const\s+)*)
        (?P<pname>[A-Za-z_][A-Za-z0-9_]*)?
        (?:\s*\[[^\]]*\])?
        $""",
    re.VERBOSE,
)


def classify_out_params(params_str):
    results = []
    if params_str.strip() in ("", "void"):
        return results

    for raw in split_params(params_str):
        raw = raw.strip()
        if not raw or raw == "void":
            continue
        if "(" in raw and "*" in raw.split("(")[0]:
            continue

        stripped = raw.strip()
        pointee_const = bool(re.match(r"^\s*const\b", stripped))

        m = PARAM_RE.match(stripped)
        if not m:
            continue
        stars = m.group("stars")
        if len(stars) != 1:
            continue
        base = m.group("base")

        if pointee_const:
            continue

        if base in EXCLUDED_PTR_TYPES:
            continue

        if base in SCALAR_PTR_TYPES:
            results.append((m.group("pname") or "?", base, False))
        elif STRUCT_PTR_RE.match(base):
            results.append((m.group("pname") or "?", base, False))
    return results


def store_through_pointer_re(pname):
    esc = re.escape(pname)
    return re.compile(
        r"(?:\*\s*" + esc + r"\s*(?:=[^=]|[+\-*/|&^]=)"
        r"|\b" + esc + r"\s*->\s*[A-Za-z_][A-Za-z0-9_]*\s*(?:=[^=]|[+\-*/|&^]=)"
        r"|\b" + esc + r"\s*\[[^\]]*\]\s*(?:=[^=]|[+\-*/|&^]=))"
    )


def module_priority(stem):
    low = stem.lower()
    for idx, (_bucket_name, needles) in enumerate(PRIORITY_BUCKETS):
        for needle in needles:
            if needle in low:
                return idx
    return len(PRIORITY_BUCKETS)


def load_whitelist(path):
    names = set()
    if not os.path.isfile(path):
        return names
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            name = re.split(r"\s+", line.strip(), maxsplit=1)[0]
            if name:
                names.add(name)
    return names


def scan_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        raw = f.read()
    clean = strip_comments_and_strings(raw)

    suspects = []
    for name, _ret, params_str, body_start, body_end in iter_toplevel_functions(clean):
        out_params = classify_out_params(params_str)
        if not out_params:
            continue

        body = clean[body_start:body_end + 1]

        if SILENCING_CALL_RE.search(body):
            continue

        stored_params = []
        for pname, ptype, _const in out_params:
            if pname == "?":
                continue
            if store_through_pointer_re(pname).search(body):
                stored_params.append((pname, ptype))

        if not stored_params:
            continue

        line_no = raw.count("\n", 0, body_start) + 1
        name_idx = raw.rfind(name, 0, body_start)
        if name_idx != -1:
            line_no = raw.count("\n", 0, name_idx) + 1

        suspects.append({
            "func": name,
            "params": ", ".join(f"{t}* {n}" for n, t in stored_params),
            "file": path,
            "line": line_no,
        })
    return suspects


def find_c_files(libs_root):
    out = []
    for dirpath, _dirnames, filenames in os.walk(libs_root):
        for fn in filenames:
            if fn.endswith(".c"):
                out.append(os.path.join(dirpath, fn))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(
        description="HLE out-param endianness heuristic auditor (INFO-level, advisory)."
    )
    ap.add_argument("--libs-root", default="libs", help="root to scan (default: libs)")
    ap.add_argument("--whitelist", default=DEFAULT_WHITELIST,
                    help="whitelist file (function names + reasons)")
    ap.add_argument("--module", default=None,
                    help="only scan files whose stem matches this substring")
    ap.add_argument("--top", type=int, default=10, help="how many top suspects to print")
    ap.add_argument("--verbose", action="store_true", help="print every suspect, not just top-N")
    args = ap.parse_args()

    if not os.path.isdir(args.libs_root):
        print(f"[endian_audit] libs root not found: {args.libs_root}", file=sys.stderr)
        sys.exit(0)

    whitelist = load_whitelist(args.whitelist)

    files = find_c_files(args.libs_root)
    if args.module:
        files = [f for f in files if args.module.lower() in os.path.basename(f).lower()]

    by_module = {}

    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        suspects = scan_file(path)
        kept = []
        for s in suspects:
            if s["func"] in whitelist:
                continue
            kept.append(s)
        if kept:
            by_module.setdefault(stem, []).extend(kept)

    module_order = sorted(by_module.keys(), key=lambda s: (module_priority(s), s.lower()))

    print("=" * 78)
    print("endian_audit.py -- HLE out-param endianness heuristic (INFO-level, advisory)")
    print(f"Scanned {len(files)} file(s) under '{args.libs_root}'"
          + (f" (module filter: {args.module})" if args.module else ""))
    print(f"Whitelist: {args.whitelist} ({len(whitelist)} entries loaded)")
    print("Rule: exported fn has a non-const scalar/struct pointer out-param that is")
    print("      STORED THROUGH in the body, with NO ps3_bswap*/vm_write* call anywhere")
    print("      in that function body.")
    print("=" * 78)

    total_suspects = sum(len(v) for v in by_module.values())
    print(f"\nTotal suspects (post-whitelist): {total_suspects}")
    print("\nPer-module suspect counts (boot-relevance order):")
    for stem in module_order:
        bucket_idx = module_priority(stem)
        bucket_name = PRIORITY_BUCKETS[bucket_idx][0] if bucket_idx < len(PRIORITY_BUCKETS) else "other"
        print(f"  [{bucket_name:9s}] {stem:24s} {len(by_module[stem]):4d}")

    flat = []
    for stem in module_order:
        for s in by_module[stem]:
            flat.append(s)

    print(f"\nTop {args.top if not args.verbose else len(flat)} suspects:")
    show = flat if args.verbose else flat[:args.top]
    for s in show:
        print(f"  {s['file']}:{s['line']}  {s['func']}({s['params']})")

    if not flat:
        print("  (none)")

    print()
    print("Advisory tool -- exit code is always 0.")
    sys.exit(0)


if __name__ == "__main__":
    main()

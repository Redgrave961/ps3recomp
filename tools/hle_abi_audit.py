#!/usr/bin/env python3
"""hle_abi_audit.py -- differential ABI-signature audit for HLE functions.

Same technique as the PPU/SPU decode conformance audits, applied one layer up:
instead of diffing our instruction lifter against the PowerISA reference, this
diffs our HLE C function signatures (libs/**/*.c) against RPCS3's real,
shipped implementation of the same PS3 module functions
(rpcs3/rpcs3/Emu/Cell/Modules/*.cpp).

This is the bug class that produced blocker #16 (cellGcmGetTimeStampLocation),
#18 (CellFsStat), #19d, and cellGcmGetTiledPitchSize: we quietly turn a
function that PS3 games expect to *return* a value into one that writes an
out-param and returns an error code (or vice versa), or we drop/add a
parameter. Those are silent ABI mismatches -- they compile fine, and only
misbehave at runtime when a caller reads the return register/parameter that
we didn't populate the way it expects.

RPCS3 (rpcs3/ in this repo) is GPLv2. We read ONLY the function signature
(return type + parameter types) as ground truth for the real PS3 ABI --
never copy its function bodies. This repo stays MIT/clean-room.

Usage:
    py -3 tools/hle_abi_audit.py [--libs DIR] [--oracle DIR] [--whitelist FILE]
                                  [--json OUT.json]

Stdlib-only, py -3, LF line endings, 4-space indent.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field


MODULE_PRIORITY = [
    "cellGcmSys",
    "cellVideoOut",
    "sys_fs",
    "cellAudio",
    "cellSysutil",
    "cellSysmodule",
]


def module_rank(module: str) -> tuple:
    try:
        return (0, MODULE_PRIORITY.index(module))
    except ValueError:
        return (1, module)


NAME_PREFIXES = ("cell", "sce", "sys_")

_IDENT = r"[A-Za-z_][A-Za-z0-9_]*"


@dataclass
class Param:
    raw_type: str
    name: str
    is_pointer: bool
    base_type: str


@dataclass
class Signature:
    name: str
    return_raw: str
    return_is_pointer: bool
    return_base: str
    params: list = field(default_factory=list)
    file: str = ""
    line: int = 0
    return_is_status_by_body: object = None

    def param_sig(self) -> str:
        return ", ".join(f"{p.raw_type} {p.name}".strip() for p in self.params)

    def pretty(self) -> str:
        return f"{self.return_raw} {self.name}({self.param_sig()})"


_INT_ALIASES = {
    "u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64",
    "int", "unsigned", "unsigned int", "unsigned char", "unsigned short",
    "unsigned long", "unsigned long long", "long", "long long",
    "short", "char", "signed char", "bool", "size_t", "ssize_t",
    "float", "f32", "f64", "double",
    "be_t<u8>", "be_t<u16>", "be_t<u32>", "be_t<u64>",
    "be_t<s8>", "be_t<s16>", "be_t<s32>", "be_t<s64>",
}

ERROR_RETURN_TYPES = {
    "error_code", "cellerror", "cellfserrno",
}

AMBIGUOUS_STATUS_TYPES = {"s32", "int"}

VOID_TYPES = {"void"}


def strip_cv(t: str) -> str:
    t = t.strip()
    t = re.sub(r"\bconst\b", "", t)
    t = re.sub(r"\bvolatile\b", "", t)
    t = re.sub(r"\s+", " ", t).strip()
    return t


def normalize_base(t: str) -> str:
    t = strip_cv(t)
    t = t.replace("*", "").strip()
    t = re.sub(r"\s+", " ", t)

    m = re.match(r"^vm::(?:ptr|cptr|bptr|pptr)<\s*(.+?)\s*>$", t)
    if m:
        t = m.group(1)
        t = strip_cv(t)

    m = re.match(r"^be_t<\s*(.+?)\s*>$", t)
    if m:
        t = m.group(1)

    t = t.lower()
    if t in ("unsigned",):
        t = "unsigned int"
    return t


OUR_FNPTR_TYPEDEFS: set = set()

ORACLE_PTR_ALIASES: set = set()

OUR_HANDLE_TYPEDEFS: set = set()

_HANDLE_NAME_RE = re.compile(r"^(Cell|Sce)[A-Za-z0-9]*Handle$")


def is_pointer_type(t: str) -> bool:
    t = strip_cv(t)
    bare = t.replace("*", "").strip()
    if bare in OUR_FNPTR_TYPEDEFS or bare in ORACLE_PTR_ALIASES:
        return True
    if re.match(r"^vm::(?:ptr|cptr|bptr|pptr)<", t):
        return True
    if "*" in t:
        return True
    return False


def is_handle_typedef(t: str) -> bool:
    t = strip_cv(t).replace("*", "").strip()
    return t in OUR_HANDLE_TYPEDEFS or bool(_HANDLE_NAME_RE.match(t))


def make_param(raw_type: str, name: str) -> Param:
    raw_type = strip_cv(raw_type)
    return Param(
        raw_type=raw_type,
        name=name,
        is_pointer=is_pointer_type(raw_type),
        base_type=normalize_base(raw_type),
    )


_RETURN_STMT_RE = re.compile(r"return\s+([^;]*?)\s*;")
_STATUS_LITERAL_RE = re.compile(
    r"^\(?\s*(?:\([A-Za-z_][A-Za-z0-9_]*\)\s*)?"
    r"(?:CELL_OK"
    r"|CELL_[A-Za-z0-9_]*_ERROR[A-Za-z0-9_]*"
    r"|CELL_E[A-Z][A-Z0-9_]*"
    r"|SCE_[A-Za-z0-9_]*_ERROR[A-Za-z0-9_]*"
    r")\s*\)?$"
)


def find_matching_brace(text: str, open_brace_pos: int) -> int:
    depth = 0
    i = open_brace_pos
    n = len(text)
    while i < n:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return n - 1


def body_looks_like_status_return(body: str) -> bool:
    returns = [m.group(1).strip() for m in _RETURN_STMT_RE.finditer(body)]
    if not returns:
        return False
    return all(_STATUS_LITERAL_RE.match(r) for r in returns)


def classify_return(raw: str) -> tuple:
    raw = strip_cv(raw)
    if is_pointer_type(raw):
        return True, "pointer"
    base = normalize_base(raw)
    if base in VOID_TYPES:
        return False, "void"
    if base in ERROR_RETURN_TYPES:
        return False, "error"
    if base in AMBIGUOUS_STATUS_TYPES:
        return False, "ambiguous-status"
    if base in _INT_ALIASES:
        return False, "value"
    return False, f"value:{base}"


_OUR_FNPTR_TYPEDEF_RE = re.compile(
    r"typedef\s+[A-Za-z_][A-Za-z0-9_\s]*?\(\s*\*\s*(" + _IDENT + r")\s*\)\s*\("
)

_OUR_HANDLE_TYPEDEF_RE = re.compile(
    r"typedef\s+(?:u8|u16|u32|u64|s8|s16|s32|s64|int|unsigned\s+int)\s+"
    r"(" + _IDENT + r")\s*;"
)

_ORACLE_PTR_ALIAS_RE = re.compile(
    r"^using\s+(" + _IDENT + r")\s*=\s*vm::(?:ptr|cptr|bptr|pptr)\s*<",
    re.MULTILINE,
)


def scan_our_fnptr_typedefs(root: str) -> set:
    names = set()
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if not (fn.endswith(".h") or fn.endswith(".c")):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            for m in _OUR_FNPTR_TYPEDEF_RE.finditer(text):
                names.add(m.group(1))
    return names


def scan_our_handle_typedefs(root: str) -> set:
    names = set()
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if not fn.endswith(".h"):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            for m in _OUR_HANDLE_TYPEDEF_RE.finditer(text):
                name = m.group(1)
                if _HANDLE_NAME_RE.match(name):
                    names.add(name)
    return names


def scan_oracle_ptr_aliases(root: str) -> set:
    names = set()
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if not (fn.endswith(".cpp") or fn.endswith(".h")):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            for m in _ORACLE_PTR_ALIAS_RE.finditer(text):
                names.add(m.group(1))
    return names


def _split_top_level_commas(s: str):
    parts = []
    depth = 0
    cur = []
    for ch in s:
        if ch in "<(":
            depth += 1
            cur.append(ch)
        elif ch in ">)":
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


def parse_params(params_str: str) -> list:
    params_str = params_str.strip()
    if params_str == "" or params_str == "void":
        return []
    out = []
    for part in _split_top_level_commas(params_str):
        part = part.strip()
        if not part:
            continue
        part = re.split(r"=", part, maxsplit=1)[0].strip()
        m = re.match(r"^(.+?\(\s*\*\s*)(" + _IDENT + r")(\s*\).*)$", part)
        if m:
            raw_type = (m.group(1) + m.group(3)).strip()
            name = m.group(2)
            out.append(make_param(raw_type, name))
            continue
        m = re.match(r"^(.*?[\s\*])(" + _IDENT + r")$", part)
        if m:
            raw_type = m.group(1).strip()
            name = m.group(2)
        else:
            raw_type = part
            name = ""
        out.append(make_param(raw_type, name))
    return out


def extract_ours(root: str) -> dict:
    sigs = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if not fn.endswith(".c"):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue

            text_nc = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
            text_nc = re.sub(r"//[^\n]*", "", text_nc)

            for m in re.finditer(
                r"^(?P<static>static\s+)?"
                r"(?P<ret>[A-Za-z_][A-Za-z0-9_:<>\s\*]*?)\s+"
                r"(?P<name>" + _IDENT + r")\s*\(\s*(?P<params>[^;{}]*?)\s*\)\s*\n?\s*\{",
                text_nc,
                flags=re.MULTILINE,
            ):
                if m.group("static"):
                    continue
                name = m.group("name")
                if not name.startswith(NAME_PREFIXES):
                    continue
                if name in ("sys_fs",):
                    continue
                ret_raw = m.group("ret").strip()
                if ret_raw in ("return", "else", "if", "while", "for", "switch"):
                    continue
                params_raw = m.group("params")
                line_no = text_nc.count("\n", 0, m.start()) + 1
                is_ptr, base_kind = classify_return(ret_raw)

                status_by_body = None
                if base_kind == "ambiguous-status":
                    open_brace = m.end() - 1
                    close_brace = find_matching_brace(text_nc, open_brace)
                    body = text_nc[open_brace:close_brace + 1]
                    status_by_body = body_looks_like_status_return(body)

                sig = Signature(
                    name=name,
                    return_raw=ret_raw,
                    return_is_pointer=is_ptr,
                    return_base=base_kind,
                    params=parse_params(params_raw),
                    file=os.path.relpath(path, root_start(root)),
                    line=line_no,
                    return_is_status_by_body=status_by_body,
                )
                if name not in sigs:
                    sigs[name] = sig
    return sigs


def root_start(root: str) -> str:
    return os.path.dirname(os.path.normpath(root))


REG_FUNC_RE = re.compile(
    r"REG_FUNC\s*\(\s*(?P<module>" + _IDENT + r")\s*,\s*(?P<name>" + _IDENT + r")\s*\)"
)


def extract_oracle(root: str) -> tuple:
    sigs = {}
    name_to_module = {}

    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if not fn.endswith(".cpp"):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue

            for m in REG_FUNC_RE.finditer(text):
                name_to_module.setdefault(m.group("name"), m.group("module"))

            text_nc = re.sub(r"/\*.*?\*/", lambda mm: re.sub(r"[^\n]", " ", mm.group(0)), text, flags=re.DOTALL)
            text_nc = re.sub(r"//[^\n]*", "", text_nc)

            for m in re.finditer(
                r"^(?P<ret>[A-Za-z_][A-Za-z0-9_:<>\s\*&,]*?)\s+"
                r"(?P<name>" + _IDENT + r")\s*\(\s*(?P<params>[^;{}]*?)\s*\)\s*\n?\s*\{",
                text_nc,
                flags=re.MULTILINE,
            ):
                name = m.group("name")
                if not name.startswith(NAME_PREFIXES):
                    continue
                if "::" in m.group("ret"):
                    pass
                ret_raw = m.group("ret").strip()
                if ret_raw in ("return", "else", "if", "while", "for", "switch", "template"):
                    continue
                pre_start = max(0, m.start("name") - 3)
                if text_nc[pre_start:m.start("name")].endswith("::"):
                    continue
                params_raw = m.group("params")
                line_no = text_nc.count("\n", 0, m.start()) + 1

                params = parse_params(params_raw)
                if params and re.match(r"^(ppu_thread|cpu_thread|spu_thread)\s*&$",
                                        strip_cv(params[0].raw_type)):
                    params = params[1:]

                is_ptr, base_kind = classify_return(ret_raw)
                sig = Signature(
                    name=name,
                    return_raw=ret_raw,
                    return_is_pointer=is_ptr,
                    return_base=base_kind,
                    params=params,
                    file=os.path.relpath(path, root_start_oracle(root)),
                    line=line_no,
                )
                if name not in sigs:
                    sigs[name] = sig
    return sigs, name_to_module


def root_start_oracle(root: str) -> str:
    return root


@dataclass
class Mismatch:
    name: str
    module: str
    kind: str
    detail: str
    ours: Signature
    theirs: Signature


def diff_signatures(ours: Signature, theirs: Signature) -> list:
    problems = []

    ours_kind = ours.return_base.split(":")[0]
    theirs_kind = theirs.return_base.split(":")[0]

    def _effective_kind(sig: Signature, k: str, other_k: str) -> str:
        if k == "ambiguous-status":
            if sig.return_is_status_by_body:
                return "error"
            return "error" if other_k == "void" else "value"
        return k

    ours_eff = _effective_kind(ours, ours_kind, theirs_kind)
    theirs_eff = _effective_kind(theirs, theirs_kind, ours_kind)

    if ours.return_is_pointer != theirs.return_is_pointer:
        problems.append((
            "return-kind",
            f"pointer-return mismatch: ours={'ptr' if ours.return_is_pointer else 'value'} "
            f"({ours.return_raw}) vs RPCS3={'ptr' if theirs.return_is_pointer else 'value'} "
            f"({theirs.return_raw})",
        ))
    elif ours_eff != theirs_eff:
        problems.append((
            "return-kind",
            f"ours returns {ours_eff} ({ours.return_raw}) but RPCS3 returns "
            f"{theirs_eff} ({theirs.return_raw})",
        ))

    count_differs = len(ours.params) != len(theirs.params)
    if count_differs:
        problems.append((
            "param-count",
            f"ours has {len(ours.params)} param(s), RPCS3 has {len(theirs.params)}",
        ))

    for i in range(min(len(ours.params), len(theirs.params))):
        po, pt = ours.params[i], theirs.params[i]
        if po.is_pointer != pt.is_pointer:
            if (not po.is_pointer and is_handle_typedef(po.raw_type) and pt.is_pointer):
                continue
            tag = " (unaligned -- param count differs, position may not correspond)" if count_differs else ""
            problems.append((
                "param-pointer-vs-value",
                f"param #{i + 1}: ours `{po.raw_type} {po.name}` is "
                f"{'a pointer' if po.is_pointer else 'a value'} but RPCS3's "
                f"`{pt.raw_type}` is {'a pointer' if pt.is_pointer else 'a value'}{tag}",
            ))

    return problems


def load_whitelist(path: str) -> dict:
    entries = {}
    if not os.path.isfile(path):
        return entries
    with open(path, "r", encoding="utf-8") as f:
        for raw_line in f:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if ":" not in line:
                continue
            name, reason = line.split(":", 1)
            entries[name.strip()] = reason.strip()
    return entries


def build_report(libs_root: str, oracle_root: str, whitelist_path: str):
    global OUR_FNPTR_TYPEDEFS, ORACLE_PTR_ALIASES, OUR_HANDLE_TYPEDEFS
    OUR_FNPTR_TYPEDEFS = scan_our_fnptr_typedefs(libs_root)
    ORACLE_PTR_ALIASES = scan_oracle_ptr_aliases(oracle_root)
    OUR_HANDLE_TYPEDEFS = scan_our_handle_typedefs(libs_root)

    ours = extract_ours(libs_root)
    theirs, name_to_module = extract_oracle(oracle_root)
    whitelist = load_whitelist(whitelist_path)

    mismatches = []
    matched = []
    ours_only = []
    theirs_only = []

    for name, our_sig in sorted(ours.items()):
        their_sig = theirs.get(name)
        if their_sig is None:
            ours_only.append(our_sig)
            continue
        problems = diff_signatures(our_sig, their_sig)
        if problems:
            if name in whitelist:
                continue
            module = name_to_module.get(name, "?")
            for kind, detail in problems:
                mismatches.append(Mismatch(name, module, kind, detail, our_sig, their_sig))
        else:
            matched.append(name)

    for name, their_sig in sorted(theirs.items()):
        if name not in ours:
            theirs_only.append(their_sig)

    mismatches.sort(key=lambda m: (module_rank(m.module), m.name, m.kind))

    return {
        "ours_total": len(ours),
        "theirs_total": len(theirs),
        "matched": matched,
        "mismatches": mismatches,
        "ours_only": ours_only,
        "theirs_only": theirs_only,
        "whitelist": whitelist,
    }


def format_report(report: dict) -> str:
    lines = []
    lines.append("HLE ABI audit -- ours (libs/**/*.c) vs RPCS3 oracle (rpcs3/.../Modules/*.cpp)")
    lines.append(f"functions parsed: ours={report['ours_total']}  RPCS3={report['theirs_total']}")
    lines.append(f"name overlap checked: {len(report['matched']) + len({m.name for m in report['mismatches']})}")
    lines.append(f"matched (ABI-compatible): {len(report['matched'])}")
    lines.append(f"MISMATCHES: {len({m.name for m in report['mismatches']})} function(s), "
                 f"{len(report['mismatches'])} finding(s)")
    lines.append("")

    if report["mismatches"]:
        lines.append("=" * 78)
        lines.append("MISMATCHES (grouped by module, boot-relevance order)")
        lines.append("=" * 78)
        current_module = None
        for m in report["mismatches"]:
            if m.module != current_module:
                current_module = m.module
                lines.append("")
                lines.append(f"-- module: {current_module} --")
            lines.append(f"[{m.kind}] {m.name}")
            lines.append(f"    ours   : {m.ours.pretty()}")
            lines.append(f"             ({m.ours.file}:{m.ours.line})")
            lines.append(f"    RPCS3  : {m.theirs.pretty()}")
            lines.append(f"             ({m.theirs.file}:{m.theirs.line})")
            lines.append(f"    reason : {m.detail}")
            lines.append("")
    else:
        lines.append("(no mismatches)")

    lines.append("=" * 78)
    lines.append(f"INFO: ours-only (RPCS3 doesn't implement/we couldn't match) = {len(report['ours_only'])}")
    lines.append(f"INFO: RPCS3-only (we don't implement) = {len(report['theirs_only'])}")
    lines.append("=" * 78)

    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(here)
    parser.add_argument("--libs", default=os.path.join(repo_root, "libs"),
                         help="root of our HLE C sources (default: libs/)")
    parser.add_argument("--oracle", default=os.path.join(repo_root, "rpcs3", "rpcs3", "Emu", "Cell", "Modules"),
                         help="root of RPCS3's Modules/ (default: rpcs3/rpcs3/Emu/Cell/Modules)")
    parser.add_argument("--whitelist", default=os.path.join(here, "hle_abi_audit_whitelist.txt"),
                         help="known-benign name:reason list")
    parser.add_argument("--json", default=None, help="also write a JSON report to this path")
    args = parser.parse_args(argv)

    if not os.path.isdir(args.libs):
        print(f"error: libs root not found: {args.libs}", file=sys.stderr)
        return 2
    if not os.path.isdir(args.oracle):
        print(f"error: oracle root not found: {args.oracle}", file=sys.stderr)
        return 2

    report = build_report(args.libs, args.oracle, args.whitelist)
    print(format_report(report))

    if args.json:
        def sig_json(s: Signature) -> dict:
            return {
                "name": s.name,
                "return_type": s.return_raw,
                "params": [{"type": p.raw_type, "name": p.name} for p in s.params],
                "file": s.file,
                "line": s.line,
            }

        payload = {
            "ours_total": report["ours_total"],
            "theirs_total": report["theirs_total"],
            "matched": report["matched"],
            "mismatches": [
                {
                    "name": m.name,
                    "module": m.module,
                    "kind": m.kind,
                    "detail": m.detail,
                    "ours": sig_json(m.ours),
                    "rpcs3": sig_json(m.theirs),
                }
                for m in report["mismatches"]
            ],
            "ours_only": [sig_json(s) for s in report["ours_only"]],
            "theirs_only": [sig_json(s) for s in report["theirs_only"]],
        }
        with open(args.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(payload, f, indent=2)
            f.write("\n")

    return 1 if report["mismatches"] else 0


if __name__ == "__main__":
    sys.exit(main())

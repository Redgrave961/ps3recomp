from __future__ import annotations

import argparse
import bisect
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from elf_parser import ELFFile
import dwarf_common as dw
from dwarf_common import uleb, sleb, cstr


def parse_one_unit(data: bytes, o: int):
    if o + 4 <= len(data):
        unit_len = struct.unpack_from(">I", data, o)[0]
        end = o + 4 + unit_len
        version = struct.unpack_from(">H", data, o + 4)[0]
        header_len = struct.unpack_from(">I", data, o + 6)[0]
        prog_start = o + 10 + header_len
        p = o + 10
        min_inst = data[p]; p += 1
        if version >= 4:
            p += 1
        default_is_stmt = data[p]; p += 1
        line_base = struct.unpack_from(">b", data, p)[0]; p += 1
        line_range = data[p]; p += 1
        opcode_base = data[p]; p += 1
        std_lengths = list(data[p:p + opcode_base - 1]); p += opcode_base - 1

        include_dirs = [""]
        while data[p] != 0:
            s, p = cstr(data, p); include_dirs.append(s)
        p += 1
        files = [("", 0)]
        while data[p] != 0:
            name, p = cstr(data, p)
            dir_idx, p = uleb(data, p)
            _mtime, p = uleb(data, p)
            _size, p = uleb(data, p)
            files.append((name, dir_idx))
        p += 1

        def filepath(idx):
            if idx >= len(files):
                return f"<file{idx}>"
            name, di = files[idx]
            d = include_dirs[di] if di < len(include_dirs) else ""
            return f"{d}/{name}" if d and not name.startswith(("/", "\\")) and ":" not in name else name

        addr = 0; file = 1; line = 1; p = prog_start
        addr_size = 4
        while p < end:
            op = data[p]; p += 1
            if op >= opcode_base:
                adj = op - opcode_base
                addr += (adj // line_range) * min_inst
                line += line_base + (adj % line_range)
                yield (addr, filepath(file), line)
            elif op == 0:
                length, p = uleb(data, p)
                sub = data[p]
                if sub == 1:
                    yield (addr, None, 0)
                    addr = 0; file = 1; line = 1
                elif sub == 2:
                    addr = struct.unpack_from(">I" if addr_size == 4 else ">Q", data, p + 1)[0]
                p += length
            else:
                if op == 1:
                    yield (addr, filepath(file), line)
                elif op == 2:
                    v, p = uleb(data, p); addr += v * min_inst
                elif op == 3:
                    v, p = sleb(data, p); line += v
                elif op == 4:
                    file, p = uleb(data, p)
                elif op == 5:
                    _v, p = uleb(data, p)
                elif op == 8:
                    addr += ((255 - opcode_base) // line_range) * min_inst
                elif op == 9:
                    addr += struct.unpack_from(">H", data, p)[0]; p += 2
                elif op in (6, 7, 10, 11):
                    pass
                else:
                    for _ in range(std_lengths[op - 1]):
                        _v, p = uleb(data, p)


def _stmt_list_offsets(elf) -> list[int]:
    dinfo = dw.open_dwarf(elf)
    if dinfo is None:
        return []
    offs = []
    for _off, tag, vals, _depth in dinfo.dies():
        if tag == dw.DW_TAG_compile_unit and dw.DW_AT_stmt_list in vals:
            offs.append(vals[dw.DW_AT_stmt_list])
    return offs


def build_table(elf):
    sec = {s.name_str: i for i, s in enumerate(elf.section_headers) if s.name_str}
    if ".debug_line" not in sec:
        raise SystemExit("no .debug_line (stripped / retail build?)")
    data = elf.get_section_data(sec[".debug_line"])
    rows = set()
    for o in _stmt_list_offsets(elf):
        try:
            rows.update(parse_one_unit(data, o))
        except (struct.error, IndexError):
            continue
    return sorted(rows, key=lambda r: (r[0], r[1] is None, r[1] or "", r[2]))


def resolve(rows, addr):
    addrs = [r[0] for r in rows]
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0 or rows[i][1] is None:
        return None
    return rows[i]


def main() -> int:
    ap = argparse.ArgumentParser(description="Resolve code addresses to source:line via DWARF")
    ap.add_argument("input", help="Unstripped ELF (run unfself.py on an EBOOT.BIN first)")
    ap.add_argument("addrs", nargs="*", help="Addresses to resolve (hex, e.g. 0x4F4A98)")
    ap.add_argument("--dump", action="store_true", help="Print the whole address->line table")
    args = ap.parse_args()

    elf = ELFFile(args.input); elf.load()
    rows = build_table(elf)
    print(f"[dwarf_line] {len(rows)} address rows", file=sys.stderr)

    if args.dump:
        for addr, fp, line in rows:
            if fp is not None:
                print(f"0x{addr:08X}  {fp}:{line}")
        return 0

    for a in args.addrs:
        addr = int(a, 16)
        r = resolve(rows, addr)
        if r:
            print(f"0x{addr:08X} -> {r[1]}:{r[2]}")
        else:
            print(f"0x{addr:08X} -> (no line info)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

from __future__ import annotations

import struct

DW_TAG_compile_unit = 0x11
DW_TAG_structure_type = 0x13
DW_TAG_union_type = 0x17
DW_TAG_member = 0x0d
DW_TAG_subprogram = 0x2e
DW_TAG_formal_parameter = 0x05
DW_TAG_base_type = 0x24
DW_TAG_pointer_type = 0x0f
DW_TAG_typedef = 0x16
DW_TAG_const_type = 0x26
DW_TAG_volatile_type = 0x35
DW_TAG_array_type = 0x01
DW_TAG_enumeration_type = 0x04

DW_AT_name = 0x03
DW_AT_byte_size = 0x0b
DW_AT_type = 0x49
DW_AT_data_member_location = 0x38
DW_AT_stmt_list = 0x10


def uleb(d: bytes, o: int) -> tuple[int, int]:
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if not (b & 0x80):
            return r, o


def sleb(d: bytes, o: int) -> tuple[int, int]:
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if not (b & 0x80):
            if b & 0x40:
                r |= -(1 << s)
            return r, o


def cstr(d: bytes, o: int) -> tuple[str, int]:
    z = d.find(b"\0", o)
    return d[o:z].decode("latin1"), z + 1


class DwarfInfo:
    def __init__(self, info: bytes, abbrev: bytes, dstr: bytes):
        self.info = info
        self.abbrev = abbrev
        self.dstr = dstr

    def _parse_abbrev(self, off: int) -> dict:
        table: dict[int, tuple] = {}
        d = self.abbrev
        o = off
        while o < len(d):
            code, o = uleb(d, o)
            if code == 0:
                break
            tag, o = uleb(d, o)
            has_children = d[o]; o += 1
            attrs = []
            while True:
                at, o = uleb(d, o)
                fm, o = uleb(d, o)
                if at == 0 and fm == 0:
                    break
                attrs.append((at, fm))
            table[code] = (tag, has_children, attrs)
        return table

    def _read_form(self, o: int, form: int, addr_size: int, cu_base: int):
        d = self.info
        if form == 0x01:
            fmt = ">I" if addr_size == 4 else ">Q"
            return struct.unpack_from(fmt, d, o)[0], o + addr_size
        if form == 0x0b:
            return d[o], o + 1
        if form == 0x05:
            return struct.unpack_from(">H", d, o)[0], o + 2
        if form == 0x06:
            return struct.unpack_from(">I", d, o)[0], o + 4
        if form == 0x07:
            return struct.unpack_from(">Q", d, o)[0], o + 8
        if form == 0x08:
            return cstr(d, o)
        if form == 0x0e:
            so = struct.unpack_from(">I", d, o)[0]
            return cstr(self.dstr, so)[0], o + 4
        if form == 0x0f:
            return uleb(d, o)
        if form == 0x0d:
            return sleb(d, o)
        if form == 0x0c:
            return d[o], o + 1
        if form == 0x11:
            return d[o] + cu_base, o + 1
        if form == 0x12:
            return struct.unpack_from(">H", d, o)[0] + cu_base, o + 2
        if form == 0x13:
            return struct.unpack_from(">I", d, o)[0] + cu_base, o + 4
        if form == 0x14:
            return struct.unpack_from(">Q", d, o)[0] + cu_base, o + 8
        if form == 0x15:
            v, o = uleb(d, o)
            return v + cu_base, o
        if form == 0x10:
            return struct.unpack_from(">I", d, o)[0], o + 4
        if form == 0x0a:
            n = d[o]
            return d[o + 1:o + 1 + n], o + 1 + n
        if form == 0x03:
            n = struct.unpack_from(">H", d, o)[0]
            return d[o + 2:o + 2 + n], o + 2 + n
        if form == 0x04:
            n = struct.unpack_from(">I", d, o)[0]
            return d[o + 4:o + 4 + n], o + 4 + n
        if form == 0x09:
            n, o = uleb(d, o)
            return d[o:o + n], o + n
        raise ValueError(f"unhandled DWARF form 0x{form:x}")

    def dies(self):
        info = self.info
        o = 0
        while o < len(info):
            cu_start = o
            unit_len = struct.unpack_from(">I", info, o)[0]
            cu_end = o + 4 + unit_len
            abbr_off = struct.unpack_from(">I", info, o + 6)[0]
            addr_size = info[o + 10]
            table = self._parse_abbrev(abbr_off)
            p = o + 11
            depth = 0
            while p < cu_end:
                die_off = p
                code, p = uleb(info, p)
                if code == 0:
                    depth -= 1
                    continue
                tag, has_children, attrs = table[code]
                vals = {}
                for at, fm in attrs:
                    v, p = self._read_form(p, fm, addr_size, cu_start)
                    vals[at] = v
                yield die_off, tag, vals, depth
                if has_children:
                    depth += 1
            o = cu_end


def open_dwarf(elf) -> DwarfInfo | None:
    sec = {s.name_str: i for i, s in enumerate(elf.section_headers) if s.name_str}
    if ".debug_info" not in sec or ".debug_abbrev" not in sec:
        return None
    dstr = elf.get_section_data(sec[".debug_str"]) if ".debug_str" in sec else b""
    return DwarfInfo(elf.get_section_data(sec[".debug_info"]),
                     elf.get_section_data(sec[".debug_abbrev"]), dstr)

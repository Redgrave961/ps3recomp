#!/usr/bin/env python3
"""
Find SPU function boundaries inside an embedded SPU ELF.

Emits a JSON list of {"start": addr, "end": addr} ready for
`spu_lifter.py --functions`.

Seeds the function set from four sources:
  1. ELF entry point (e_entry).
  2. Symbol table -- STT_FUNC entries (with their st_value/st_size if size
     is non-zero, which gives us *exact* boundaries when present).
  3. All brsl/brasl targets the disassembler can resolve in the code.
  4. Addresses loaded into $r0 by `il`/`ila` -- the link-register resume
     idiom (see collect_link_register_targets).

For each function start without a symbol-provided size, the end is
determined by scanning forward until we hit one of:
   - `stop` / `stopd`,
   - `bi $r0` (the SPU ABI return — link reg is $r0),
   - the next function start in the ordered seed set,
   - the end of the executable section.

The output also reports a "data tail" if the executable PT_LOAD segment
extends past the last detected function -- a hint that .rodata is linked
into .text, and that the `.word` count from `spu_lifter.py` should drop
once function boundaries are honoured.

Usage:
    python find_spu_functions.py <spu.elf> [--out functions.json]
                                           [--code-out code.bin]
                                           [--base ADDR]
"""

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spu_disasm import disassemble_spu, spu_decode

ELF_MAGIC          = b"\x7FELF"
EM_SPU             = 23
SHT_SYMTAB         = 2
SHT_STRTAB         = 3
SHF_EXECINSTR      = 4
STT_FUNC           = 2


def be_u32(buf, off): return struct.unpack_from(">I", buf, off)[0]
def be_u16(buf, off): return struct.unpack_from(">H", buf, off)[0]


def parse_elf(buf):
    if buf[:4] != ELF_MAGIC:
        raise SystemExit("Not an ELF file")
    if buf[5] != 2:
        raise SystemExit("Not big-endian (SPU ELFs are BE)")
    if be_u16(buf, 0x12) != EM_SPU:
        raise SystemExit("Not an SPU ELF (e_machine != 23)")

    e_entry     = be_u32(buf, 0x18)
    e_phoff     = be_u32(buf, 0x1C)
    e_shoff     = be_u32(buf, 0x20)
    e_phentsize = be_u16(buf, 0x2A)
    e_phnum     = be_u16(buf, 0x2C)
    e_shentsize = be_u16(buf, 0x2E)
    e_shnum     = be_u16(buf, 0x30)
    e_shstrndx  = be_u16(buf, 0x32)

    phs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack_from(">IIIIIIII", buf, off)
        phs.append(dict(type=p_type, off=p_off, vaddr=p_vaddr, filesz=p_filesz,
                        memsz=p_memsz, flags=p_flags))

    shs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_off, sh_size, sh_link, sh_info, \
            sh_addralign, sh_entsize = struct.unpack_from(">IIIIIIIIII", buf, off)
        shs.append(dict(name_off=sh_name, type=sh_type, flags=sh_flags,
                        addr=sh_addr, off=sh_off, size=sh_size, link=sh_link,
                        info=sh_info, entsize=sh_entsize))

    shstrs = b""
    if 0 <= e_shstrndx < len(shs):
        s = shs[e_shstrndx]
        shstrs = buf[s["off"]:s["off"] + s["size"]]

    def sec_name(sh):
        n = sh["name_off"]
        if n >= len(shstrs):
            return ""
        end = shstrs.find(b"\x00", n)
        return shstrs[n:end].decode("ascii", "replace")

    for sh in shs:
        sh["name"] = sec_name(sh)

    return dict(entry=e_entry, phs=phs, shs=shs)


def read_symbols(buf, shs):
    funcs = []
    for sh in shs:
        if sh["type"] != SHT_SYMTAB:
            continue
        strtab = shs[sh["link"]] if 0 <= sh["link"] < len(shs) else None
        strbuf = buf[strtab["off"]:strtab["off"] + strtab["size"]] if strtab else b""
        if sh["entsize"] <= 0:
            continue
        avail = max(0, len(buf) - sh["off"]) // sh["entsize"]
        for i in range(min(sh["size"] // sh["entsize"], avail)):
            off = sh["off"] + i * sh["entsize"]
            st_name, st_value, st_size, st_info, st_other, st_shndx = \
                struct.unpack_from(">IIIBBH", buf, off)
            if (st_info & 0xF) != STT_FUNC:
                continue
            name = ""
            if st_name < len(strbuf):
                e = strbuf.find(b"\x00", st_name)
                name = strbuf[st_name:e].decode("ascii", "replace")
            funcs.append(dict(addr=st_value, size=st_size, name=name))
    return funcs


def text_image(buf, phs):
    xs = [ph for ph in phs if ph["type"] == 1 and (ph["flags"] & 1)]
    if not xs:
        raise SystemExit("No executable PT_LOAD segment found")
    lo = min(ph["vaddr"] for ph in xs)
    img = bytearray(max(ph["vaddr"] + ph["filesz"] for ph in xs) - lo)
    for ph in xs:
        img[ph["vaddr"] - lo:ph["vaddr"] - lo + ph["filesz"]] = buf[ph["off"]:ph["off"] + ph["filesz"]]
    return bytes(img), lo


def pick_text(phs):
    for ph in phs:
        if ph["type"] == 1 and (ph["flags"] & 1):
            return ph["off"], ph["vaddr"], ph["filesz"]
    raise SystemExit("No executable PT_LOAD segment found")


_BRANCH_MNEMONICS = {
    "brsl", "brasl",
    "br", "bra",
    "brz", "brnz", "brhz", "brhnz",
}

def collect_branch_targets(insns):
    targets = set()
    for ins in insns:
        if ins.mnemonic not in _BRANCH_MNEMONICS:
            continue
        ops = [t.strip() for t in ins.operands.split(",") if t.strip()]
        for t in ops:
            if t.startswith("0x"):
                try:
                    targets.add(int(t, 16))
                except ValueError:
                    pass
    return targets


def collect_link_register_targets(insns):
    targets = set()
    for ins in insns:
        if ins.mnemonic not in ("il", "ila"):
            continue
        ops = [t.strip() for t in ins.operands.split(",") if t.strip()]
        if len(ops) != 2 or ops[0] != "$r0":
            continue
        tok = ops[1]
        try:
            targets.add(int(tok, 16) if tok.lower().startswith(("0x", "-0x"))
                        else int(tok, 10))
        except ValueError:
            pass
    return targets


_INDIRECT_BR = {"bi", "bisl", "bisled", "biz", "binz", "bihz", "bihnz"}


def collect_computed_branch_targets(insns, window=8):
    targets = set()
    for idx, ins in enumerate(insns):
        if ins.mnemonic not in _INDIRECT_BR:
            continue
        ops = [t.strip() for t in ins.operands.split(",") if t.strip()]
        if not ops or not ops[-1].startswith("$r"):
            continue
        reg = ops[-1]
        for j in range(idx - 1, max(-1, idx - 1 - window), -1):
            w = insns[j]
            if w.mnemonic not in ("il", "ila"):
                continue
            wops = [t.strip() for t in w.operands.split(",") if t.strip()]
            if len(wops) != 2 or wops[0] != reg:
                continue
            tok = wops[1]
            try:
                targets.add(int(tok, 16) if tok.lower().startswith(("0x", "-0x"))
                            else int(tok, 10))
            except ValueError:
                pass
            break
    return targets


def collect_jump_table_targets(insns, code, code_start, code_end,
                               window=48, max_entries=32):
    del window
    valid_starts = {ins.addr for ins in insns}
    targets = set()
    seen_bases = set()
    for ins in insns:
        if ins.mnemonic not in ("il", "ila"):
            continue
        wops = [t.strip() for t in ins.operands.split(",") if t.strip()]
        if len(wops) != 2:
            continue
        try:
            tok = wops[1]
            tbl = int(tok, 16) if tok.lower().startswith(("0x", "-0x")) else int(tok, 10)
        except ValueError:
            continue
        if tbl == 0 or (tbl & 3) or not (code_start <= tbl < code_end):
            continue
        if tbl in seen_bases:
            continue
        seen_bases.add(tbl)
        off = tbl - code_start
        entries = set()
        for k in range(max_entries):
            o = off + 4 * k
            if o + 4 > len(code):
                break
            wrd = int.from_bytes(code[o:o + 4], "big")
            if wrd & 3:
                break
            abs_ok = wrd in valid_starts
            rel = (tbl + wrd) & 0x3FFFF
            rel_ok = (rel & 3) == 0 and rel in valid_starts
            if not abs_ok and not rel_ok:
                break
            if abs_ok:
                entries.add(wrd)
            if rel_ok:
                entries.add(rel)
        if len(entries) >= 2:
            targets.update(entries)
    return targets


def collect_ila_continuation_targets(insns, code_start, code_end):
    valid_starts = {ins.addr for ins in insns}
    targets = set()
    ilhu_val = {}
    for ins in insns:
        wops = [t.strip() for t in ins.operands.split(",") if t.strip()]
        if len(wops) != 2:
            continue
        try:
            tok = wops[1]
            imm = int(tok, 16) if tok.lower().startswith(("0x", "-0x")) else int(tok, 10)
        except ValueError:
            continue
        cand = None
        if ins.mnemonic in ("il", "ila"):
            cand = imm
        elif ins.mnemonic == "ilhu":
            ilhu_val[wops[0]] = (imm & 0xFFFF) << 16
            continue
        elif ins.mnemonic == "iohl" and wops[0] in ilhu_val:
            cand = ilhu_val.pop(wops[0]) | (imm & 0xFFFF)
        if cand is None:
            continue
        if cand and (cand & 3) == 0 and code_start <= cand < code_end \
           and cand in valid_starts:
            targets.add(cand)
    return targets


def collect_duff_targets(insns, code_start, code_end, window=12, span=64):
    targets = set()
    for idx, ins in enumerate(insns):
        if ins.mnemonic != "bi":
            continue
        ops = [t.strip() for t in ins.operands.split(",") if t.strip()]
        if len(ops) != 1 or not ops[0].startswith("$r") or ops[0] == "$r0":
            continue
        reg = ops[0]
        link = None
        imm = 0
        for j in range(idx - 1, max(-1, idx - 1 - window), -1):
            w = insns[j]
            wops = [t.strip() for t in w.operands.split(",") if t.strip()]
            if w.mnemonic == "ai" and len(wops) == 3 \
                    and wops[0] == reg and wops[1] == reg:
                try:
                    imm += int(wops[2], 0)
                except ValueError:
                    break
                continue
            if w.mnemonic in ("brsl", "brasl") and wops and wops[0] == reg:
                try:
                    tgt = int(wops[-1], 0)
                except ValueError:
                    break
                if tgt == w.addr + 4:
                    link = w.addr + 4
                break
        if link is None or imm <= 0:
            continue
        base = link + imm
        for o in range(0, span + 4, 4):
            t = base + o
            if code_start <= t < code_end:
                targets.add(t)
    return targets


def collect_function_pointer_tables(buf, phs, code_start, code_end,
                                    insns_by_addr, min_run=3, max_run=1024):
    targets = set()
    for ph in phs:
        if ph.get("type") != 1:
            continue
        off, fsz, vaddr = ph["off"], ph["filesz"], ph["vaddr"]
        seg = buf[off:off + fsz]
        skew = (-vaddr) & 3
        run = []
        def _flush(r):
            if len(r) >= min_run and len(set(r)) >= 2:
                targets.update(r)
        i = skew
        while i + 4 <= len(seg):
            w = int.from_bytes(seg[i:i + 4], "big")
            if (w & 3) == 0 and code_start <= w < code_end and w in insns_by_addr:
                run.append(w)
                if len(run) >= max_run:
                    _flush(run); run = []
            else:
                _flush(run); run = []
            i += 4
        _flush(run)
    return targets


def collect_brsl_targets(insns):
    return {t for ins in insns if ins.mnemonic in ("brsl", "brasl")
            for t in [int(tok.strip(), 16) for tok in ins.operands.split(",")
                      if tok.strip().startswith("0x")]}


_TERMINATORS_NO_FALLTHROUGH = {
    "stop", "stopd",
    "br", "bra", "iret",
}


LV2_RESUMING_STOPS = {0x100, 0x110, 0x111}


def is_resuming_stop(ins):
    return ins.mnemonic == "stop" and (ins.raw & 0x3FFF) in LV2_RESUMING_STOPS


def is_return(ins):
    if ins.mnemonic != "bi":
        return False
    ops = [t.strip() for t in ins.operands.split(",") if t.strip()]
    return ops == ["$r0"]


def find_end(start, insns_by_addr, sorted_starts, code_end):
    i = 0
    while i < len(sorted_starts) and sorted_starts[i] <= start:
        i += 1
    next_start = sorted_starts[i] if i < len(sorted_starts) else code_end

    pc = start
    while pc < next_start and pc < code_end:
        ins = insns_by_addr.get(pc)
        if ins is None:
            break
        if is_return(ins):
            return pc + 4
        if ins.mnemonic in _TERMINATORS_NO_FALLTHROUGH and not is_resuming_stop(ins):
            return pc + 4
        pc += 4

    return min(next_start, code_end)


def resolve_functions(seed_starts, sized_by_start, insns_by_addr, code_end):
    sorted_starts = sorted(seed_starts)
    funcs = []
    for start in sorted_starts:
        end = sized_by_start[start] if start in sized_by_start else find_end(start, insns_by_addr, sorted_starts, code_end)
        if end > start:
            funcs.append((start, end))
    funcs.sort()
    cleaned = []
    for s, e in funcs:
        if cleaned and s < cleaned[-1][1]:
            ps, pe = cleaned[-1]
            cleaned[-1] = (ps, max(pe, e))
        else:
            cleaned.append((s, e))
    return cleaned


def is_padding(ins):
    return ins.raw == 0 or ins.mnemonic in ("nop", "lnop")


def collect_gap_entries(funcs, insns_by_addr):
    entries = set()
    for (_, a), (b, _) in zip(funcs, funcs[1:]):
        if b - a < 8:
            continue
        span = [insns_by_addr.get(x) for x in range(a, b, 4)]
        if any(i is None or i.mnemonic == ".word" for i in span):
            continue
        start = True
        for ins in span:
            if start and not is_padding(ins):
                entries.add(ins.addr)
                start = False
            if not start and (is_return(ins) or (ins.mnemonic in _TERMINATORS_NO_FALLTHROUGH and not is_resuming_stop(ins))):
                start = True
    return entries


def detect_functions(buf, base_override=None, verbose=True, raw=False):
    if raw:
        base = base_override if base_override is not None else 0
        elf = {"entry": base, "phs": [{"type": 1, "flags": 1, "off": 0,
                                       "vaddr": base, "filesz": len(buf),
                                       "memsz": len(buf)}], "shs": []}
        text_off, text_va, text_size = 0, base, len(buf)
    else:
        elf = parse_elf(buf)
        text_off, text_va, _ = pick_text(elf["phs"])
        code, text_va = text_image(buf, elf["phs"])
    if raw:
        code = buf[text_off:text_off + text_size]
    base = base_override if base_override is not None else text_va
    insns = disassemble_spu(code, base_addr=base)
    insns_by_addr = {ins.addr: ins for ins in insns}
    code_start = base
    code_end   = base + len(code)

    syms = read_symbols(buf, elf["shs"]) if elf["shs"] else []
    seed_starts = set()
    sized_funcs = []

    if code_start <= elf["entry"] < code_end:
        seed_starts.add(elf["entry"])
    for s in syms:
        if code_start <= s["addr"] < code_end:
            seed_starts.add(s["addr"])
            if s["size"] > 0:
                sized_funcs.append((s["addr"], s["addr"] + s["size"]))
    for t in collect_branch_targets(insns):
        if code_start <= t < code_end:
            seed_starts.add(t)
    fptr_targets = collect_function_pointer_tables(
        buf, elf["phs"], code_start, code_end, insns_by_addr)
    for t in (collect_link_register_targets(insns)
              | collect_computed_branch_targets(insns)
              | collect_jump_table_targets(insns, code, code_start, code_end)
              | collect_ila_continuation_targets(insns, code_start, code_end)
              | collect_duff_targets(insns, code_start, code_end)
              | fptr_targets):
        if code_start <= t < code_end and (t & 3) == 0 and t in insns_by_addr:
            seed_starts.add(t)
    if fptr_targets and verbose:
        print(f"  {len(fptr_targets)} data function-pointer-table target(s): "
              f"{', '.join(f'0x{a:X}' for a in sorted(fptr_targets))}")

    if not seed_starts:
        seed_starts.add(code_start)

    sized_by_start = {s: e for (s, e) in sized_funcs}
    cleaned = resolve_functions(seed_starts, sized_by_start, insns_by_addr, code_end)
    for _ in range(16):
        gap = collect_gap_entries(cleaned, insns_by_addr) - seed_starts
        if not gap:
            break
        seed_starts |= gap
        cleaned = resolve_functions(seed_starts, sized_by_start, insns_by_addr, code_end)
    sorted_starts = sorted(seed_starts)

    if verbose:
        print(f"Text segment: va=0x{code_start:X} .. 0x{code_end:X}"
              f" ({len(code):,} bytes, {len(code)//4:,} instructions)")
        print(f"Seeds: entry={'set' if elf['entry'] else 'none'}, "
              f"{len(syms)} STT_FUNC symbols, "
              f"{len(collect_brsl_targets(insns))} brsl/brasl + "
              f"{len(collect_branch_targets(insns)) - len(collect_brsl_targets(insns))} "
              f"other branch targets, "
              f"{len(sorted_starts)} unique starts")
        print(f"Detected {len(cleaned)} function(s)")
        if cleaned:
            cov = sum(e - s for s, e in cleaned)
            print(f"Function coverage: {cov:,} / {len(code):,} bytes "
                  f"({100.0 * cov / len(code):.1f}%) -- "
                  f"the rest is data/padding embedded in .text")

    return cleaned, (text_off, base, len(code))


_TAIL_STOPS = {"bi", "br", "bra", "stop", "stopd"}


def infer_code_end(buf):
    elf = parse_elf(buf)
    if any((sh["flags"] & SHF_EXECINSTR) and sh["size"] for sh in elf["shs"]):
        return 0
    code, base = text_image(buf, elf["phs"])
    return _infer_code_end(code, base, elf["entry"], buf, elf["phs"])


_FLOW_ENDS = {"bi", "br", "bra", "iret", "stopd"}
_FLOW_HINTS = {"hbr", "hbra", "hbrr"}


def reachable_code_end(code, base, entries):
    insns = disassemble_spu(code, base_addr=base)
    by_addr = {i.addr: i for i in insns}
    end = base + len(code)
    seen, undecodable = set(), set()
    work = [e for e in entries if base <= e < end]
    while work:
        while work:
            a = work.pop()
            while base <= a < end and a not in seen:
                ins = by_addr.get(a)
                if ins is None:
                    break
                seen.add(a)
                if ins.mnemonic == ".word":
                    undecodable.add(a)
                    break
                m = ins.mnemonic
                if m not in _FLOW_HINTS:
                    work.extend(t for t in collect_branch_targets([ins]) if base <= t < end)
                if m in _FLOW_ENDS or (m == "stop" and (ins.raw & 0x3FFF) == 0):
                    break
                a += 4
        reached = [by_addr[a] for a in sorted(seen) if by_addr[a].mnemonic != ".word"]
        more = (collect_link_register_targets(reached)
                | collect_computed_branch_targets(reached)
                | collect_jump_table_targets(reached, code, base, end)
                | collect_ila_continuation_targets(reached, base, end))
        work = [t for t in more if base <= t < end and t not in seen]
    return (max(seen) + 4 if seen else base), sorted(undecodable)


def _infer_code_end(code, base, entry, buf, phs):
    end = base + len(code)
    insns = disassemble_spu(code, base_addr=base)
    first = next((i.addr for i in insns if i.mnemonic == ".word"), None)
    if first is None:
        return 0
    stops = [i.addr for i in insns if i.addr < first and i.mnemonic in _TAIL_STOPS]
    if not stops:
        return 0
    ce = max(stops) + 4
    below = [i for i in insns if i.addr < ce]
    by_addr = {i.addr: i for i in insns}
    refs = (collect_branch_targets(below)
            | collect_link_register_targets(below)
            | collect_computed_branch_targets(below)
            | collect_jump_table_targets(below, code, base, end)
            | collect_ila_continuation_targets(below, base, end)
            | collect_duff_targets(below, base, end)
            | collect_function_pointer_tables(buf, phs, base, end, by_addr))
    if ce <= entry < end or any(ce <= t < end for t in refs):
        return 0
    return ce


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input", help="SPU ELF (e.g. extracted by extract_spu_images.py)")
    p.add_argument("--out", default=None, help="JSON output (default: stdout)")
    p.add_argument("--code-out", default=None,
                   help="Also write the raw .text bytes here (for "
                        "`spu_lifter.py --base <va>`)")
    p.add_argument("--raw", action="store_true",
                   help="input is a raw local-store image (a SPURS job blob), "
                        "not an ELF: no phdrs, no symbols, so every seed comes "
                        "from the branch analysis")
    p.add_argument("--base", type=lambda x: int(x, 0), default=None,
                   help="Override the .text base address")
    args = p.parse_args()

    with open(args.input, "rb") as f:
        buf = f.read()

    funcs, (text_off, base, size) = detect_functions(buf, args.base, raw=args.raw)

    out_obj = [{"start": s, "end": e} for s, e in funcs]
    if args.out:
        with open(args.out, "w") as f:
            json.dump(out_obj, f, indent=2)
        print(f"Wrote {args.out} ({len(out_obj)} function(s))")
    else:
        print(json.dumps(out_obj, indent=2))

    if args.code_out:
        with open(args.code_out, "wb") as f:
            f.write(buf[text_off:text_off + size])
        print(f"Wrote {args.code_out} ({size} bytes, base 0x{base:X})")


if __name__ == "__main__":
    sys.exit(main() or 0)

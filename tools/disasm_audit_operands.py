#!/usr/bin/env py -3
import argparse
import os
import re
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf_parser import ELFFile, PT_LOAD
from ppu_disasm import decode

WHITELIST_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "disasm_audit_operands_whitelist.txt")


def _capstone_or_die():
    try:
        import capstone
        return capstone
    except ImportError:
        print("disasm_audit_operands: capstone not installed -- "
              "run: py -3 -m pip install capstone", file=sys.stderr)
        sys.exit(1)


_REG_RE = re.compile(r"^(?P<cls>[rfv]|cr)(?P<idx>\d+)$", re.IGNORECASE)
_CR_BIT_RE = re.compile(r"^(?:cr(?P<idx>\d+))?(?P<bit>lt|gt|eq|so|un)$",
                         re.IGNORECASE)
_CR_BIT_VALUE = {"lt": 0, "gt": 1, "eq": 2, "so": 3, "un": 3}
_HEX_RE = re.compile(r"^-?0x[0-9a-fA-F]+$")
_DEC_RE = re.compile(r"^-?\d+$")
_DISP_RE = re.compile(
    r"^(?P<disp>-?(?:0x[0-9a-fA-F]+|\d+))\((?P<reg>[a-zA-Z]+\d+|\d+)\)$")


def _parse_int_token(tok: str):
    if _HEX_RE.match(tok):
        return int(tok, 16)
    if _DEC_RE.match(tok):
        return int(tok, 10)
    return None


def canon_operand(tok: str):
    tok = tok.strip()
    if not tok:
        return ("raw", "")

    m = _DISP_RE.match(tok)
    if m:
        disp = _parse_int_token(m.group("disp"))
        reg_tok = m.group("reg")
        base = _REG_RE.match(reg_tok)
        if base:
            base_idx = int(base.group("idx"))
        elif reg_tok.isdigit():
            base_idx = int(reg_tok)
        else:
            base_idx = reg_tok
        return ("dispmem", disp, base_idx)

    m = _REG_RE.match(tok)
    if m:
        return ("reg", m.group("cls").lower(), int(m.group("idx")))

    m = _CR_BIT_RE.match(tok)
    if m and (m.group("idx") is not None or m.group("bit")):
        field = int(m.group("idx")) if m.group("idx") is not None else 0
        return ("crbit", field, _CR_BIT_VALUE[m.group("bit").lower()])

    val = _parse_int_token(tok)
    if val is not None:
        return ("imm", val)

    return ("raw", tok.lower())


_CR_LOGICAL_MNEMONICS = {
    "crand", "cror", "crxor", "crnand", "crnor", "creqv", "crandc", "crorc",
}


def _reinterpret_as_crbit(canon):
    if canon[0] == "imm" and 0 <= canon[1] <= 31:
        v = canon[1]
        return ("crbit", v >> 2, v & 3)
    return canon


def canon_operand_list(op_str: str, mnemonic: str = ""):
    op_str = op_str.strip()
    if not op_str:
        return []
    parts = [p.strip() for p in op_str.split(",") if p.strip() != ""]
    canon = [canon_operand(p) for p in parts]
    if mnemonic in _CR_LOGICAL_MNEMONICS:
        canon = [_reinterpret_as_crbit(c) for c in canon]
    return canon


def _standalone_reg0_equiv(a, b):
    reg0 = ("reg", "r", 0)
    imm0 = ("imm", 0)
    return (a == reg0 and b == imm0) or (a == imm0 and b == reg0)


def _abs_branch_target_equiv(a, b):
    if a[0] != "imm" or b[0] != "imm":
        return False
    v1, v2 = a[1], b[1]
    if (v1 & 0xFFFFFFFF) != (v2 & 0xFFFFFFFF):
        return False
    low32 = v1 & 0xFFFFFFFF
    if v1 == v2:
        return True
    if not (low32 & 0x80000000):
        return False
    sign_ext_64_unsigned = low32 | 0xFFFFFFFF00000000
    return {v1, v2} == {low32, sign_ext_64_unsigned}


def _capstone_dropped_zero_branch_target(ours_list, cs_list, addr):
    if len(ours_list) != len(cs_list) + 1:
        return False
    extra = ours_list[-1]
    if extra[0] != "imm":
        return False
    if extra[1] not in (0, addr):
        return False
    return ours_list[:-1] == cs_list


def operands_equal(ours_list, cs_list, addr=None):
    if ours_list == cs_list:
        return True
    if addr is not None and \
            _capstone_dropped_zero_branch_target(ours_list, cs_list, addr):
        return True
    if len(ours_list) != len(cs_list):
        return False
    for a, b in zip(ours_list, cs_list):
        if a == b:
            continue
        if _standalone_reg0_equiv(a, b):
            continue
        if _abs_branch_target_equiv(a, b):
            continue
        return False
    return True


def load_whitelist(path: str):
    wl = {}
    if not os.path.exists(path):
        return wl
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "|" not in line:
                continue
            mn, reason = line.split("|", 1)
            wl[mn.strip()] = reason.strip()
    return wl


def executable_words(elf_path: str):
    elf = ELFFile(elf_path)
    elf.load()
    if not elf.elf_header.big_endian:
        print("disasm_audit_operands: WARNING -- ELF parsed as "
              "little-endian; PS3 PPU binaries should be big-endian",
              file=sys.stderr)

    segments = []
    for idx, ph in enumerate(elf.program_headers):
        if ph.p_type == PT_LOAD and (ph.p_flags & 1) and ph.p_filesz > 0:
            data = elf.get_segment_data(idx)
            segments.append((ph.p_vaddr, data))

    if not segments:
        print("disasm_audit_operands: no PF_X PT_LOAD segments found",
              file=sys.stderr)
        return

    fmt = ">I" if elf.elf_header.big_endian else "<I"
    for base_vaddr, data in segments:
        n_words = len(data) // 4
        for i in range(n_words):
            off = i * 4
            word = struct.unpack_from(fmt, data, off)[0]
            yield base_vaddr + off, word


def decode_ours(word: int, addr: int):
    insn = decode(word, addr)
    return insn.mnemonic, insn.operands


def make_capstone_decoder():
    capstone = _capstone_or_die()
    md = capstone.Cs(capstone.CS_ARCH_PPC,
                      capstone.CS_MODE_64 + capstone.CS_MODE_BIG_ENDIAN)
    md.detail = False

    def _decode(word: int, addr: int):
        data = struct.pack(">I", word)
        insns = list(md.disasm(data, addr))
        if not insns:
            return None
        return insns[0].mnemonic, insns[0].op_str

    return _decode


def run_audit(elf_path: str, limit: int, whitelist: dict, ours_decoder=None,
              label="real"):
    cs_decode = make_capstone_decoder()
    if ours_decoder is None:
        ours_decoder = decode_ours

    total = 0
    mnemonic_match = 0
    divergence_counter = Counter()
    divergence_samples = defaultdict(list)
    info_counter = Counter()
    info_samples = defaultdict(list)

    for addr, word in executable_words(elf_path):
        if limit and total >= limit:
            break
        total += 1

        ours_mn, ours_ops = ours_decoder(word, addr)
        cs_result = cs_decode(word, addr)
        if cs_result is None:
            continue
        cs_mn, cs_ops = cs_result

        if ours_mn != cs_mn:
            continue
        mnemonic_match += 1

        ours_canon = canon_operand_list(ours_ops, ours_mn)
        cs_canon = canon_operand_list(cs_ops, ours_mn)

        if operands_equal(ours_canon, cs_canon, addr):
            continue

        if len(ours_canon) != len(cs_canon):
            kind = "operand-count"
        else:
            kind = "operand-value"
            if sorted(map(repr, ours_canon)) == sorted(map(repr, cs_canon)):
                kind = "operand-order"

        key = (ours_mn, kind)
        if ours_mn in whitelist:
            info_counter[key] += 1
            if len(info_samples[key]) < 5:
                info_samples[key].append((addr, ours_ops, cs_ops))
        else:
            divergence_counter[key] += 1
            if len(divergence_samples[key]) < 5:
                divergence_samples[key].append((addr, ours_ops, cs_ops))

    return {
        "total": total,
        "mnemonic_match": mnemonic_match,
        "divergence_counter": divergence_counter,
        "divergence_samples": divergence_samples,
        "info_counter": info_counter,
        "info_samples": info_samples,
        "label": label,
    }


def print_report(result, top_n: int, whitelist: dict):
    total = result["total"]
    mnemonic_match = result["mnemonic_match"]
    divergence_counter = result["divergence_counter"]
    divergence_samples = result["divergence_samples"]
    info_counter = result["info_counter"]
    info_samples = result["info_samples"]

    print(f"disasm_audit_operands [{result['label']}]: {total} words "
          f"decoded, {mnemonic_match} had matching mnemonic (the operand-"
          f"comparison population)")

    residual = sum(divergence_counter.values())
    residual_groups = len(divergence_counter)
    info_total = sum(info_counter.values())

    print()
    print(f"=== REAL operand divergences (non-whitelisted): {residual} "
          f"words across {residual_groups} (mnemonic, kind) groups ===")
    print(f"top {top_n} by count (descending):")
    for (mn, kind), count in divergence_counter.most_common(top_n):
        samples = divergence_samples[(mn, kind)]
        print(f"  {count:8d}  mnemonic={mn!r:14s} kind={kind:16s}")
        for addr, ours_op, cs_op in samples:
            print(f"        0x{addr:08X}: ours=\"{mn} {ours_op}\" "
                  f"capstone=\"{mn} {cs_op}\"")

    print()
    print(f"=== Whitelisted operand-format noise (INFO): {info_total} "
          f"words across {len(info_counter)} (mnemonic, kind) groups ===")
    for (mn, kind), count in info_counter.most_common():
        reason = whitelist.get(mn, "(no reason recorded)")
        print(f"  INFO  {count:8d}  mnemonic={mn!r:14s} kind={kind:16s}"
              f"  -- {reason}")
        for addr, ours_op, cs_op in info_samples[(mn, kind)][:2]:
            print(f"        0x{addr:08X}: ours=\"{mn} {ours_op}\" "
                  f"capstone=\"{mn} {cs_op}\"")

    print()
    print("=== Sanity check: common ops (addi, lwz, stw, ori, rlwinm) ===")
    sanity_ok = True
    for op in ("addi", "lwz", "stw", "ori", "rlwinm"):
        bad = [((mn, kind), c) for (mn, kind), c in divergence_counter.items()
               if mn == op]
        if bad:
            sanity_ok = False
            for (mn, kind), c in bad:
                print(f"  FAIL  {op}: {c} non-whitelisted operand "
                      f"divergences (kind={kind})")
        else:
            print(f"  PASS  {op}: 0 non-whitelisted operand divergences")
    if not sanity_ok:
        print("  ** SANITY FAILED ** -- canonicalization is incomplete, "
              "see FAIL lines above")

    print()
    print(f"SUMMARY[{result['label']}]: total_words={total} "
          f"mnemonic_match_population={mnemonic_match} "
          f"residual_real_divergence_words={residual} "
          f"residual_groups={residual_groups} "
          f"whitelisted_info_words={info_total}")

    return residual, sanity_ok


def _make_corrupted_decoder_double_sign_extend():
    def _decode(word, addr):
        mn, ops = decode_ours(word, addr)
        if mn == "addi":
            parts = [p.strip() for p in ops.split(",")]
            if len(parts) == 3:
                imm = _parse_int_token(parts[2])
                if imm is not None and imm < 0:
                    corrupted = imm - 0x10000
                    parts[2] = f"{corrupted}" if corrupted >= 0 \
                        else f"-0x{-corrupted:X}"
                    ops = ", ".join(parts)
        return mn, ops
    return _decode


def _make_corrupted_decoder_mask_bug():
    def _decode(word, addr):
        mn, ops = decode_ours(word, addr)
        if mn == "rlwinm" or mn == "rlwinm.":
            parts = [p.strip() for p in ops.split(",")]
            if len(parts) == 5:
                me = _parse_int_token(parts[4])
                if me is not None and me < 31:
                    parts[4] = str(me + 1)
                    ops = ", ".join(parts)
        return mn, ops
    return _decode


def run_self_test(elf_path: str, limit: int, kind: str, whitelist: dict):
    if kind == "immediate":
        print("disasm_audit_operands --self-test-immediate-bug: seeding a "
              "double-sign-extension bug on 'addi' negative immediates in "
              "an IN-PROCESS COPY of decode() (tools/ppu_disasm.py on disk "
              "is untouched)")
        corrupted = _make_corrupted_decoder_double_sign_extend()
        target_mn = "addi"
    elif kind == "mask":
        print("disasm_audit_operands --self-test-mask-bug: seeding an "
              "off-by-one ME-field bug on 'rlwinm' in an IN-PROCESS COPY "
              "of decode() (tools/ppu_disasm.py on disk is untouched)")
        corrupted = _make_corrupted_decoder_mask_bug()
        target_mn = "rlwinm"
    else:
        raise ValueError(kind)

    result = run_audit(elf_path, limit, whitelist, ours_decoder=corrupted,
                        label=f"self-test-{kind}")
    residual, _sanity = print_report(result, 15, whitelist)

    hit = any(mn == target_mn
              for (mn, _kind) in result["divergence_counter"])
    if not hit or residual == 0:
        print(f"SELF-TEST FAILED: seeded {kind} bug on {target_mn!r} did "
              f"NOT surface as a REAL operand divergence")
        return False
    print(f"SELF-TEST PASSED: seeded {kind} bug on {target_mn!r} surfaced "
          f"as a REAL operand divergence ({residual} words)")
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Operand-level cross-check of tools/ppu_disasm.py vs "
                    "Capstone (PPC64 big-endian) over an ELF's executable "
                    "segments, restricted to mnemonic-matching words.")
    parser.add_argument("elf", help="Input PPU ELF to audit (e.g. game/EBOOT.elf)")
    parser.add_argument("--limit", type=int, default=0,
                        help="Stop after N words (0 = all, default)")
    parser.add_argument("--top", type=int, default=30,
                        help="How many divergence groups to print "
                            "(default 30)")
    parser.add_argument("--self-test-immediate-bug", action="store_true",
                        help="Seeded double-sign-extension bug on addi "
                            "(never touches ppu_disasm.py on disk)")
    parser.add_argument("--self-test-mask-bug", action="store_true",
                        help="Seeded off-by-one ME-field bug on rlwinm "
                            "(never touches ppu_disasm.py on disk)")
    args = parser.parse_args()

    whitelist = load_whitelist(WHITELIST_PATH)

    if args.self_test_immediate_bug:
        ok = run_self_test(args.elf, args.limit, "immediate", whitelist)
        sys.exit(0 if ok else 1)
    if args.self_test_mask_bug:
        ok = run_self_test(args.elf, args.limit, "mask", whitelist)
        sys.exit(0 if ok else 1)

    if not os.path.exists(args.elf):
        print(f"disasm_audit_operands: input file not found: {args.elf}",
              file=sys.stderr)
        sys.exit(1)

    result = run_audit(args.elf, args.limit, whitelist)
    residual, sanity_ok = print_report(result, args.top, whitelist)
    sys.exit(0 if (residual == 0 and sanity_ok) else 1)


if __name__ == "__main__":
    main()

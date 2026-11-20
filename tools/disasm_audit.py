#!/usr/bin/env py -3
import argparse
import os
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf_parser import ELFFile, PT_LOAD
from ppu_disasm import decode


def _capstone_or_die():
    try:
        import capstone
        return capstone
    except ImportError:
        print("disasm_audit: capstone not installed -- "
              "run: py -3 -m pip install capstone", file=sys.stderr)
        sys.exit(1)


ALIAS_WHITELIST = {
    ("or", "mr"):
        "or rD,rA,rA (Rc=0) -- Capstone simplifies register-move form to "
        "'mr'; ours prints the canonical 'or'. Confirmed: rA==rB required; "
        "'or.' (Rc=1) does NOT get simplified by Capstone (stays 'or.') so "
        "that pair is NOT in this whitelist -- a mismatch there is real.",
    ("ori", "nop"):
        "ori r0,r0,0 -- both decoders treat this specific all-zero encoding "
        "as the canonical no-op spelling; 'ori' with any non-zero rD/rA/UI "
        "does NOT alias (confirmed ori r3,r4,0 -> both print 'ori').",
    ("rlwinm", "slwi"):
        "rlwinm rD,rA,n,0,31-n (n<32) -- Capstone simplifies the common "
        "'shift left logical immediate' shape; ours keeps canonical rlwinm "
        "with explicit mb/me. Real rlwinm mismatches (wrong mb/me/sh from a "
        "table-encoding bug) still show up as (rlwinm, rlwinm) pairs with "
        "differing operands -- but v1 is mnemonic-only, so note this as a "
        "known blind spot for a future operand-level v2 check.",
    ("rlwinm", "srwi"):
        "rlwinm rD,rA,32-n,n,31 -- Capstone's 'shift right logical "
        "immediate' simplification, same family as slwi above.",
    ("rlwinm", "clrlwi"):
        "rlwinm rD,rA,0,n,31 -- Capstone's 'clear left n bits' "
        "simplification (rotate amount 0, mb=n, me=31).",
    ("rlwinm", "rotlwi"):
        "rlwinm rD,rA,n,0,31 -- Capstone's 'rotate left immediate' "
        "simplification (full-width rotate, mb=0, me=31).",
    ("rlwnm", "rotlw"):
        "rlwnm rD,rA,rB,0,31 -- register-shift-count analog of rotlwi.",
    ("cror", "crmove"):
        "cror bt,ba,ba (BA==BB) -- Capstone's 'copy one CR bit' "
        "simplification. Distinct-operand cror (e.g. cror 2,3,4) is NOT "
        "simplified -- confirmed both decoders print 'cror' there, so a "
        "mismatch on a distinct-operand crXXX instruction is still real "
        "(this is exactly the crnand/crnor swap bug class the tool exists "
        "to catch).",
    ("crnor", "crnot"):
        "crnor bt,ba,ba (BA==BB) -- Capstone's 'invert one CR bit' "
        "simplification ('crnot' aka 'crnotc0lt' family).",
    ("creqv", "crset"):
        "creqv bt,ba,ba (BA==BB) -- Capstone's 'set CR bit' simplification "
        "(A==B makes A eqv A always true).",
    ("crxor", "crclr"):
        "crxor bt,ba,ba (BA==BB) -- Capstone's 'clear CR bit' "
        "simplification (A==B makes A xor A always false).",
    ("mtcr", "mtcrf"):
        "mtcrf 0xff,rS (all 8 CRM nibbles set) -- ours always prints the "
        "'mtcr' short form for the full-mask case; Capstone always prints "
        "the general 'mtcrf mask,rS' form regardless of mask value.",
    ("rldicl", "clrldi"):
        "rldicl rD,rA,0,mb -- Capstone's 'clear left mb bits, 64-bit' "
        "simplification (sh=0, so it's a pure mask, no rotate); the 64-bit "
        "analog of rlwinm/clrlwi above. MEASURED at 0x0001058C (real game "
        "code): rldicl r3,r3,0,32 <-> clrldi r3,r3,0x20.",
    ("rldicl", "rotldi"):
        "rldicl rD,rA,sh,0 -- Capstone's 'rotate left doubleword immediate' "
        "simplification (mb=0, so it's a pure rotate, no mask). MEASURED at "
        "0x0110EA84: rldicl r19,r11,0,0 <-> rotldi r19,r11,0.",
    ("rldicr", "sldi"):
        "rldicr rD,rA,sh,me with me==63-sh -- Capstone's 'shift left "
        "doubleword immediate' simplification, the 64-bit analog of "
        "rlwinm/slwi above. MEASURED at 0x00010564: rldicr r4,r4,4,59 <-> "
        "sldi r4,r4,4 (63-4==59).",
    ("vnor", "vnot"):
        "vnor vD,vA,vA (vA==vB) -- Capstone's vector 'not' simplification, "
        "the VMX analog of the scalar or/mr and nor/not families. MEASURED "
        "at 0x0002563C: vnor v6,v6,v6 <-> vnot v6,v6.",
    ("mfcr", "mfocrf"):
        "mfcr/mfocrf share opcd=31,XO=19 and differ only in bit 11 (the "
        "'L' mode-select bit, introduced post-Cell in later Power ISA "
        "revisions as 'move one CR field' vs the original 'move whole "
        "CR'). The Cell PPU predates this ISA revision (based on PowerPC "
        "2.02) so ours always emits the older 'mfcr' form; Capstone "
        "supports the newer encoding and emits 'mfocrf' whenever L=1. "
        "MEASURED: ALL 248 real mismatch sites have L-bit=1 (confirmed by "
        "direct bit extraction over every opcd=31/XO=19 word in "
        "game/EBOOT.elf) -- deterministic, bit-driven, not noise.",
    ("mtcrf", "mtocrf"):
        "mtcrf/mtocrf share opcd=31,XO=144 and differ only in bit 11 (same "
        "'L' mode-select bit as mfcr/mfocrf above, same pre-Cell-ISA "
        "reasoning). MEASURED: ALL 221 real mismatch sites have L-bit=1 "
        "(confirmed by direct bit extraction over every opcd=31/XO=144 "
        "word in game/EBOOT.elf).",
}


_BDNZ_BDZ_FRIENDLY = {
    "bdnzt": "bdnz", "bdnzf": "bdnz", "bdzt": "bdz", "bdzf": "bdz",
}

_BRANCH_LKAA_SUFFIXES = ("", "l", "a", "la")

_TRAP_ROOTS = {"tdi": "td", "twi": "tw"}


def is_whitelisted_by_rule(ours_mn: str, cs_mn: str) -> str | None:
    if cs_mn == ours_mn + "+" or cs_mn == ours_mn + "-":
        return ("branch-prediction hint suffix ('+'=likely taken, "
                "'-'=likely not taken, Book I p.20 Fig.22 'at' bits) -- "
                "ours never emits the hint suffix, Capstone always does "
                "when at != 0b00; same base mnemonic otherwise.")

    for suf in _BRANCH_LKAA_SUFFIXES:
        if suf and not (ours_mn.endswith(suf) and cs_mn.endswith(suf)):
            continue
        cs_root = cs_mn[:len(cs_mn) - len(suf)] if suf else cs_mn
        ours_root = ours_mn[:len(ours_mn) - len(suf)] if suf else ours_mn
        friendly_root = _BDNZ_BDZ_FRIENDLY.get(cs_root)
        if friendly_root is not None and ours_root.startswith(friendly_root) \
                and ours_root != friendly_root:
            return ("combined decrement+condition branch (optionally with "
                    "an 'l'/'a'/'la' LK/AA suffix on both sides): ours "
                    "spells the derived friendly condition name (e.g. "
                    "bdnzge/bdnzlt, or bdnzgela/bdnzltla with suffix), "
                    "Capstone keeps the raw '" + cs_root + "' root + the "
                    "CR bit name as an operand -- same instruction, "
                    "naming-convention difference only.")

    trap_root = _TRAP_ROOTS.get(ours_mn)
    if trap_root is not None and cs_mn.startswith(trap_root):
        return ("trap TO-field friendly name: ours prints the raw numeric "
                "TO field (e.g. 'tdi 1,rA,SI'), Capstone maps common TO "
                "values to named forms (e.g. 'tdlgti'/'tdlti'). Sample "
                "sites measured at 0x0110D834/0x0110C308 are OUTSIDE any "
                "functions.json range -- likely data-in-.text, not real "
                "trap instructions (T8's territory), but the alias "
                "relationship itself is real either way.")

    return None


SANITY_COMMON_OPS = ("add", "lwz", "bl", "stw")


def executable_words(elf_path: str):
    elf = ELFFile(elf_path)
    elf.load()
    if not elf.elf_header.big_endian:
        print("disasm_audit: WARNING -- ELF parsed as little-endian; "
              "PS3 PPU binaries should be big-endian", file=sys.stderr)

    segments = []
    for idx, ph in enumerate(elf.program_headers):
        if ph.p_type == PT_LOAD and (ph.p_flags & 1) and ph.p_filesz > 0:
            data = elf.get_segment_data(idx)
            segments.append((ph.p_vaddr, data))

    if not segments:
        print("disasm_audit: no PF_X PT_LOAD segments found", file=sys.stderr)
        return

    fmt = ">I" if elf.elf_header.big_endian else "<I"
    for base_vaddr, data in segments:
        n_words = len(data) // 4
        for i in range(n_words):
            off = i * 4
            word = struct.unpack_from(fmt, data, off)[0]
            yield base_vaddr + off, word


def decode_ours(word: int, addr: int) -> str:
    insn = decode(word, addr)
    return insn.mnemonic


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
        return insns[0].mnemonic

    return _decode


def classify_pair(ours_mn: str, cs_mn: str):
    pair = (ours_mn, cs_mn)
    if pair in ALIAS_WHITELIST:
        return True, ALIAS_WHITELIST[pair]
    rule_reason = is_whitelisted_by_rule(ours_mn, cs_mn)
    if rule_reason is not None:
        return True, rule_reason
    return False, None


def run_audit(elf_path: str, limit: int = 0):
    cs_decode = make_capstone_decoder()

    total = 0
    mismatch_counter = Counter()
    whitelist_counter = Counter()
    mismatch_samples = defaultdict(list)
    whitelist_reasons = {}
    none_count = 0
    none_samples = []

    for addr, word in executable_words(elf_path):
        if limit and total >= limit:
            break
        total += 1
        ours_mn = decode_ours(word, addr)
        cs_mn = cs_decode(word, addr)

        if cs_mn is None:
            none_count += 1
            if len(none_samples) < 3:
                none_samples.append(addr)
            continue

        if ours_mn == cs_mn:
            continue

        pair = (ours_mn, cs_mn)
        is_wl, reason = classify_pair(ours_mn, cs_mn)
        if is_wl:
            whitelist_counter[pair] += 1
            whitelist_reasons[pair] = reason
        else:
            mismatch_counter[pair] += 1
            if len(mismatch_samples[pair]) < 3:
                mismatch_samples[pair].append(addr)

    return total, mismatch_counter, whitelist_counter, mismatch_samples, \
        whitelist_reasons, none_count, none_samples


def print_report(total, mismatch_counter, whitelist_counter,
                  mismatch_samples, whitelist_reasons, none_count,
                  none_samples, top_n: int):
    print(f"disasm_audit: {total} words decoded")
    print(f"disasm_audit: {none_count} words Capstone could not decode "
          f"at all (not counted as mismatches; no oracle mnemonic to "
          f"compare)")
    if none_samples:
        sample_str = ", ".join(f"0x{a:08X}" for a in none_samples)
        print(f"  sample addresses: {sample_str}")

    residual = sum(mismatch_counter.values())
    residual_pairs = len(mismatch_counter)
    whitelisted = sum(whitelist_counter.values())

    print()
    print(f"=== Mismatches: {residual} words across {residual_pairs} "
          f"non-whitelisted (ours_mn, capstone_mn) pairs ===")
    print(f"top {top_n} by count (descending):")
    for pair, count in mismatch_counter.most_common(top_n):
        ours_mn, cs_mn = pair
        addrs = ", ".join(f"0x{a:08X}" for a in mismatch_samples[pair])
        print(f"  {count:8d}  ours={ours_mn!r:14s} capstone={cs_mn!r:14s}"
              f"  samples: {addrs}")

    print()
    print(f"=== Whitelisted alias pairs (INFO, {whitelisted} words across "
          f"{len(whitelist_counter)} pairs) ===")
    for pair, count in whitelist_counter.most_common():
        ours_mn, cs_mn = pair
        reason = whitelist_reasons[pair]
        print(f"  INFO  {count:8d}  ours={ours_mn!r:14s} "
              f"capstone={cs_mn!r:14s}  -- {reason}")

    print()
    print("=== Sanity check: common ops (add, lwz, bl, stw) ===")
    sanity_ok = True
    for op in SANITY_COMMON_OPS:
        bad_pairs = [(pair, c) for pair, c in mismatch_counter.items()
                     if pair[0] == op]
        if bad_pairs:
            sanity_ok = False
            for pair, c in bad_pairs:
                print(f"  FAIL  {op}: {c} non-whitelisted mismatches as "
                      f"{pair[1]!r}")
        else:
            print(f"  PASS  {op}: 0 non-whitelisted mismatches")
    if not sanity_ok:
        print("  ** SANITY FAILED ** -- see FAIL lines above")

    print()
    print(f"SUMMARY: total_words={total} residual_mismatch_words={residual} "
          f"residual_pairs={residual_pairs} whitelisted_words={whitelisted} "
          f"capstone_refused={none_count}")

    return residual


SELF_TEST_SWAP = {"crnor": "crnand", "crnand": "crnor"}


def decode_ours_corrupted(word: int, addr: int) -> str:
    mn = decode_ours(word, addr)
    return SELF_TEST_SWAP.get(mn, mn)


def run_self_test(elf_path: str, limit: int):
    print("disasm_audit --self-test: seeding a crnor/crnand mnemonic swap "
          "in an IN-PROCESS COPY of the decode call (tools/ppu_disasm.py "
          "on disk is untouched)")
    cs_decode = make_capstone_decoder()

    total = 0
    mismatch_counter = Counter()
    mismatch_samples = defaultdict(list)

    for addr, word in executable_words(elf_path):
        if limit and total >= limit:
            break
        total += 1
        ours_mn = decode_ours_corrupted(word, addr)
        cs_mn = cs_decode(word, addr)
        if cs_mn is None or ours_mn == cs_mn:
            continue
        pair = (ours_mn, cs_mn)
        is_wl, _reason = classify_pair(ours_mn, cs_mn)
        if is_wl:
            continue
        mismatch_counter[pair] += 1
        if len(mismatch_samples[pair]) < 3:
            mismatch_samples[pair].append(addr)

    print(f"self-test: {total} words decoded with corrupted ours-side "
          f"decoder")
    top = mismatch_counter.most_common(10)
    print("top 10 mismatch pairs:")
    for pair, count in top:
        addrs = ", ".join(f"0x{a:08X}" for a in mismatch_samples[pair])
        print(f"  {count:8d}  ours={pair[0]!r:14s} capstone={pair[1]!r:14s}"
              f"  samples: {addrs}")

    seeded_pairs_present = any(
        pair[0] in SELF_TEST_SWAP for pair, _ in top
    )
    if not seeded_pairs_present or not top:
        print("SELF-TEST FAILED: seeded crnor/crnand swap did NOT surface "
              "at the top of the mismatch report")
        return False

    print("SELF-TEST PASSED: seeded crnor/crnand swap surfaced at the top "
          "of the mismatch report (as expected)")
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Cross-check tools/ppu_disasm.py against Capstone "
                    "(PPC64 big-endian) over an ELF's executable segments.")
    parser.add_argument("elf", help="Input PPU ELF to audit (e.g. game/EBOOT.elf)")
    parser.add_argument("--limit", type=int, default=0,
                        help="Stop after N words (0 = all, default)")
    parser.add_argument("--top", type=int, default=20,
                        help="How many mismatch pairs to print (default 20)")
    parser.add_argument("--self-test", action="store_true",
                        help="Run the seeded crnor/crnand self-test instead "
                            "of the real audit (never touches the real "
                            "decoder file on disk)")
    args = parser.parse_args()

    if args.self_test:
        ok = run_self_test(args.elf, args.limit)
        sys.exit(0 if ok else 1)

    if not os.path.exists(args.elf):
        print(f"disasm_audit: input file not found: {args.elf}",
              file=sys.stderr)
        sys.exit(1)

    total, mismatch_counter, whitelist_counter, mismatch_samples, \
        whitelist_reasons, none_count, none_samples = \
        run_audit(args.elf, args.limit)
    residual = print_report(total, mismatch_counter, whitelist_counter,
                            mismatch_samples, whitelist_reasons, none_count,
                            none_samples, args.top)
    sys.exit(0 if residual == 0 else 1)


if __name__ == "__main__":
    main()

from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from elf_parser import ELFFile
from ppu_disasm import decode as ppu_decode
from spu_disasm import spu_decode

EM_PPC64 = 21
EM_SPU = 23

_LINE = re.compile(r"^\s*([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{2} )+)\s+([a-zA-Z][a-zA-Z0-9_]*\.?)")

ALIAS = {
    ("or", "mr"),
    ("nor", "not"),
    ("tw", "trap"),
    ("rldicl", "clrldi"),
    ("rldicl", "rotldi"),
    ("rlwinm", "clrlwi"),
    ("rlwinm", "rotlwi"),
    ("rlwnm", "rotlw"),
    ("mtcrf", "mtocrf"),
    ("addi", "li"), ("addi", "la"), ("addis", "lis"),
    ("ori", "nop"),
}


def machine(elf_path: str) -> int:
    ef = ELFFile(elf_path); ef.load()
    return ef.elf_header.e_machine


def parse_objdump_text(text: str) -> list[tuple[int, int, str]]:
    rows = []
    for line in text.splitlines():
        m = _LINE.match(line)
        if not m:
            continue
        raw = bytes(int(b, 16) for b in m.group(2).split())
        if len(raw) != 4:
            continue
        word = struct.unpack(">I", raw)[0]
        rows.append((int(m.group(1), 16), word, m.group(3).lower()))
    return rows


def run_objdump(objdump: str, elf_path: str) -> list[tuple[int, int, str]]:
    out = subprocess.run([objdump, "-d", elf_path], capture_output=True, text=True).stdout
    return parse_objdump_text(out)


def audit(objdump: str, elf_path: str, is_spu: bool):
    rows = run_objdump(objdump, elf_path)
    decode = (lambda w, a: spu_decode(w, a).mnemonic) if is_spu \
        else (lambda w, a: ppu_decode(w, a).mnemonic)

    total = matched = 0
    mismatches = Counter()
    samples: dict[tuple[str, str], int] = {}
    for addr, word, oracle in rows:
        ours = decode(word, addr).lower()
        total += 1
        if ours == oracle or (ours, oracle) in ALIAS:
            matched += 1
        else:
            key = (ours, oracle)
            mismatches[key] += 1
            samples.setdefault(key, addr)
    return total, matched, mismatches, samples


def main() -> int:
    ap = argparse.ArgumentParser(description="Diff our PPU/SPU decoder vs the GNU objdump oracle")
    ap.add_argument("inputs", nargs="+", help="ELF(s) to audit (SPU or PPU, auto-detected)")
    ap.add_argument("--objdump", help="objdump binary (else OBJDUMP_PPU / OBJDUMP_SPU env)")
    ap.add_argument("--show", type=int, default=20, help="How many mismatch classes to list")
    args = ap.parse_args()

    grand_total = grand_match = 0
    all_mism = Counter()
    all_samples = {}
    for path in args.inputs:
        mach = machine(path)
        is_spu = mach == EM_SPU
        if mach not in (EM_PPC64, EM_SPU):
            print(f"{path}: machine {mach} is neither PPU nor SPU, skipping", file=sys.stderr)
            continue
        objdump = args.objdump or os.environ.get("OBJDUMP_SPU" if is_spu else "OBJDUMP_PPU")
        if not objdump:
            raise SystemExit("error: pass --objdump or set OBJDUMP_%s"
                             % ("SPU" if is_spu else "PPU"))
        total, matched, mism, samples = audit(objdump, path, is_spu)
        arch = "SPU" if is_spu else "PPU"
        pct = 100 * matched / total if total else 0
        print(f"{os.path.basename(path)} [{arch}]: {matched}/{total} mnemonics match ({pct:.2f}%)")
        grand_total += total; grand_match += matched
        for k, v in mism.items():
            all_mism[k] += v
            all_samples.setdefault(k, samples[k])

    if grand_total:
        print(f"\nTOTAL: {grand_match}/{grand_total} "
              f"({100*grand_match/grand_total:.2f}%)  mismatch classes: {len(all_mism)}")
    if all_mism:
        print(f"\ntop mismatches (ours -> objdump):")
        for (ours, oracle), n in all_mism.most_common(args.show):
            print(f"  {n:5}x  {ours:12} -> {oracle:12}  e.g. 0x{all_samples[(ours, oracle)]:X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

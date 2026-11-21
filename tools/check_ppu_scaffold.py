#!/usr/bin/env python3
import argparse
import importlib.util
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tools", "ppu_scaffold_baseline.json")

SOURCES = [
    "runtime/ppu/ppu_loader.cpp",
    "runtime/ppu/ppu_hle.cpp",
    "runtime/ppu/ppu_fs.cpp",
    "runtime/ppu/ppu_sysprx.cpp",
]

INCLUDE_DIRS = [
    "include",
    "runtime/ppu",
    "runtime/spu",
    "runtime/memory",
    "runtime/syscalls",
    "runtime/platform",
    "libs/video",
]


def stub_header() -> str:
    spec = importlib.util.spec_from_file_location(
        "ppu_lifter", os.path.join(ROOT, "tools", "ppu_lifter.py"))
    lifter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifter)
    return lifter.HEADER_PREAMBLE + (
        '\n#ifdef __cplusplus\nextern "C" {\n#endif\n'
        "void ppu_recomp_register(void);\n"
        '#ifdef __cplusplus\n}\n#endif\n'
    )


def compiler_family(cxx: str) -> str:
    try:
        out = subprocess.run([cxx, "--version"], capture_output=True, text=True,
                             timeout=15)
        text = (out.stdout + out.stderr).lower()
        if "clang" in text:
            return "clang"
        if "gcc" in text or "free software foundation" in text:
            return "gcc"
    except Exception:
        pass
    return "clang" if "clang" in os.path.basename(cxx) else "gcc"


def pick_toolchain(explicit=None):
    cxx = explicit or os.environ.get("CXX") or "c++"
    family = compiler_family(cxx)
    osname = {"win32": "windows", "darwin": "darwin"}.get(sys.platform, "linux")
    argv = [cxx, "-std=c++20", "-fsyntax-only", "-w"]
    if family == "clang":
        argv.append("-ferror-limit=0")
    return f"{osname}-{family}", argv


def count_errors(text: str) -> int:
    return len(re.findall(r"(?m)^[^\n]*?\berror:", text))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true",
                    help="re-record the baseline for this toolchain")
    ap.add_argument("--cxx", help="compiler to use (default: $CXX, or the "
                                  "platform's usual)")
    ap.add_argument("--verbose", action="store_true",
                    help="print the compiler diagnostics")
    args = ap.parse_args()

    key, prefix = pick_toolchain(args.cxx)

    tmp = tempfile.mkdtemp(prefix="ppu_scaffold_")
    try:
        with open(os.path.join(tmp, "ppu_recomp.h"), "w") as fh:
            fh.write(stub_header())

        incs = ["-I" + tmp]
        incs += ["-I" + os.path.join(ROOT, d) for d in INCLUDE_DIRS]

        print(f"[ppu-scaffold] toolchain: {key}")
        print(f"[ppu-scaffold] {' '.join(os.path.basename(p) for p in prefix[:1])}"
              f" {platform.machine()}")

        results, total = {}, 0
        for src in SOURCES:
            cmd = list(prefix) + incs + [os.path.join(ROOT, src)]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            n = count_errors(proc.stdout + proc.stderr)
            results[src] = n
            total += n
            print(f"  {n:4d}  {src}")
            if args.verbose and n:
                print((proc.stdout + proc.stderr).strip()[:4000])
        print(f"  {total:4d}  TOTAL")

        base = {}
        if os.path.exists(BASELINE):
            with open(BASELINE) as fh:
                base = json.load(fh)

        if args.update:
            base[key] = results
            with open(BASELINE, "w") as fh:
                json.dump(base, fh, indent=2, sort_keys=True)
                fh.write("\n")
            print(f"[ppu-scaffold] baseline recorded for {key}")
            return 0

        if key not in base:
            print(f"[ppu-scaffold] no baseline for {key} yet -- reporting only.")
            print(f"[ppu-scaffold] record it with: "
                  f"python tools/check_ppu_scaffold.py --update")
            return 0

        want, bad, better = base[key], [], []
        for src in SOURCES:
            got, exp = results[src], want.get(src)
            if exp is None:
                print(f"[ppu-scaffold] {src} is new; run --update")
                continue
            if got > exp:
                bad.append(f"{src}: {exp} -> {got}")
            elif got < exp:
                better.append(f"{src}: {exp} -> {got}")

        for line in better:
            print(f"[ppu-scaffold] IMPROVED  {line}")
        if better and not bad:
            print("[ppu-scaffold] the port moved forward -- re-record with "
                  "--update so the ratchet holds the new ground")
        if bad:
            print("\n[ppu-scaffold] REGRESSION: the scaffold got worse on "
                  f"{key}")
            for line in bad:
                print(f"  {line}")
            print("\nRe-run with --verbose to see the diagnostics.")
            return 1

        print(f"[ppu-scaffold] OK -- no regression against the {key} baseline")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

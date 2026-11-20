#!/usr/bin/env python3
import argparse
import json
import os
import sys

import idapro

import ida_funcs
import ida_auto
import idautils


def main():
    ap = argparse.ArgumentParser(description="Headless IDA function export for ps3recomp")
    ap.add_argument("elf", help="Path to decrypted EBOOT.elf")
    ap.add_argument("-o", "--output", default="ida_out", help="Output dir for functions.json")
    args = ap.parse_args()

    elf = os.path.abspath(args.elf)
    if not os.path.isfile(elf):
        print(f"ERROR: ELF not found: {elf}", file=sys.stderr)
        sys.exit(1)
    out_dir = os.path.abspath(args.output)
    os.makedirs(out_dir, exist_ok=True)

    print(f"[ida] opening {elf}", flush=True)
    if idapro.open_database(elf, run_auto_analysis=True) != 0:
        print("ERROR: idapro.open_database failed", file=sys.stderr)
        sys.exit(2)
    try:
        ida_auto.auto_wait()
        funcs = []
        for ea in idautils.Functions():
            f = ida_funcs.get_func(ea)
            if not f:
                continue
            funcs.append({
                "addr": f"0x{ea:08X}",
                "size": int(f.end_ea - f.start_ea),
                "name": ida_funcs.get_func_name(ea) or "",
                "thunk": bool(f.flags & ida_funcs.FUNC_THUNK),
            })
        path = os.path.join(out_dir, "functions.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(funcs, fh)
        print(f"[ida] functions: {len(funcs)} -> {path}", flush=True)
    finally:
        idapro.close_database(save=False)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
import argparse, json, struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elf_parser


def ha16(v):
    return ((v >> 16) + ((v >> 15) & 1)) & 0xFFFF


def relocate(prx_path, base):
    f = elf_parser.ELFFile(prx_path)
    f.load()
    loads = [p for p in f.program_headers if p.p_type == 1]
    if not loads:
        raise SystemExit("no PT_LOAD segments")
    memsz = max(p.p_vaddr + p.p_memsz for p in loads)

    img = bytearray(memsz)
    for p in loads:
        seg = f.raw_data[p.p_offset:p.p_offset + p.p_filesz]
        img[p.p_vaddr:p.p_vaddr + len(seg)] = seg

    seg_vaddr = [p.p_vaddr for p in loads]
    counts = {1: 0, 4: 0, 6: 0}
    skipped = 0
    seg_errs = 0
    for r in f.relocations:
        t = r.r_type
        patched_seg = r.r_sym & 0xFF
        symbol_seg = (r.r_sym >> 8) & 0xFF
        if patched_seg >= len(seg_vaddr) or symbol_seg >= len(seg_vaddr):
            seg_errs += 1
            continue
        off = seg_vaddr[patched_seg] + r.r_offset
        value = (base + seg_vaddr[symbol_seg] + r.r_addend) & 0xFFFFFFFF
        if t == 1:
            struct.pack_into(">I", img, off, value)
        elif t == 4:
            struct.pack_into(">H", img, off, value & 0xFFFF)
        elif t == 6:
            struct.pack_into(">H", img, off, ha16(value))
        else:
            skipped += 1
            continue
        counts[t] = counts.get(t, 0) + 1

    manifest = {
        "source": os.path.basename(prx_path),
        "base": base,
        "memsz": memsz,
        "filesz_total": sum(p.p_filesz for p in loads),
        "segments": [
            {"vaddr": p.p_vaddr, "filesz": p.p_filesz, "memsz": p.p_memsz,
             "flags": p.p_flags}
            for p in loads
        ],
        "reloc_applied": counts,
        "reloc_skipped": skipped,
        "reloc_seg_errors": seg_errs,
        "module_info": {
            "name": getattr(getattr(f, "module_info", None), "name", None),
            "n_imports": len(getattr(f, "imports", []) or []),
            "n_exports": len(getattr(f, "exports", []) or []),
        },
    }
    return bytes(img), manifest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prx")
    ap.add_argument("--base", required=True,
                    help="fixed load base, e.g. 0x0D000000")
    ap.add_argument("--out", required=True, help="output linked image (.bin)")
    ap.add_argument("--manifest", help="output manifest JSON (default: <out>.json)")
    args = ap.parse_args()
    base = int(args.base, 0)
    if base & 0xFFFF:
        raise SystemExit("base must be 64KB-aligned for clean HA/LO relocation")

    img, manifest = relocate(args.prx, base)
    with open(args.out, "wb") as fh:
        fh.write(img)
    mpath = args.manifest or (args.out + ".json")
    with open(mpath, "w") as fh:
        json.dump(manifest, fh, indent=2)

    print("linked %s @ base 0x%08X" % (manifest["source"], base))
    print("  image %d bytes (filesz %d)" % (manifest["memsz"], manifest["filesz_total"]))
    print("  relocations applied:", manifest["reloc_applied"],
          "skipped:", manifest["reloc_skipped"])
    print("  -> %s\n  -> %s" % (args.out, mpath))


if __name__ == "__main__":
    main()

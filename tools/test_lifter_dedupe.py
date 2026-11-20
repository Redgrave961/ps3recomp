#!/usr/bin/env python3
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppu_lifter import LiftedFunction, PPULifter


def _lifter():
    try:
        return PPULifter.__new__(PPULifter)
    except Exception as exc:
        print(f"cannot construct lifter: {exc}")
        raise


def main() -> int:
    lf = _lifter()
    lf.functions = [
        LiftedFunction(name="f_1848", start_addr=0x1848, end_addr=0x1850,
                       body_lines=["/* truncated */"]),
        LiftedFunction(name="f_1848", start_addr=0x1848, end_addr=0x1874,
                       body_lines=["/* full */"]),
        LiftedFunction(name="f_1874", start_addr=0x1874, end_addr=0x1900,
                       body_lines=["/* other */"]),
    ]
    out = lf._emit_functions()

    starts = [f.start_addr for f in out]
    assert len(starts) == len(set(starts)), f"duplicate starts survived: {starts}"
    assert len(out) == 2, f"expected 2 functions, got {len(out)}"

    kept = next(f for f in out if f.start_addr == 0x1848)
    assert kept.end_addr == 0x1874, (
        f"kept the truncated body (end=0x{kept.end_addr:X}); widest extent must win")

    assert starts == [0x1848, 0x1874], f"emission order changed: {starts}"

    lf.functions = [
        LiftedFunction(name="a", start_addr=0x10, end_addr=0x20),
        LiftedFunction(name="b", start_addr=0x20, end_addr=0x30),
    ]
    assert len(lf._emit_functions()) == 2

    from spu_lifter import SPULifter
    for cls, kw in ((PPULifter, {}), (SPULifter, {})):
        assert cls(prefix="libsre", **kw).prefix == "libsre_", (
            f"{cls.__name__} left a prefix without a trailing underscore")
        assert cls(prefix="libsre_", **kw).prefix == "libsre_", (
            f"{cls.__name__} double-appended the underscore")
        assert cls(prefix="", **kw).prefix == "", (
            f"{cls.__name__} invented a prefix from an empty one")

    lf2 = _lifter()
    lf2.code_lo, lf2.code_hi = 0x10000, 0x1D718
    lf2.hle_stub_nids = {0x1DA98: 0xEBE5F72F}
    assert lf2._outside_code(0x1DA98) is False, "import stub treated as non-code"
    assert lf2._outside_code(0x1E000) is True,  "real data accepted as code"
    assert lf2._outside_code(0x12000) is False, "in-range code rejected"
    lf2.code_hi = None
    assert lf2._outside_code(0x1E000) is False, "no code-end must disable the check"

    print("ok: truncated duplicate dropped, widest extent kept, order "
          "preserved, symbol prefix normalised, import stubs stay callable")
    return 0


if __name__ == "__main__":
    sys.exit(main())

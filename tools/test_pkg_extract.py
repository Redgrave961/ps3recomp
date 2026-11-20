import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pkg_extract import out_path

OUT = os.path.join("out", "dir")


def under_out(path):
    root = os.path.abspath(OUT)
    return os.path.commonpath([root, os.path.abspath(path)]) == root


def main():
    j = lambda *p: os.path.join(OUT, *p)

    assert out_path(OUT, "PARAM.SFO") == j("PARAM.SFO")
    assert out_path(OUT, "USRDIR/EBOOT.BIN") == j("USRDIR", "EBOOT.BIN")
    assert out_path(OUT, "USRDIR\\EBOOT.BIN") == j("USRDIR", "EBOOT.BIN")

    a = out_path(OUT, "USRDIR/PDIPFS/A/YG")
    b = out_path(OUT, "USRDIR/PDIPFS/B/YG")
    assert a != b, "PDIPFS entries collapsed onto one path"

    for hostile in ("../evil", "../../evil", "a/../../evil", "/etc/passwd",
                    "C:/Windows/System32/evil", "./a//b", "a/./b"):
        p = out_path(OUT, hostile)
        assert under_out(p), f"{hostile!r} escaped to {p}"

    for empty in ("..", "/", "../..", "   "):
        try:
            out_path(OUT, empty)
        except ValueError:
            pass
        else:
            raise AssertionError(f"{empty!r} should have been rejected")

    print("pkg_extract.out_path: OK")


if __name__ == "__main__":
    main()

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import objdump_audit as oa
from ppu_disasm import decode as ppu_decode

SNIPPET = """\
Disassembly of section .text:

00010324 <foo>:
   10324:\t7c 60 1b 78 \tmr      r0,r3
   10328:\t37 ff ff ff \taddic.  r31,r31,-1
   1032c:\t60 00 00 00 \tnop
   10330:\t4e 80 00 20 \tblr
   10334:\t00 00 00 00 \t.long 0x0
"""


def main() -> int:
    rows = oa.parse_objdump_text(SNIPPET)
    got = {addr: (word, mn) for addr, word, mn in rows}

    assert 0x10334 not in got, "data (.long) line should be skipped"
    assert set(got) == {0x10324, 0x10328, 0x1032c, 0x10330}, sorted(hex(a) for a in got)

    assert got[0x10328][1] == "addic.", got[0x10328]

    assert got[0x10324][0] == 0x7C601B78, hex(got[0x10324][0])

    for addr, (word, oracle) in got.items():
        ours = ppu_decode(word, addr).mnemonic.lower()
        assert ours == oracle or (ours, oracle) in oa.ALIAS, \
            f"0x{addr:X}: ours {ours!r} vs objdump {oracle!r}"

    print("ok: objdump line parser (incl. record-form '.') and decoder agreement pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

from ppu_disasm import Instruction
from ppu_lifter import PPULifter, LiftedFunction, _last_line_is_terminator


def emit(mnemonic: str, operands: str = "") -> str:
    lifter = PPULifter(prefix="")
    func = LiftedFunction(name="f", start_addr=0x1000, end_addr=0x2000)
    return lifter._translate_op(Instruction(0x1000, 0, mnemonic, operands), func)


def main() -> int:
    blr, blrl = emit("blr"), emit("blrl")
    bctrl, bnelrl = emit("bctrl"), emit("bnelrl", "cr7")

    assert blr == "return;", blr

    assert "ppu_call_ctr" in blrl, blrl
    assert "ctx->lr" in blrl, blrl
    assert "return;" not in blrl, f"blrl must not return -- link = call: {blrl}"

    assert "ppu_call_to" in bnelrl, bnelrl
    assert "ctx->lr" in bnelrl, bnelrl
    assert "if (" in bnelrl and bnelrl.index("ctx->lr = ") < bnelrl.index("if ("), bnelrl

    assert bctrl == "ctx->lr = 0x00001004; ppu_call_ctr(ctx);", bctrl

    bdnz = emit("bdnz", "0x1000")
    assert bdnz.startswith("if ("), bdnz
    assert not _last_line_is_terminator([f"    {bdnz}"]), bdnz
    assert _last_line_is_terminator(["    goto loc_00001000;"])
    assert _last_line_is_terminator(["    return;"])

    print("ok: blr returns; blrl/bnelrl dispatch via LR and continue; "
          "conditional branches fall through")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
RUNTIME = TOOLS.parent / 'runtime' / 'spu'
sys.path.insert(0, str(TOOLS))

from spu_disasm import disassemble_spu
from spu_lifter import SPULifter, merge_chunk_groups


def ri16(op9, i16, rt):
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt


def ri10(op8, i10, ra, rt):
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt


def rr(op11, rb, ra, rt):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt


PROGRAM = [
    ri16(0x081, 0, 3),
    ri10(0x1C, 1, 3, 3),
    ri10(0x7C, 100, 3, 6),
    rr(0x041, 6, 5, 7),
    ri16(0x040, (0x04 - 0x10) >> 2, 7),
    rr(0x1A8, 0, 0, 0),
]
BOUNDS = [(0x00, 0x04), (0x04, 0x18)]

DRIVER = r'''
#include "lifted.h"
#include <assert.h>
#include <stdio.h>
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
volatile int g_spu_direct = 0;              /* SPU_DIRECT off: every hop drains */
static int returned;
void spu_indirect_branch(spu_context* ctx) { (void)ctx; returned = 1; }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }
int main(void) {
    static spu_context c;
    uint32_t taken_at = 0;
    c.int_enable = 1; c.event_mask = 1; c.event_status = 1;
    spu_func_00000000(&c);
    /* The drain: deliver a pending interrupt before each transfer. The
     * "handler" acknowledges the event and sets r5, ending the wait loop. */
    while (g_spu_trampoline_fn && !returned) {
        void (*fn)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        if (c.int_enable && (c.event_status & c.event_mask)) {
            taken_at = c.pc; c.event_status = 0; c.gpr[5]._u32[0] = 1;
        }
        fn(&c);
    }
    printf("%x %u\n", taken_at, c.gpr[3]._u32[0]);
    return 0;
}
'''


def lift(merge):
    insns = disassemble_spu(b''.join(struct.pack('>I', w) for w in PROGRAM), 0)
    lifter = SPULifter()
    lifter.func_starts = {s for s, _ in BOUNDS}
    groups = merge_chunk_groups(insns, BOUNDS, 4096, entry_points={0}) if merge else {}
    for s, e in BOUNDS:
        lifter.lift_function(insns, s, groups.get(s, e))
    return lifter


class MergeChunkInterrupts(unittest.TestCase):
    CHECK = (r'loc_00000004:\n    if \(ctx->int_enable && \(ctx->event_status & ctx->event_mask\)\) '
             r'\{ ctx->pc = 0x4; (ctx->gpr\[\d+\] = sr\d+; )*g_spu_trampoline_fn = spu_func_00000004; return; \}')

    def body(self, lifter, addr):
        return '\n'.join(next(f for f in lifter.functions if f.start_addr == addr).body_lines)

    def test_merged_entry_checks_for_pending_interrupt(self):
        merged = self.body(lift(True), 0)
        self.assertRegex(merged, self.CHECK)
        self.assertIn('goto loc_00000004;', merged)

    def test_unmerged_output_has_no_entry_check(self):
        lifter = lift(False)
        for addr in (0, 4):
            self.assertNotIn('ctx->event_mask', self.body(lifter, addr))

    def run_program(self, merge):
        cc = shutil.which('clang') or shutil.which('cc')
        if not cc:
            if os.environ.get('CI'):
                self.fail('C compiler required in CI')
            self.skipTest('C compiler unavailable')
        lifter = lift(merge)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            context = (RUNTIME / 'spu_context.h').as_posix()
            helpers = (RUNTIME / 'spu_helpers.h').as_posix()
            (root / 'lifted.h').write_text(lifter.emit_header().replace(
                '../../runtime/spu/spu_context.h', context))
            (root / 'lifted.c').write_text(lifter.emit_source('lifted.h').replace(
                '../../runtime/spu/spu_helpers.h', helpers))
            (root / 'driver.c').write_text(DRIVER)
            exe = root / ('proof.exe' if sys.platform == 'win32' else 'proof')
            built = subprocess.run([cc, '-O1', '-I', str(RUNTIME), '-o', str(exe),
                                    str(root / 'lifted.c'), str(root / 'driver.c')],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            return ran.stdout.split()

    def test_unmerged_loop_takes_interrupt_at_chunk_entry(self):
        self.assertEqual(self.run_program(False), ['4', '1'])

    def test_merged_loop_takes_interrupt_at_chunk_entry(self):
        self.assertEqual(self.run_program(True), ['4', '1'])


if __name__ == '__main__':
    unittest.main()

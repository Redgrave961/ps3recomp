#!/usr/bin/env python3
"""sys_lwmutex_trylock on a mutex another thread holds returns CELL_EBUSY.

lv2 answers a contended trylock with CELL_EBUSY (0x8001000A). The context-style
HLE in runtime/ppu/ppu_sysprx.cpp returned 0x8001000B, CELL_ETIMEDOUT, which a
real kernel only gives a lock whose timeout expired. Titles that poll with
trylock and test for EBUSY then treat the busy case as a hard failure; FIOS in
PlayStation All-Stars Battle Royale (BCUS98472) logs
"sys_lwmutex_trylock failed on mutex 'dearchiver_opticalFunnel'" on every
contended poll.

Builds the real lwmutex handlers (the block pulled verbatim out of
ppu_sysprx.cpp) with the real park/wait layer (runtime/platform/ps3_sync.c)
into a small host program over guest memory, and checks:
  - a trylock on a free mutex succeeds and stamps the owner, recursion 0;
  - a trylock by a second thread while it is held returns CELL_EBUSY, not
    CELL_ETIMEDOUT, and leaves the holder's owner/recursion untouched.
"""
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def lwmutex_block(src):
    start = src.index('#define LWM_OWNER')
    m = re.search(r'static void sys_lwcond_wait\(ppu_context\* ctx\)\s*\{', src)
    assert m, 'sys_lwcond_wait not found'
    i, depth = m.end(), 1
    while depth:
        depth += {'{': 1, '}': -1}.get(src[i], 0)
        i += 1
    return src[start:i]


HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "ps3_sync.h"
alignas(64) static uint8_t mem[0x10000];
extern "C" uint8_t* vm_base = mem;
extern "C" void vm_write32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; i++) mem[a + i] = (uint8_t)(v >> (24 - 8 * i)); }
extern "C" uint32_t vm_read32(uint64_t a) { return (uint32_t)mem[a] << 24 | mem[a + 1] << 16 | mem[a + 2] << 8 | mem[a + 3]; }
extern "C" int32_t ppu_thread_priority(uint64_t) { return 1000; }
struct ppu_context { uint64_t gpr[32]; uint64_t thread_id; };
%s
extern "C" void ppu_thread_note_block(uint64_t) {}
static int32_t ret(const ppu_context& c) { return (int32_t)(uint32_t)c.gpr[3]; }
int main() {
    const uint32_t LWM = 0x1000, A = 0x101, B = 0x102;
    int fails = 0, ok;

    ppu_context c = {};
    c.gpr[3] = LWM; c.thread_id = A;
    sys_lwmutex_trylock(&c);
    ok = ret(c) == 0 && vm_read32(LWM + LWM_OWNER) == A && vm_read32(LWM + LWM_RECUR) == 0;
    printf("[%%s] free mutex: thread A acquires (ret 0x%%08X, owner 0x%%X, recur %%u)\n",
           ok ? "PASS" : "FAIL", (uint32_t)ret(c), vm_read32(LWM + LWM_OWNER), vm_read32(LWM + LWM_RECUR));
    fails += !ok;

    c = ppu_context{};
    c.gpr[3] = LWM; c.thread_id = B;
    sys_lwmutex_trylock(&c);
    ok = (uint32_t)ret(c) == 0x8001000Au && c.gpr[3] == 0xFFFFFFFF8001000Aull;
    printf("[%%s] held by A: thread B gets CELL_EBUSY (ret 0x%%08X)%%s\n",
           ok ? "PASS" : "FAIL", (uint32_t)ret(c),
           (uint32_t)ret(c) == 0x8001000Bu ? " -- that is CELL_ETIMEDOUT" : "");
    fails += !ok;
    ok = vm_read32(LWM + LWM_OWNER) == A && vm_read32(LWM + LWM_RECUR) == 0;
    printf("[%%s] held by A: owner and recursion unchanged (owner 0x%%X, recur %%u)\n",
           ok ? "PASS" : "FAIL", vm_read32(LWM + LWM_OWNER), vm_read32(LWM + LWM_RECUR));
    fails += !ok;
    return fails ? 1 : 0;
}
'''


def build(tmp, code):
    cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    cxx = os.environ.get('CXX') or shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
    if not cc or not cxx:
        return None
    c = Path(tmp) / 't.cpp'
    obj = Path(tmp) / 'ps3_sync.o'
    exe = Path(tmp) / 't'
    c.write_text(code)
    platform = ROOT / 'runtime/platform'
    libs = ['-lsynchronization'] if os.name == 'nt' else ['-lpthread']
    for cmd in ([cc, '-std=gnu17', '-O1', '-w', '-c', str(platform / 'ps3_sync.c'), '-o', str(obj)],
                [cxx, '-std=c++17', '-O1', '-w', '-I' + str(platform), str(c), str(obj), '-o', str(exe), *libs]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[-2000:])
            raise SystemExit('FAIL: harness did not compile')
    return exe


def main():
    src = (ROOT / 'runtime/ppu/ppu_sysprx.cpp').read_text()
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, HARNESS % lwmutex_block(src))
        if not exe:
            print('SKIP: no C/C++ compiler')
            return
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout, end='')
        if r.returncode:
            raise SystemExit('FAIL: contended sys_lwmutex_trylock')
    print('PASS: contended sys_lwmutex_trylock returns CELL_EBUSY')


if __name__ == '__main__':
    main()

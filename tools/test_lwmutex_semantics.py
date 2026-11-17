#!/usr/bin/env python3
"""sys_lwmutex_create / sys_lwmutex_trylock in the context-style HLE (runtime/ppu/ppu_sysprx.cpp).

Builds the real lwmutex handlers (the block pulled verbatim out of
ppu_sysprx.cpp) with the real park/wait layer (runtime/platform/ps3_sync.c)
into a small host program over guest memory, and checks:
  - sys_lwmutex_t.attribute (+0x08) is protocol | recursive, both words of
    sys_lwmutex_attribute_t, not the protocol alone;
  - a trylock of a lwmutex another thread holds answers CELL_EBUSY (0x8001000A),
    not 0x8001000B, which is CELL_ETIMEDOUT;
  - a free lwmutex is taken (recursive_count stays 0), and the owner's trylock
    nests (recursive_count 1), as lv2's sys_lwmutex_t counts extra levels.
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
static int fails = 0;
static void check(bool ok, const char* what, unsigned long long got)
{
    printf("[%%s] %%s (0x%%llX)\n", ok ? "PASS" : "FAIL", what, got);
    fails += !ok;
}
int main() {
    const uint32_t LWM = 0x1000, ATTR = 0x2000;
    vm_write32(ATTR + 0, 0x2);
    vm_write32(ATTR + 4, 0x10);
    ppu_context c = {};
    c.gpr[3] = LWM; c.gpr[4] = ATTR; c.thread_id = 1;
    sys_lwmutex_create(&c);
    check(c.gpr[3] == 0, "create returns CELL_OK", c.gpr[3]);
    check(vm_read32(LWM + LWM_ATTR) == 0x12, "attribute is protocol | recursive", vm_read32(LWM + LWM_ATTR));

    c = {}; c.gpr[3] = LWM; c.thread_id = 1;
    sys_lwmutex_trylock(&c);
    check(c.gpr[3] == 0 && vm_read32(LWM + LWM_OWNER) == 1 && vm_read32(LWM + LWM_RECUR) == 0,
          "trylock on a free lwmutex takes it", c.gpr[3]);
    c = {}; c.gpr[3] = LWM; c.thread_id = 1;
    sys_lwmutex_trylock(&c);
    check(c.gpr[3] == 0 && vm_read32(LWM + LWM_RECUR) == 1, "the owner's trylock nests", c.gpr[3]);
    c = {}; c.gpr[3] = LWM; c.thread_id = 2;
    sys_lwmutex_trylock(&c);
    check((uint32_t)c.gpr[3] == 0x8001000Au, "trylock while another thread holds it is CELL_EBUSY", c.gpr[3]);
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
            raise SystemExit('FAIL: lwmutex semantics')
    print('PASS: lwmutex attribute word and trylock result')


if __name__ == '__main__':
    main()

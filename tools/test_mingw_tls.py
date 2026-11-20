#!/usr/bin/env python3
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFINE = re.compile(r'^\s*#\s*define\s+(\w+)\s+__declspec\(thread\)\s*$')
COND_OPEN = re.compile(r'^\s*#\s*if')
COND_MID = re.compile(r'^\s*#\s*(else|elif)\b')
COND_END = re.compile(r'^\s*#\s*endif\b')


def sites():
    for sub in ('runtime', 'libs'):
        for path in sorted((ROOT / sub).rglob('*')):
            if path.suffix not in ('.c', '.h', '.cpp') or '/tests/' in path.as_posix():
                continue
            lines = path.read_text(errors='replace').splitlines()
            stack = []
            for i, line in enumerate(lines):
                if COND_OPEN.match(line):
                    stack.append(i)
                elif COND_END.match(line) and stack:
                    stack.pop()
                m = DEFINE.match(line)
                if not m or not stack:
                    continue
                start, depth, end = stack[-1], 0, None
                for j in range(start, len(lines)):
                    if COND_OPEN.match(lines[j]):
                        depth += 1
                    elif COND_END.match(lines[j]):
                        depth -= 1
                        if depth == 0:
                            end = j
                            break
                name = m.group(1)
                keep = [ln for ln in lines[start:end + 1]
                        if COND_OPEN.match(ln) or COND_MID.match(ln) or COND_END.match(ln)
                        or re.match(r'^\s*#\s*define\s+%s\b' % name, ln)]
                yield path.relative_to(ROOT).as_posix(), i + 1, name, '\n'.join(keep)


TLS_ASM = re.compile(r'\.tls|__emutls|%fs:|@tpoff|@tlsgd|@gottpoff|tprel|tlsdesc|tpidr_el0', re.I)


def probe(cmd, tmp, code):
    src = Path(tmp) / 'probe.c'
    asm = Path(tmp) / 'probe.s'
    src.write_text(code)
    r = subprocess.run(cmd + ['-O1', '-S', '-o', str(asm), str(src)], capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr.strip()[:200]
    return bool(TLS_ASM.search(asm.read_text())), ''


def main():
    mingw = shutil.which('x86_64-w64-mingw32-gcc')
    if mingw:
        cmd, how = [mingw], 'x86_64-w64-mingw32-gcc'
    else:
        if os.name == 'nt' or sys.platform == 'darwin':
            print(f'SKIP: no x86_64-w64-mingw32-gcc, and the host-compiler emulation needs an ELF host '
                  f'(this is {sys.platform})')
            return
        cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
        if not cc:
            print('SKIP: no MinGW gcc and no host C compiler')
            return
        with tempfile.TemporaryDirectory() as tmp:
            yes, _ = probe([cc], tmp, 'static __thread int v;\nint* get(void) { return &v; }\n')
            no, _ = probe([cc], tmp, 'static int v;\nint* get(void) { return &v; }\n')
        if yes is not True or no is not False:
            print(f'SKIP: cannot tell thread-local from global in {os.path.basename(cc)} output on this host '
                  f'(controls: __thread -> {yes}, plain static -> {no})')
            return
        cmd = [cc, '-D_WIN32', '-U_MSC_VER', '-D__declspec(x)=__attribute__((x))']
        how = f'{os.path.basename(cc)} with MinGW defines'
    found = list(sites())
    assert found, 'no __declspec(thread) definitions found'
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for rel, line, name, block in found:
            tls, err = probe(cmd, tmp, f'{block}\nstatic {name} int v;\nint* get(void) {{ return &v; }}\n')
            if tls is None:
                print(f'FAIL {rel}:{line} {name}: probe does not compile: {err}')
                bad += 1
                continue
            print(f"{'ok  ' if tls else 'FAIL'} {rel}:{line} {name}: "
                  f"{'thread-local' if tls else 'process-wide global'}")
            bad += not tls
    print(f'{len(found)} qualifiers checked ({how})')
    if bad:
        raise SystemExit(f'FAIL: {bad} thread-local qualifier(s) are process-wide under MinGW')
    print('PASS')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LLVM_MINGW_CC = r"E:\tools\llvm-mingw\bin\x86_64-w64-mingw32-clang.exe"
DECLSPEC_THREAD = re.compile(r'__declspec\s*\(\s*thread\s*\)')
PROBES = (
    ('c', 'static __thread int v;\nint* get(void) { return &v; }\n'),
    ('c', 'static _Thread_local int v;\nint* get(void) { return &v; }\n'),
    ('c++', 'extern constinit thread_local int v;\nint* get() { return &v; }\n'),
)


def sources():
    for sub in ('runtime', 'libs', 'include'):
        for path in sorted((ROOT / sub).rglob('*')):
            if path.suffix in ('.c', '.h', '.cpp') and '/tests/' not in path.as_posix() \
                    and not path.name.startswith('stb_'):
                yield path


def mingw_cc():
    if os.name == 'nt':
        return LLVM_MINGW_CC if os.path.exists(LLVM_MINGW_CC) else None
    return shutil.which('x86_64-w64-mingw32-clang')


def main():
    bad = [f'{p.relative_to(ROOT).as_posix()}:{i}'
           for p in sources()
           for i, line in enumerate(p.read_text(errors='replace').splitlines(), 1)
           if DECLSPEC_THREAD.search(line)]
    for site in bad:
        print(f'FAIL {site}: __declspec(thread) is a process-wide global under MinGW; use __thread')
    cc = mingw_cc()
    if not cc:
        print('SKIP native TLS probe: no llvm-mingw compiler')
    else:
        with tempfile.TemporaryDirectory() as tmp:
            for lang, code in PROBES:
                src = Path(tmp) / ('probe.c' if lang == 'c' else 'probe.cpp')
                asm = Path(tmp) / 'probe.s'
                src.write_text(code)
                r = subprocess.run([cc, '-x', lang, '-std=c++20' if lang == 'c++' else '-std=gnu17',
                                    '-O2', '-S', '-o', str(asm), str(src)],
                                   capture_output=True, text=True)
                text = asm.read_text() if r.returncode == 0 else ''
                native = '_tls_index' in text and '__emutls' not in text and '_ZTW' not in text
                print(f"{'ok  ' if native else 'FAIL'} {code.splitlines()[0]}: "
                      f"{'native TLS' if native else r.stderr.strip()[:200] or 'not native TLS'}")
                bad += [] if native else [code]
    if bad:
        raise SystemExit(f'FAIL: {len(bad)} thread-local problem(s)')
    print('PASS')


if __name__ == '__main__':
    main()

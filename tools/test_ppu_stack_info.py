#!/usr/bin/env python3
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ppu-stack-info-') as tmp:
    binary=Path(tmp)/('test.exe' if os.name=='nt' else 'test')
    if os.name=='nt':
        platform=[str(root/'runtime/memory/vm_track.c'),'-Wl,--gc-sections','-lsynchronization']
    elif sys.platform=='darwin':
        platform=[str(root/'runtime/platform/win32_compat.c'),'-Wl,-dead_strip']
    else:
        platform=[str(root/'runtime/platform/win32_compat.c'),'-Wl,--gc-sections','-pthread','-lm']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections',
        '-I'+str(root/'include'),'-I'+str(root/'runtime'),'-I'+str(root/'runtime/syscalls'),
        str(root/'tests/test_ppu_stack_info.c'),str(root/'runtime/platform/ps3_sync.c'),*platform,'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60,stderr=subprocess.DEVNULL if os.environ.get('CI') else None)

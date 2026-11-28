#!/usr/bin/env python3
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ppu-prio-start-') as tmp:
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
        str(root/'tests/test_ppu_prio_start.c'),str(root/'runtime/platform/ps3_sync.c'),*platform,'-o',str(binary)],check=True)
    quiet=subprocess.PIPE if os.environ.get('CI') else None
    for on in (False, True):
        env={k:v for k,v in os.environ.items() if k!='PS3_PRIO_START'}
        if on: env['PS3_PRIO_START']='1'
        result=subprocess.run([str(binary)],timeout=60,env=env,stderr=quiet,text=True)
        if result.returncode and result.stderr:
            sys.stderr.write(result.stderr)
        result.check_returncode()
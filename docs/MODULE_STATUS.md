# PS3 HLE Module Status

The HLE modules in this tree are the ones the port imports. Each keeps the
functions an import reaches or the runtime calls directly.

## Kernel (lv2)

| Module | Notes |
|---|---|
| sys_ppu_thread | Host threads, join, detach, priority, rename |
| sys_fs | File I/O with PS3-to-host path mapping, directory ops, stat |
| sys_memory, sys_mmapper | Allocator over the guest address space, containers, shared memory |
| sys_event | Event queues, ports, event flags |
| sys_mutex, sys_cond, sys_semaphore, sys_rwlock | Host-backed sync primitives with timeouts |
| sys_timer | Host clock, periodic event timers, usleep |
| sys_spu (thread groups, raw SPU) | Lifted SPU programs on host threads |

## Firmware modules run as lifted code

| Module | Notes |
|---|---|
| libsre | cellSpurs, cellSync, cellOvis, cellSheap |
| libresc | cellResc |

## HLE libraries

| Module | Notes |
|---|---|
| sysPrxForUser | lwmutex, lwcond, spinlocks, threads, heap, string/mem ops, printf (`runtime/ppu/ppu_sysprx.cpp` and `libs/system/sysPrxForUser.c`) |
| cellFs | File I/O, directories, AIO, SDATA/EDAT through NPDRM (`runtime/ppu/ppu_fs.cpp`) |
| cellSysmodule | Load/unload tracking |
| cellSysutil | Callbacks, system params, system cache |
| cellGame | Boot check, content permit, param read, game data |
| cellSaveData | AutoLoad2, Delete2, UserAutoLoad/UserAutoSave |
| cellMsgDialog, cellOskDialog | Dialogs answered without UI |
| cellUserInfo | Default user |
| cellRtc | Host time as PS3 ticks and date/time |
| cellPad, cellKb, cellMouse | XInput or SDL3 pads, keyboard, mouse |
| cellAudio, cellAvconfExt | WASAPI / SDL3 output, audio-out configuration and gamma |
| cellGcmSys, cellVideoOut | Command buffer, IO mapping, tiles and zcull, flip and vblank handlers; video-out configuration |
| cellPngDec | stb_image decode |
| sys_net | Offline model; host sockets with `PS3_NET_ONLINE` |
| cellNetCtl, cellHttp, cellHttps, cellSsl | Offline network state and lifecycle |
| sceNp, sceNpLookup, sceNpCommerce2, sceNpTrophy | Offline NP manager, lookup and commerce; trophies stored on the host |

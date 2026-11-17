# ps3recomp

### *Because the Cell processor deserves a second life*

> Static recompilation runtime libraries for PlayStation 3 titles.
> Turn PS3 binaries into native executables. No emulator required.

**[Join the sp00nznet recomp Discord](https://discord.gg/CRpzGWZFcu)** — the
community hub for sp00nznet's recomp projects.

---

## What Is This?

**ps3recomp** is the toolkit — runtime, HLE system libraries and lifting tools —
that the Asura's Wrath port (`../port`, BLES01227) is built on. PPU and SPU code
is translated ahead of time into C/C++ and linked against this runtime; Sony's
own libsre and libresc run as lifted firmware code.

The same philosophy is behind:
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) (N64 -> native)
- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) (Xbox 360 -> native)
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) (PS2 -> native)
- [ReXGlue](https://github.com/rexglue/rexglue-sdk) (Xbox 360 -> native)

## The Challenge

| Component | What It Is | Why It's Hard |
|-----------|-----------|---------------|
| **PPU** | PowerPC 64-bit main CPU | Custom ISA extensions, VMX/AltiVec SIMD |
| **6x SPU** | 128-bit SIMD vector processors | Proprietary ISA, 256KB local store, explicit DMA |
| **Memory** | 256MB XDR RAM | 128-byte alignment requirements, split bus |
| **LV2 Kernel** | Proprietary OS layer | ~300+ system calls, complex threading model |
| **PRX Modules** | Dynamic libraries | 100+ system libraries with intricate interdependencies |

## Architecture

```
ps3recomp/
├── tools/            # Analysis and lifting pipeline (ppu_loader, ppu_lifter, spu_lifter,
│                     # build_spu_workloads, lift_prx, lift_firmware_spu, lift_spurs_jobs,
│                     # gen_hle_nids, nid_database, npdrm, ...)
├── cmake/            # PS3RecompSPU.cmake: SPU program and SPURS job generation
├── runtime/
│   ├── ppu/          # Loader, HLE dispatch, cellFs, sysPrxForUser CRT (compiled by the port)
│   ├── spu/          # SPU context, channels, MFC DMA, interpreter, lifted-code dispatch
│   ├── syscalls/     # lv2 system calls
│   ├── memory/       # Guest address space
│   ├── platform/     # Win32 / POSIX abstraction
│   └── prx/          # Lifted firmware PRX loader
├── libs/             # HLE system libraries the title imports
│   ├── audio/        # cellAudio
│   ├── video/        # cellGcmSys, cellVideoOut, RSX command processing
│   ├── input/        # cellPad, cellKb, cellMouse
│   ├── network/      # cellNetCtl, cellHttp(s), cellSsl, sceNp, sceNpCommerce2, sceNpTrophy, sys_net
│   ├── filesystem/   # NPDRM (EDAT) decryption
│   ├── system/       # cellSysutil, cellSysmodule, cellGame, cellSaveData, cellRtc, sysPrxForUser
│   ├── codec/        # cellPngDec
│   └── misc/         # cellAvconfExt, cellOskDialog
├── include/ps3emu/   # Public headers
├── tests/            # Runtime unit tests
└── docs/
```

## How It Works

```
   EBOOT.BIN ──► decrypt ──► ppu_loader (functions, imports, TOC)
                                  │
                                  ▼
             ppu_lifter (PPU → C++)   spu_lifter (SPU → C)   lift_prx (libsre, libresc)
                                  │
                                  ▼
             link with ps3recomp runtime + HLE libraries ──► native executable
```

Imports resolve in `ps3_hle_call` (`runtime/ppu/ppu_hle.cpp`): fast handlers,
then the lifted firmware exports, then context handlers, then the generated NID
table (`tools/gen_hle_nids.py`).

## SPU Strategy

1. **SPU programs are self-contained** — they live in ELF segments loaded to local store (`extract_spu_images.py` pulls them out)
2. **Each SPU program is lifted separately** — `spu_disasm.py` + `spu_lifter.py` handle the SPU ISA
3. **Local store is an array** — 256KB per SPU context
4. **DMA is the MFC engine** — `spu_dma.h`
5. **Channels are cross-thread queues** — ordering preserved
6. **Per-image dispatch** — `image_id` lets several SPU programs coexist at overlapping local-store addresses; an interpreter covers code that was not lifted

SPURS runs as Sony's lifted libsre on real SPU threads and lv2 objects.

## Documentation

| Document | What It Covers |
|----------|---------------|
| **[Architecture](docs/ARCHITECTURE.md)** | Cell overview, pipeline stages, memory model |
| **[Building](docs/BUILDING.md)** | Runtime library build, options, tests |
| **[FAQ](docs/FAQ.md)** | Common questions and errors |
| **[Runtime Reference](docs/RUNTIME.md)** | Memory, PPU/SPU contexts, types, endianness, syscall dispatch, DMA |
| **[Syscall Reference](docs/SYSCALLS.md)** | LV2 kernel syscalls |
| **[NID System](docs/NID_SYSTEM.md)** | PS3 function linking and NID computation |
| **[Module Status](docs/MODULE_STATUS.md)** | Status table for the HLE modules |
| **[Tools Reference](docs/TOOLS.md)** | The pipeline tools |
| **[PPU Recompilation](docs/PPU_RECOMP.md)** | The PPU boot scaffold |
| **[SPU Lifter](docs/SPU_LIFTER.md)** | SPU lifting and the SPU runtime model |
| **[Firmware LLE](docs/FIRMWARE_LLE.md)** | Running Sony's own firmware modules as lifted code |
| **[Platform Abstraction](docs/PLATFORM_ABSTRACTION.md)** | Win32 vs POSIX |
| **[Debug Console](docs/DEBUG_CONSOLE.md)** | Query a running title without a rebuild |
| **[Runtime Diagnostics](docs/DIAGNOSTICS.md)** | Generated index of the runtime's environment variables |
| **[Prototype & Debug Builds](docs/PROTO_BUILDS.md)** | Unencrypted, symbol-bearing builds as ground truth |
| **[Changelog](CHANGELOG.md)** | Release history |

## Relationship to Other Projects

| Project | Role |
|---------|------|
| **[RPCS3](https://github.com/RPCS3/rpcs3)** | Primary reference for HLE module behaviour and system call semantics. |
| **[XenonRecomp](https://github.com/hedge-dev/XenonRecomp)** | PowerPC recompiler for Xbox 360; same ISA family and approach. |
| **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** | Pioneered the modern "recompile to C, link with runtime" approach. |

RPCS3 is a behavioural reference: we read what it does and write our own. No
RPCS3 (GPL) code belongs in this tree.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Legal

This project does not contain any proprietary Sony code, firmware, encryption keys, or copyrighted material ([docs/FIRMWARE_LLE.md](docs/FIRMWARE_LLE.md) explains how LLE works without shipping firmware). It provides clean-room implementations of system library interfaces based on publicly documented behavior. You must provide your own legally obtained PS3 game files.

Contributors are credited in **[CONTRIBUTORS.md](CONTRIBUTORS.md)**.

## License

MIT License. See [LICENSE](LICENSE) for details.

ps3recomp was created by Ned Heller ([@sp00nznet](https://github.com/sp00nznet)).
Ports, videos and write-ups built on it are welcome. Please name ps3recomp and
link this repository, and keep the copyright notice in LICENSE, which the MIT
licence requires.

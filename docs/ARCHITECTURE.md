# ps3recomp Architecture

Technical overview of how ps3recomp statically recompiles PS3 (Cell Broadband Engine) executables into native host code.

## Cell Architecture Overview

The PlayStation 3 uses the Cell Broadband Engine, which contains:

- **1x PPU (Power Processing Unit)**: A dual-threaded 64-bit PowerPC core (PPE) running at 3.2 GHz. This is the main CPU that runs game logic, OS calls, and orchestrates SPU tasks. It uses the Power ISA with VMX/AltiVec (128-bit SIMD) extensions.

- **6x SPUs (Synergistic Processing Units)**: Six available SPU cores (of 8 total; 1 reserved for the OS, 1 disabled for yield). Each SPU has its own 256 KB Local Store (LS) and communicates with main memory via DMA. SPUs run a custom SIMD-oriented instruction set, not PowerPC.

- **RSX (Reality Synthesizer)**: An NVIDIA NV47-based GPU with 256 MB dedicated VRAM and access to 256 MB of main RAM. Controlled via a command buffer (FIFO) written by the PPU.

- **256 MB main RAM** (XDR): Shared between PPU and SPUs via DMA.

- **256 MB VRAM** (GDDR3): Dedicated to the RSX GPU.

## Recompilation Pipeline

The pipeline transforms a PS3 ELF binary into native C/C++ source code that can be compiled for any host platform.

### Stage 1: ELF Loading and Analysis

1. Decrypt the SELF/ESELF container to obtain the raw ELF (`tools/decrypt_self.py`).
2. Parse ELF headers, program headers, and section headers.
3. Extract the `.text` (code), `.data`, `.rodata`, `.bss` segments.
4. Parse the `.prx` relocation tables and import/export tables (NIDs).

### Stage 2: PPU Disassembly

1. Disassemble all executable sections using the PowerPC 64-bit instruction set.
2. Identify function boundaries via:
   - Symbol table entries
   - `blr` (branch-to-link-register) return instructions
   - Call graph analysis from `bl` (branch-and-link) instructions
   - Pattern matching for function prologues (`mflr r0; stw r0, ...`)
3. Build a complete call graph.

### Stage 3: Control Flow Analysis

1. Identify basic blocks within each function.
2. Reconstruct control flow graphs (CFGs).
3. Detect and classify branch types:
   - Direct branches (`b`, `bl`) -- trivial targets
   - Conditional branches (`beq`, `bne`, `blt`, etc.) -- if/else structures
   - Indirect branches (`bctr`, `bctrl`) -- switch tables, virtual calls
4. Recover switch/jump tables by analyzing the `r12`-load + `mtctr` + `bctr` pattern.
5. Detect loop structures for potential optimization.

### Stage 4: Code Generation

1. Emit C source for each function:
   - Each PPU register maps to a local variable (`uint64_t r0..r31`)
   - CR (Condition Register) fields map to flag variables
   - VMX registers map to `u128` locals
   - Memory accesses go through `vm::read<T>()` / `vm::write<T>()` with endian conversion
2. Branch targets become `goto` labels or structured `if`/`while`/`switch`.
3. Function calls to known addresses become direct C function calls.
4. NID-resolved imports become calls to HLE stub functions.
5. Generate a function pointer table mapping guest addresses to host functions.

### Stage 5: Compilation

1. The generated C code is compiled by a standard host compiler (Clang/GCC/MSVC).
2. Linked against the ps3recomp runtime library (memory manager, thread manager, HLE stubs).
3. Produces a native executable.

## Memory Model

### Guest-to-Host Address Translation

PS3 uses 32-bit effective addresses (despite the 64-bit PPU, games use 32-bit mode). The runtime maps the entire 4 GB guest address space into host memory:

```
host_ptr = vm::g_base + guest_addr
```

`vm::g_base` is set at initialization to a large `mmap`/`VirtualAlloc` region.

### Endianness

PS3 is big-endian; x86/ARM hosts are little-endian. Every memory access in recompiled code passes through endian conversion:

- `be_t<T>` wraps values stored in big-endian format
- `vm::read<T>(addr)` reads from guest memory with byte-swap
- `vm::write<T>(addr, val)` writes to guest memory with byte-swap

Register-local computation stays in host-native byte order. Conversion only happens at memory boundaries.

### Memory Map

| Guest Address Range | Region |
|---|---|
| `0x00010000 - 0x0FFFFFFF` | User ELF text + data |
| `0x10000000 - 0x1FFFFFFF` | PRX module space |
| `0x20000000 - 0x2FFFFFFF` | Stack / TLS |
| `0x30000000 - 0x3FFFFFFF` | Heap (user malloc) |
| `0xC0000000 - 0xCFFFFFFF` | RSX local memory (VRAM, mapped) |
| `0xD0000000 - 0xDFFFFFFF` | RSX IO-mapped main memory |

## SPU Handling

SPU programs found in the ELF and in the firmware are lifted to C ahead of time
(`tools/spu_lifter.py`, `tools/build_spu_workloads.py`, `tools/lift_spurs_jobs.py`).
Code that is loaded at run time without a lifted counterpart runs on the SPU
interpreter (`runtime/spu/spu_interp.c`). SPURS is Sony's own libsre, lifted,
running on real SPU thread groups.

## Module / NID System

### How PS3 Linking Works

PS3 executables do not link against shared libraries by name. Instead, each exported/imported function has a unique 32-bit **NID (Name Identifier)** derived by:

```
NID = SHA-1(function_name)[:4]  (first 4 bytes, big-endian u32)
```

The ELF's import table lists: `(module_name, NID)` pairs. At load time, the PS3 OS resolves each NID to a function pointer from the loaded PRX modules.

### ps3recomp's Approach

1. `tools/ppu_loader.py` extracts the `(module, NID, stub)` imports.
2. `tools/ppu_lifter.py --hle-stubs` turns each import stub into `ps3_hle_call(nid)`.
3. `ps3_hle_call` resolves the NID: fast handlers, lifted firmware exports, context handlers, then the table `tools/gen_hle_nids.py` generates from the `libs/` sources (`tools/nid_database.py` computes the NIDs).
4. An unresolved NID is reported once and returns `CELL_OK`.

## RSX Graphics Pipeline

The RSX GPU is handled by a three-layer translation architecture:

```
cellGcmSys (HLE)  →  RSX Command Processor  →  Renderer
                          │
                    NV47xx FIFO parsing
                    GPU state tracking
```

1. **cellGcmSys** manages the interface between game code and the GPU: command buffer control, display buffers, memory mapping, flip/VBlank, tile/zcull configuration.

2. **RSX Command Processor** (`rsx_core.c`, `vk/rsx_frontend.cpp`) parses the NV47xx FIFO command buffer and tracks GPU state: surfaces, viewport, blend, depth/stencil, textures (16 units), vertex attributes (16), shader programs, draw commands.

3. **Renderer** receives state changes and draw calls from the command processor.

## Comparison with RPCS3

| Aspect | RPCS3 | ps3recomp |
|---|---|---|
| **Approach** | Dynamic emulation (JIT) | Static recompilation (AOT) |
| **PPU** | LLVM JIT or interpreter | C source generation, compiled natively |
| **SPU** | LLVM JIT or ASMJIT | Static recomp, interpreter fallback |
| **RSX** | Vulkan/OpenGL renderer | Vulkan renderer |
| **Compatibility** | Broad (~70% of library) | Per-game (each game is a project) |
| **Performance** | Good, JIT overhead | Potentially better (fully optimized AOT) |
| **Portability** | x86-64 primary, ARM64 WIP | Any platform with a C compiler |
| **Development** | One binary runs many games | Each game needs analysis and customization |
| **Memory** | Dynamic guest memory | Statically mapped guest address space |
| **HLE modules** | ~100 modules, varying completion | The modules the port imports (see [MODULE_STATUS.md](MODULE_STATUS.md)); libsre and libresc run as lifted firmware |
| **Use case** | General-purpose emulator | Targeted game ports and mods |

The key tradeoff: RPCS3 aims for broad compatibility across the PS3 library. ps3recomp trades that breadth for potentially higher performance and easier game-specific customization (modding, patching, porting to new platforms).

---

## Further Reading

For deeper dives into specific subsystems, see:

- **[RUNTIME.md](RUNTIME.md)** — Complete runtime reference (VM, PPU/SPU contexts, type system, endianness, syscall dispatch, DMA)
- **[SYSCALLS.md](SYSCALLS.md)** — All LV2 kernel syscall implementations documented
- **[NID_SYSTEM.md](NID_SYSTEM.md)** — How PS3 function linking works, NID computation, module registration
- **[TOOLS.md](TOOLS.md)** — Recompiler pipeline tools reference
- **[PLATFORM_ABSTRACTION.md](PLATFORM_ABSTRACTION.md)** — How Win32/POSIX differences are handled
- **[BUILDING.md](BUILDING.md)** — Build system reference

# Building ps3recomp

The runtime is built as part of the port: `port/CMakeLists.txt` adds this tree
with `add_subdirectory` and `port/build.ps1` drives the build. This page covers
the runtime library on its own.

## Prerequisites

| Tool | Version | Purpose |
|------|---------|---------|
| CMake | 3.20+ | Build system generator |
| Ninja | any | Generator used by the port and CI |
| clang-cl (Windows) / GCC or Clang (Linux) | C17, C++20 | Runtime and HLE modules |
| Python | 3.10+ | Lifter and generator tools |
| SDL3, Vulkan-Headers, volk, VulkanMemoryAllocator | | Found with `find_package(... CONFIG REQUIRED)`; vcpkg provides all four |

## Runtime library

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<vcpkg>/installed/<triplet>
cmake --build build
```

On Windows configure with `-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`
from an environment with the MSVC headers and libraries. Plain `cl` is not
supported: the runtime uses `__atomic_*`, `__int128` and `__builtin_bswap*`.

| CMake option | Default | Description |
|---|---|---|
| `PS3RECOMP_BUILD_TESTS` | `OFF` | Build and register the runtime tests |
| `PS3RECOMP_OPT_FLAG` | `-O2` | Optimisation flag for every non-Debug configuration |
| `PS3RECOMP_PAD_SDL` | `OFF` | Windows: SDL3 controller backend instead of XInput |

`runtime/ppu/` is excluded from the library: it includes the lifter-generated
`ppu_recomp.h`, so the port compiles it into its own target, together with
`runtime/spu/spu_dispatch_mt.c` (needs clang's `musttail`).

## Tests

```bash
cmake -S . -B build-tests -G Ninja -DPS3RECOMP_BUILD_TESTS=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

`tests/sync_stress` and `tests/syscalls` also configure standalone. The
`tools/test_*.py` suites run directly with Python.

## PPU boot scaffold check

```bash
python tools/check_ppu_scaffold.py            # check against the baseline
python tools/check_ppu_scaffold.py --verbose  # show the diagnostics
python tools/check_ppu_scaffold.py --update   # re-record after improving it
```

It synthesises a stub `ppu_recomp.h` from `ppu_lifter.py`'s `HEADER_PREAMBLE`
and compiles `runtime/ppu/` against it. `tools/ppu_scaffold_baseline.json`
records the error count per toolchain; the check fails if a count goes up.

## Platforms

- **Windows** (clang-cl): the platform the port runs on. Links `ws2_32`,
  `ole32`, `bcrypt`, `synchronization`.
- **Linux** (GCC, Clang): the runtime builds and its tests run in CI
  (`.github/workflows/linux.yml`). Links `Threads::Threads`; `_GNU_SOURCE` is
  defined for `dladdr`.

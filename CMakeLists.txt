cmake_minimum_required(VERSION 3.20)
# Keep in step with CHANGELOG.md. This sat at 0.3.0 until August
# 2026 while the project shipped 0.6.x through 0.8.0, so every build printed
# "ps3recomp runtime v0.3.0" and was believed. v0.9.0 is the first TAGGED one.
project(ps3recomp_runtime
    VERSION 0.19.0
    LANGUAGES C CXX
    DESCRIPTION "ps3recomp - Static recompilation runtime for PS3 games"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PS3RECOMP_BUILD_TESTS "Build unit tests" OFF)
set(PS3RECOMP_OPT_FLAG "-O2" CACHE STRING "Optimisation flag for every non-Debug configuration of the runtime and the lifted SPU code")

# ---------------------------------------------------------------------------
# Collect source files
# ---------------------------------------------------------------------------

# HLE library sources
file(GLOB_RECURSE HLE_SOURCES
    "libs/system/*.c"
    "libs/filesystem/*.c"
    "libs/input/*.c"
    "libs/audio/*.c"
    "libs/video/*.c"
    "libs/network/*.c"
    "libs/codec/*.c"
    "libs/misc/*.c"
)
# Exclude test harnesses (e.g. libs/video/tests/*): they define their own main().
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

# Runtime core (syscalls, PPU, SPU, memory)
file(GLOB_RECURSE RUNTIME_SOURCES
    "runtime/*.c"
    "runtime/*.cpp"
)
# Exclude:
#  - test harnesses (runtime/*/tests/*): they have their own main() and #include
#    per-game generated headers (spu_recomp.h) that a plain library build lacks.
#  - the PPU boot scaffold (runtime/ppu/): compiled by the port against the
#    lifter-generated ppu_recomp.h, not as part of the runtime library. See
#    docs/PPU_RECOMP.md.
#  - spu_dispatch_mt.c: the musttail SPU indirect-branch fast path. It uses
#    __attribute__((musttail)), which MSVC cannot parse, and it must be compiled
#    *inside the game build* by clang so the tail call is guaranteed -- a plain
#    call leaks one resolver frame per guest loop iteration. The port adds it to
#    its own target.
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/|/spu_dispatch_mt\.c$")

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------
add_library(ps3recomp_runtime STATIC
    ${HLE_SOURCES}
    ${RUNTIME_SOURCES}
)

target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# HLE modules include their own headers
target_include_directories(ps3recomp_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/input"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/network"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/codec"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/misc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

find_package(SDL3 CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
file(GLOB VK_RENDERER_SOURCES CONFIGURE_DEPENDS "libs/video/vk/*.cpp")
target_sources(ps3recomp_runtime PRIVATE ${VK_RENDERER_SOURCES})
set_source_files_properties(${VK_RENDERER_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "NOMINMAX;_CRT_SECURE_NO_WARNINGS"
    COMPILE_OPTIONS "-Wno-nullability-completeness;-Wno-missing-field-initializers")
set_source_files_properties("libs/video/vk/vk_device.cpp" PROPERTIES COMPILE_OPTIONS "-Wno-nullability-completeness;-Wno-missing-field-initializers;-Wno-unused-variable")
target_link_libraries(ps3recomp_runtime PRIVATE SDL3::SDL3 Vulkan::Headers volk::volk GPUOpen::VulkanMemoryAllocator)

option(PS3RECOMP_PAD_SDL "Windows: SDL3 controller backend (native DualSense/DualShock over USB and Bluetooth) instead of XInput" OFF)
if(WIN32)
    target_link_libraries(ps3recomp_runtime PRIVATE
        ws2_32
        ole32
        bcrypt
        synchronization
    )
    if(PS3RECOMP_PAD_SDL)
        target_compile_definitions(ps3recomp_runtime PRIVATE PS3RECOMP_PAD_USE_SDL)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(ps3recomp_runtime PRIVATE Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ps3recomp_runtime PRIVATE
        $<IF:$<STREQUAL:${CMAKE_C_COMPILER_FRONTEND_VARIANT},MSVC>,/W4,-Wall$<SEMICOLON>-Wextra>
        -Wno-unused-parameter
        -fno-strict-aliasing
        # No spontaneous fused multiply-add. MSVC never contracts, and x86-64
        # without -mfma has nothing to contract into, so every validated
        # result came from separate multiplies and adds; arm64 has FMA in
        # the base ISA and Clang fuses by default there. A PPC fmadd is
        # spelt as one where the lifter means one.
        $<IF:$<STREQUAL:${CMAKE_C_COMPILER_FRONTEND_VARIANT},MSVC>,/clang:-ffp-contract=off,-ffp-contract=off>
        # Explicit, because the per-config defaults cannot be trusted: this
        # tree's caches carry CMAKE_C_FLAGS_RELEASE="" and CMAKE_CXX_FLAGS_RELEASE="",
        # and clang-cl with no /O flag compiles at /Od -- a "Release" runtime
        # was unoptimised without a word. Duplicating a default -O2 is harmless.
        # Every configuration but Debug, including none: a port configured
        # without CMAKE_BUILD_TYPE matched neither name and compiled the whole
        # runtime -- the FIFO walker, the draw engine, the MFC -- at -O0.
        $<$<NOT:$<CONFIG:Debug>>:${PS3RECOMP_OPT_FLAG}>
    )
    # SSE4.1 (any x86-64 since ~2008): pshufb/blendv for spu_shufb, the
    # hottest SPU op in lifted audio code. Probed rather than assumed: recent
    # Apple clang rejects the flag outright for arm64 ("unsupported option
    # '-msse4.1'"), which stops the whole runtime from building there.
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-msse4.1 PS3RECOMP_HAS_MSSE41)
    if(PS3RECOMP_HAS_MSSE41)
        # Lifted consumers compile the same static-inline SPU helpers. Export
        # the runtime's existing ISA requirement so their fast path agrees.
        target_compile_options(ps3recomp_runtime PUBLIC -msse4.1)
    endif()
elseif(MSVC)
    # Real cl.exe -- clang-cl reports its id as Clang and is handled above.
    #
    # This does not currently build. The runtime and the lifted output use GCC/
    # Clang builtins that cl has no equivalent for, and the failure lands as a
    # wall of errors in whichever file the build happens to reach first rather
    # than as anything that names the cause. Say it here instead.
    message(WARNING
        "Building with cl.exe (MSVC). This is not currently supported.\n"
        "  The runtime uses __atomic_* builtins, __int128 and __builtin_bswap*, "
        "and the lifted output uses __int128 for the PPC 64x64 multiplies. "
        "cl has none of them.\n"
        "  Use clang-cl instead -- it consumes the MSVC headers and libraries, "
        "so nothing else about a Visual Studio build changes:\n"
        "    cmake -S . -B build -G Ninja "
        "-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl\n"
        "  See docs/BUILDING.md. Porting the intrinsics to cl is a welcome "
        "contribution; nothing else stands in the way.")
    target_compile_options(ps3recomp_runtime PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4996
        /Zc:__cplusplus
        /experimental:c11atomics   # Enable C11 <stdatomic.h> support
    )
    target_compile_definitions(ps3recomp_runtime PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

if(NOT MSVC AND NOT WIN32)
    # dladdr/Dl_info (runtime/platform/win32_backtrace.h) are GNU extensions
    # that glibc hides unless _GNU_SOURCE is set. g++ defines it implicitly,
    # so the C++ boot scaffold never noticed; a C file including the same
    # shim does not get it for free.
    target_compile_definitions(ps3recomp_runtime PRIVATE _GNU_SOURCE)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PS3RECOMP_BUILD_TESTS)
    include(CTest)
    # sync_stress is written directly against the Win32 threading API
    # (WaitForMultipleObjects, joinable HANDLEs, Interlocked*, CRITICAL_SECTION).
    # Off Windows all of that comes from runtime/platform/win32_compat.h, so
    # the suite runs everywhere -- and is the arm64 memory-model regression
    # net for the lv2 sync primitives.
    add_subdirectory(tests/sync_stress)
    # Syscall 14's address/flags classification as a plain unit test: the
    # check is pure, so the test links one runtime translation unit
    # (lv2_lock_line.c) and nothing else.
    add_subdirectory(tests/syscalls)
    # PS3_LOG_FIRST, the saturating counter behind the "first n times" log lines.
    add_executable(test_log_first tests/test_log_first.c)
    target_include_directories(test_log_first PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME log_first COMMAND test_log_first)

    if(NOT WIN32)
        # The vector single-precision ops against the exact per-lane references.
        add_executable(test_spu_float_exact
            runtime/spu/tests/test_spu_float_exact.c)
        target_include_directories(test_spu_float_exact PRIVATE include runtime/spu)
        target_compile_options(test_spu_float_exact PRIVATE -UNDEBUG)
        target_link_libraries(test_spu_float_exact PRIVATE m)
        add_test(NAME spu_float_exact COMMAND test_spu_float_exact)
        set_tests_properties(spu_float_exact PROPERTIES TIMEOUT 300)
        # The vector forms against the scalar per-lane forms, directly.
        add_executable(test_spu_float_vector_scalar
            runtime/spu/tests/test_spu_float_vector_scalar.c)
        target_include_directories(test_spu_float_vector_scalar PRIVATE include runtime/spu)
        target_compile_options(test_spu_float_vector_scalar PRIVATE -UNDEBUG)
        target_link_libraries(test_spu_float_vector_scalar PRIVATE m)
        add_test(NAME spu_float_vector_scalar COMMAND test_spu_float_vector_scalar 4)
        set_tests_properties(spu_float_vector_scalar PROPERTIES TIMEOUT 300)
        # On x86 both take the ISA the runtime and lifted code are built for, so the SSE4.1
        # vector form is the one under test; no contraction, as in the runtime.
        if(PS3RECOMP_HAS_MSSE41)
            target_compile_options(test_spu_float_exact PRIVATE -msse4.1 -ffp-contract=off)
            target_compile_options(test_spu_float_vector_scalar PRIVATE -msse4.1 -ffp-contract=off)
        endif()

        # The interpreter's op/immediate decode at the opcode-width and
        # immediate-form boundaries, against ISA-layout encodings.
        add_executable(test_spu_decode
            runtime/spu/tests/test_spu_decode.c)
        target_include_directories(test_spu_decode PRIVATE include runtime/spu)
        target_compile_options(test_spu_decode PRIVATE -UNDEBUG)
        target_link_libraries(test_spu_decode PRIVATE m)
        add_test(NAME spu_decode COMMAND test_spu_decode)
        set_tests_properties(spu_decode PROPERTIES TIMEOUT 30)

        # fi and cflts (NEON on ARM64) against their scalar references, in the
        # default FPCR and under round-toward-zero plus flush-to-zero.
        add_executable(test_spu_fi_cflts_neon
            runtime/spu/tests/test_spu_fi_cflts_neon.c)
        target_include_directories(test_spu_fi_cflts_neon PRIVATE include runtime/spu)
        target_compile_options(test_spu_fi_cflts_neon PRIVATE -UNDEBUG)
        target_link_libraries(test_spu_fi_cflts_neon PRIVATE m)
        add_test(NAME spu_fi_cflts_neon COMMAND test_spu_fi_cflts_neon)
        set_tests_properties(spu_fi_cflts_neon PROPERTIES TIMEOUT 300)

        add_executable(test_spu_lockline_wait
            runtime/spu/tests/test_spu_lockline_wait.c)
        target_include_directories(test_spu_lockline_wait PRIVATE include)
        target_compile_options(test_spu_lockline_wait PRIVATE -UNDEBUG)
        target_link_libraries(test_spu_lockline_wait PRIVATE Threads::Threads)
        add_test(NAME spu_lockline_wait COMMAND test_spu_lockline_wait)
        set_tests_properties(spu_lockline_wait PROPERTIES TIMEOUT 15)

    endif()

    # vmx_fmaf (fused multiply-add without a libm call per lane) against fmaf.
    # Without -mfma it takes the double-rounding path; with it, the builtin.
    add_executable(test_vmx_fmaf tests/test_vmx_fmaf.c)
    target_compile_options(test_vmx_fmaf PRIVATE -UNDEBUG)
    if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(test_vmx_fmaf PRIVATE -mno-fma)
    endif()
    if(NOT WIN32)
        target_link_libraries(test_vmx_fmaf PRIVATE m)
    endif()
    add_test(NAME vmx_fmaf COMMAND test_vmx_fmaf)
    set_tests_properties(vmx_fmaf PROPERTIES TIMEOUT 120)

    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME spinlock_word_width
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spinlock_word_width.py")
        add_test(NAME spu_sdcrz
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_sdcrz.py")
    endif()
    if(NOT WIN32)
        find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME spu_fnms_zero_extended
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spu_fnms_zero_extended.py")
        endif()
    endif()
    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME loaded_image_memory
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_loaded_image_memory.py")
        add_test(NAME loader_memory_domain
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_loader_memory_domain.py")
        add_test(NAME user_memory_domain
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_user_memory_domain.py")
    endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ps3recomp_runtime
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/ps3emu
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "ps3recomp runtime v${PROJECT_VERSION}")
message(STATUS "  C compiler:   ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "")

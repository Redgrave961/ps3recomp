cmake_minimum_required(VERSION 3.20)
project(ps3recomp_runtime
    VERSION 0.19.0
    LANGUAGES C CXX
    DESCRIPTION "ps3recomp - Static recompilation runtime for PS3 games"
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS3RECOMP_BUILD_TESTS "Build unit tests" OFF)
set(PS3RECOMP_OPT_FLAG "-O2" CACHE STRING "Optimisation flag for every non-Debug configuration of the runtime and the lifted SPU code")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_RELEASE}" MATCHES "march|/arch")
    add_compile_options($<IF:$<STREQUAL:${CMAKE_C_COMPILER_FRONTEND_VARIANT},MSVC>,/arch:AVX2,-march=x86-64-v3>)
endif()

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
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

file(GLOB_RECURSE RUNTIME_SOURCES
    "runtime/*.c"
    "runtime/*.cpp"
)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/")

add_library(ps3recomp_runtime STATIC
    ${HLE_SOURCES}
    ${RUNTIME_SOURCES}
)

target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

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
        synchronization
        dxgi
    )
    if(PS3RECOMP_PAD_SDL)
        target_compile_definitions(ps3recomp_runtime PRIVATE PS3RECOMP_PAD_USE_SDL)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(ps3recomp_runtime PRIVATE Threads::Threads)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ps3recomp_runtime PRIVATE
        $<IF:$<STREQUAL:${CMAKE_C_COMPILER_FRONTEND_VARIANT},MSVC>,/W4,-Wall$<SEMICOLON>-Wextra>
        -Wno-unused-parameter
        -fno-strict-aliasing
        $<$<STREQUAL:${CMAKE_C_COMPILER_FRONTEND_VARIANT},MSVC>:/Zc:tlsGuards->
        $<IF:$<STREQUAL:${CMAKE_C_COMPILER_FRONTEND_VARIANT},MSVC>,/clang:-ffp-contract=off,-ffp-contract=off>
        $<$<NOT:$<CONFIG:Debug>>:${PS3RECOMP_OPT_FLAG}>
    )
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-msse4.1 PS3RECOMP_HAS_MSSE41)
    if(PS3RECOMP_HAS_MSSE41)
        target_compile_options(ps3recomp_runtime PUBLIC -msse4.1)
    endif()
elseif(MSVC)
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
        /experimental:c11atomics
    )
    target_compile_definitions(ps3recomp_runtime PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

if(NOT MSVC AND NOT WIN32)
    target_compile_definitions(ps3recomp_runtime PRIVATE _GNU_SOURCE)
endif()

if(PS3RECOMP_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests/sync_stress)
    add_subdirectory(tests/syscalls)
    add_executable(test_log_first tests/test_log_first.c)
    target_include_directories(test_log_first PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME log_first COMMAND test_log_first)

    if(NOT WIN32)
        add_executable(test_spu_decode
            runtime/spu/tests/test_spu_decode.c
            runtime/platform/ps3_sync.c
            runtime/spu/tests/spu_fixture_ppu.c)
        target_include_directories(test_spu_decode PRIVATE include runtime/spu)
        target_compile_options(test_spu_decode PRIVATE -UNDEBUG)
        target_link_libraries(test_spu_decode PRIVATE m)
        add_test(NAME spu_decode COMMAND test_spu_decode)
        set_tests_properties(spu_decode PROPERTIES TIMEOUT 30)

        add_executable(test_spu_fi_cflts_neon
            runtime/spu/tests/test_spu_fi_cflts_neon.c)
        target_include_directories(test_spu_fi_cflts_neon PRIVATE include runtime/spu)
        target_compile_options(test_spu_fi_cflts_neon PRIVATE -UNDEBUG)
        target_link_libraries(test_spu_fi_cflts_neon PRIVATE m)
        add_test(NAME spu_fi_cflts_neon COMMAND test_spu_fi_cflts_neon)
        set_tests_properties(spu_fi_cflts_neon PROPERTIES TIMEOUT 300)

        add_executable(test_spu_lockline_wait
            runtime/spu/tests/test_spu_lockline_wait.c
            runtime/platform/ps3_sync.c
            runtime/spu/tests/spu_fixture_ppu.c)
        target_include_directories(test_spu_lockline_wait PRIVATE include)
        target_compile_options(test_spu_lockline_wait PRIVATE -UNDEBUG)
        target_link_libraries(test_spu_lockline_wait PRIVATE Threads::Threads)
        add_test(NAME spu_lockline_wait COMMAND test_spu_lockline_wait)
        set_tests_properties(spu_lockline_wait PROPERTIES TIMEOUT 15)

    endif()

    add_executable(test_spurs
        runtime/spu/tests/test_spurs.c
        runtime/spu/spu_channels.c
        runtime/spu/spu_drain.c
        runtime/spu/spu_lockstep.c
        runtime/spu/spu_coherency.c
        runtime/spu/spu_workload.c
        runtime/spu/spu_interp.c
        runtime/platform/win32_compat.c
        runtime/platform/ps3_sync.c
        runtime/spu/tests/spu_fixture_ppu.c)
    target_include_directories(test_spurs PRIVATE include runtime/spu runtime/platform)
    target_compile_options(test_spurs PRIVATE -UNDEBUG)
    if(WIN32)
        target_compile_definitions(test_spurs PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(test_spurs PRIVATE synchronization)
    else()
        target_link_libraries(test_spurs PRIVATE Threads::Threads m)
    endif()
    foreach(mode kernel idle taskset jobchain)
        add_test(NAME spurs_${mode} COMMAND test_spurs ${mode})
        set_tests_properties(spurs_${mode} PROPERTIES TIMEOUT 60)
    endforeach()

    add_executable(test_spu_wake_stress
        runtime/spu/tests/test_spu_wake_stress.c
        runtime/spu/spu_channels.c
        runtime/spu/spu_drain.c
        runtime/spu/spu_lockstep.c
        runtime/spu/spu_coherency.c
        runtime/spu/spu_workload.c
        runtime/spu/spu_interp.c
        runtime/platform/win32_compat.c
        runtime/platform/ps3_sync.c
        runtime/spu/tests/spu_fixture_ppu.c)
    target_include_directories(test_spu_wake_stress PRIVATE include runtime/spu runtime/platform)
    target_compile_options(test_spu_wake_stress PRIVATE -UNDEBUG)
    if(WIN32)
        target_compile_definitions(test_spu_wake_stress PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(test_spu_wake_stress PRIVATE synchronization)
    else()
        target_link_libraries(test_spu_wake_stress PRIVATE Threads::Threads m)
    endif()
    add_test(NAME spu_wake_stress COMMAND test_spu_wake_stress)
    set_tests_properties(spu_wake_stress PROPERTIES TIMEOUT 180)

    add_executable(test_spu_channel_semantics
        runtime/spu/tests/test_spu_channel_semantics.c
        runtime/spu/spu_drain.c
        runtime/spu/spu_lockstep.c
        runtime/spu/spu_coherency.c
        runtime/spu/spu_workload.c
        runtime/spu/spu_interp.c
        runtime/platform/win32_compat.c
        runtime/platform/ps3_sync.c
        runtime/spu/tests/spu_fixture_ppu.c)
    target_include_directories(test_spu_channel_semantics PRIVATE include runtime/spu runtime/platform)
    target_compile_options(test_spu_channel_semantics PRIVATE -UNDEBUG)
    if(WIN32)
        target_compile_definitions(test_spu_channel_semantics PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(test_spu_channel_semantics PRIVATE synchronization)
    else()
        target_link_libraries(test_spu_channel_semantics PRIVATE Threads::Threads m)
    endif()
    add_test(NAME spu_channel_semantics COMMAND test_spu_channel_semantics)
    set_tests_properties(spu_channel_semantics PROPERTIES TIMEOUT 60)

    add_executable(test_vmx_fmaf tests/test_vmx_fmaf.c)
    target_compile_options(test_vmx_fmaf PRIVATE -UNDEBUG)
    if(NOT WIN32)
        target_link_libraries(test_vmx_fmaf PRIVATE m)
    endif()
    add_test(NAME vmx_fmaf COMMAND test_vmx_fmaf)
    set_tests_properties(vmx_fmaf PROPERTIES TIMEOUT 120)

    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME spu_sdcrz
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_sdcrz.py")
        if(NOT WIN32)
            add_test(NAME spu_fnms_zero_extended
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spu_fnms_zero_extended.py")
        endif()
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

install(TARGETS ps3recomp_runtime
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/ps3emu
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

message(STATUS "")
message(STATUS "ps3recomp runtime v${PROJECT_VERSION}")
message(STATUS "  C compiler:   ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "")

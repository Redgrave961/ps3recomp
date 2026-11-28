cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(syscall_tests LANGUAGES C)
    enable_testing()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(test_lock_line
    test_lock_line.c
    "${PS3RECOMP_ROOT}/runtime/syscalls/lv2_lock_line.c"
)

target_include_directories(test_lock_line PRIVATE
    "${PS3RECOMP_ROOT}/runtime/syscalls"
    "${PS3RECOMP_ROOT}/runtime"
    "${PS3RECOMP_ROOT}/include"
)

target_compile_options(test_lock_line PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_test(NAME lock_line COMMAND test_lock_line)
set_tests_properties(lock_line PROPERTIES
    LABELS "runtime;syscalls"
    TIMEOUT 30
)

add_executable(test_sys_fs
    test_sys_fs.c
    "${PS3RECOMP_ROOT}/runtime/syscalls/sys_fs.c"
    "${PS3RECOMP_ROOT}/runtime/platform/ps3_dev_flash.c"
    "${PS3RECOMP_ROOT}/runtime/memory/vm_track.c"
    "${PS3RECOMP_ROOT}/libs/filesystem/ps3_vfs.c"
)
target_include_directories(test_sys_fs PRIVATE
    "${PS3RECOMP_ROOT}/runtime/syscalls"
    "${PS3RECOMP_ROOT}/runtime"
    "${PS3RECOMP_ROOT}/include"
    "${PS3RECOMP_ROOT}/libs/filesystem"
)
target_compile_options(test_sys_fs PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_sys_fs PRIVATE Threads::Threads)
endif()
add_test(NAME sys_fs COMMAND test_sys_fs)
set_tests_properties(sys_fs PROPERTIES
    LABELS "runtime;syscalls"
    TIMEOUT 60
)

add_executable(test_ppu_thread_join test_ppu_thread_join.c
    "${PS3RECOMP_ROOT}/runtime/platform/ps3_sync.c"
)
target_include_directories(test_ppu_thread_join PRIVATE
    "${PS3RECOMP_ROOT}/runtime/syscalls"
    "${PS3RECOMP_ROOT}/runtime"
    "${PS3RECOMP_ROOT}/include"
)
target_compile_options(test_ppu_thread_join PRIVATE -Wall -Wextra -Wno-unused-parameter -UNDEBUG)
if(WIN32)
    target_sources(test_ppu_thread_join PRIVATE "${PS3RECOMP_ROOT}/runtime/memory/vm_track.c")
    target_link_libraries(test_ppu_thread_join PRIVATE synchronization)
else()
    target_sources(test_ppu_thread_join PRIVATE "${PS3RECOMP_ROOT}/runtime/platform/win32_compat.c")
    find_package(Threads REQUIRED)
    target_link_libraries(test_ppu_thread_join PRIVATE Threads::Threads)
endif()
add_test(NAME ppu_thread_join COMMAND test_ppu_thread_join)
set_tests_properties(ppu_thread_join PROPERTIES
    LABELS "runtime;syscalls"
    TIMEOUT 120
)

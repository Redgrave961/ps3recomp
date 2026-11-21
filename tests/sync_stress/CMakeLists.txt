cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(sync_stress LANGUAGES C)
    enable_testing()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if(NOT DEFINED PS3RECOMP_TREE_ROOT)
    set(PS3RECOMP_TREE_ROOT "${PS3RECOMP_ROOT}")
endif()

if(NOT DEFINED RUNTIME_SYNC_DIR)
    set(RUNTIME_SYNC_DIR "${PS3RECOMP_TREE_ROOT}/runtime/syscalls")
endif()

add_executable(sync_stress
    main.c
    test_shims.c
    "${RUNTIME_SYNC_DIR}/sys_mutex.c"
    "${RUNTIME_SYNC_DIR}/sys_cond.c"
    "${RUNTIME_SYNC_DIR}/sys_semaphore.c"
    "${RUNTIME_SYNC_DIR}/sys_event.c"
    "${RUNTIME_SYNC_DIR}/sys_rwlock.c"
)
if(NOT WIN32)
    target_sources(sync_stress PRIVATE
        "${PS3RECOMP_TREE_ROOT}/runtime/platform/win32_compat.c")
endif()

target_include_directories(sync_stress PRIVATE
    "${RUNTIME_SYNC_DIR}"
    "${PS3RECOMP_TREE_ROOT}/runtime"
    "${PS3RECOMP_TREE_ROOT}/include"
)

if(MSVC)
    target_compile_options(sync_stress PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /experimental:c11atomics
    )
    target_compile_definitions(sync_stress PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(sync_stress PRIVATE -Wall -Wextra -Wno-unused-parameter)
    find_package(Threads REQUIRED)
    target_link_libraries(sync_stress PRIVATE Threads::Threads)
endif()

add_test(NAME sync_stress COMMAND sync_stress)
set_tests_properties(sync_stress PROPERTIES
    LABELS "stress;runtime;sync"
    TIMEOUT 120
)
foreach(watch 1 2)
    add_test(NAME sync_stress_mutex_watch${watch} COMMAND sync_stress)
    set_tests_properties(sync_stress_mutex_watch${watch} PROPERTIES
        LABELS "stress;runtime;sync"
        TIMEOUT 120
        ENVIRONMENT "MUTEX_WATCH=${watch}"
    )
endforeach()

add_executable(test_sync_semantics
    test_sync_semantics.c test_shims.c
    "${RUNTIME_SYNC_DIR}/sys_mutex.c"
    "${RUNTIME_SYNC_DIR}/sys_cond.c"
    "${RUNTIME_SYNC_DIR}/sys_semaphore.c"
    "${RUNTIME_SYNC_DIR}/sys_rwlock.c")
if(NOT WIN32)
    target_sources(test_sync_semantics PRIVATE
        "${PS3RECOMP_TREE_ROOT}/runtime/platform/win32_compat.c")
    find_package(Threads REQUIRED)
    target_link_libraries(test_sync_semantics PRIVATE Threads::Threads)
else()
    target_link_libraries(test_sync_semantics PRIVATE winmm)
endif()
target_include_directories(test_sync_semantics PRIVATE
    "${PS3RECOMP_TREE_ROOT}" "${PS3RECOMP_TREE_ROOT}/include"
    "${RUNTIME_SYNC_DIR}" "${PS3RECOMP_TREE_ROOT}/runtime")
if(MSVC)
    target_compile_definitions(test_sync_semantics PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_sync_semantics PRIVATE /experimental:c11atomics)
else()
    target_compile_options(test_sync_semantics PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
add_test(NAME sync_semantics COMMAND test_sync_semantics)
set_tests_properties(sync_semantics PROPERTIES LABELS "runtime;sync" TIMEOUT 60)

add_executable(test_mutex_destroy
    test_mutex_destroy.c test_shims.c
    "${RUNTIME_SYNC_DIR}/sys_mutex.c"
    "${RUNTIME_SYNC_DIR}/sys_cond.c")
if(NOT WIN32)
    target_sources(test_mutex_destroy PRIVATE
        "${PS3RECOMP_TREE_ROOT}/runtime/platform/win32_compat.c")
    find_package(Threads REQUIRED)
    target_link_libraries(test_mutex_destroy PRIVATE Threads::Threads)
endif()
target_include_directories(test_mutex_destroy PRIVATE
    "${PS3RECOMP_TREE_ROOT}" "${PS3RECOMP_TREE_ROOT}/include"
    "${RUNTIME_SYNC_DIR}" "${PS3RECOMP_TREE_ROOT}/runtime")
if(MSVC)
    target_compile_definitions(test_mutex_destroy PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME mutex_destroy COMMAND test_mutex_destroy)
set_tests_properties(mutex_destroy PROPERTIES LABELS "runtime;sync" TIMEOUT 20)

add_executable(test_cond_semantics
    test_cond_semantics.c test_shims.c
    "${RUNTIME_SYNC_DIR}/sys_mutex.c"
    "${RUNTIME_SYNC_DIR}/sys_cond.c")
if(NOT WIN32)
    target_sources(test_cond_semantics PRIVATE
        "${PS3RECOMP_TREE_ROOT}/runtime/platform/win32_compat.c")
    find_package(Threads REQUIRED)
    target_link_libraries(test_cond_semantics PRIVATE Threads::Threads)
endif()
target_include_directories(test_cond_semantics PRIVATE
    "${PS3RECOMP_TREE_ROOT}" "${PS3RECOMP_TREE_ROOT}/include")
if(MSVC)
    target_compile_definitions(test_cond_semantics PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
foreach(mode owner recursive spurious)
    add_test(NAME cond_${mode} COMMAND test_cond_semantics ${mode})
    set_tests_properties(cond_${mode} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(test_event_queue_destroy
    test_event_queue_destroy.c test_shims.c
    "${RUNTIME_SYNC_DIR}/sys_event.c")
if(NOT WIN32)
    target_sources(test_event_queue_destroy PRIVATE
        "${PS3RECOMP_TREE_ROOT}/runtime/platform/win32_compat.c")
    find_package(Threads REQUIRED)
    target_link_libraries(test_event_queue_destroy PRIVATE Threads::Threads)
endif()
target_include_directories(test_event_queue_destroy PRIVATE
    "${PS3RECOMP_TREE_ROOT}" "${PS3RECOMP_TREE_ROOT}/include"
    "${RUNTIME_SYNC_DIR}" "${PS3RECOMP_TREE_ROOT}/runtime")
if(MSVC)
    target_compile_definitions(test_event_queue_destroy PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_event_queue_destroy PRIVATE /experimental:c11atomics)
else()
    target_compile_options(test_event_queue_destroy PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
add_test(NAME event_queue_destroy COMMAND test_event_queue_destroy)
set_tests_properties(event_queue_destroy PROPERTIES LABELS "runtime;sync" TIMEOUT 60)

add_executable(test_brief_wait test_brief_wait.c)
if(NOT WIN32)
    target_sources(test_brief_wait PRIVATE
        "${PS3RECOMP_TREE_ROOT}/runtime/platform/win32_compat.c")
    find_package(Threads REQUIRED)
    target_link_libraries(test_brief_wait PRIVATE Threads::Threads)
endif()
target_include_directories(test_brief_wait PRIVATE
    "${PS3RECOMP_TREE_ROOT}" "${PS3RECOMP_TREE_ROOT}/include")
add_test(NAME brief_wait COMMAND test_brief_wait)
set_tests_properties(brief_wait PROPERTIES LABELS "runtime;sync" TIMEOUT 60)

foreach(t sync_stress test_sync_semantics test_mutex_destroy test_event_queue_destroy test_cond_semantics test_brief_wait)
    if(TARGET ${t})
        target_sources(${t} PRIVATE "${PS3RECOMP_TREE_ROOT}/runtime/platform/ps3_sync.c")
        if(WIN32)
            target_link_libraries(${t} PRIVATE synchronization)
        endif()
    endif()
endforeach()

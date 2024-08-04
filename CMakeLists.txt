cmake_minimum_required(VERSION 3.14)
project(MicroSched C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # needed for ucontext on some platforms

# Warnings-as-errors
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Werror
)

# Thread support
find_package(Threads REQUIRED)

# -------------------------------------------------------------------------
# Library target
# -------------------------------------------------------------------------
add_library(micro_sched STATIC
    src/scheduler.c
    src/mutex.c
    src/semaphore.c
    src/mem_pool.c
    src/io_sim.c
)

target_include_directories(micro_sched
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(micro_sched
    PUBLIC Threads::Threads
)

# -------------------------------------------------------------------------
# Demo executable
# -------------------------------------------------------------------------
add_executable(micro_sched_demo src/demo.c)
target_link_libraries(micro_sched_demo PRIVATE micro_sched)

# -------------------------------------------------------------------------
# Tests
# -------------------------------------------------------------------------
enable_testing()

add_executable(test_scheduler tests/test_scheduler.c)
target_link_libraries(test_scheduler PRIVATE micro_sched)

add_test(NAME test_scheduler COMMAND test_scheduler)
set_tests_properties(test_scheduler PROPERTIES TIMEOUT 10)

cmake_minimum_required(VERSION 3.20)
project(MemoryGuard LANGUAGES C CXX)

# Specify C standard (C11)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Specify C++ standard (C++17 for LLVM plugins)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable compiler warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find system thread support for runtime thread-safety
find_package(Threads REQUIRED)

# Build MemoryGuard runtime as a static library
add_library(memoryguard STATIC
    runtime/src/memoryguard.c
)

# Export runtime include directory
target_include_directories(memoryguard PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/include>
    $<INSTALL_INTERFACE:include>
)

# Link threads to the runtime library
target_link_libraries(memoryguard PUBLIC Threads::Threads)

# Build the LLVM compiler plugin
add_subdirectory(compiler)

# Enable testing support for CTest
enable_testing()

# Build the allocation registry unit test executable
add_executable(test_allocation
    runtime/tests/test_allocation.c
)

# Link the test executable with the MemoryGuard runtime library
target_link_libraries(test_allocation PRIVATE memoryguard)

# Register the test case with CTest
add_test(NAME test_allocation COMMAND test_allocation)

# Build the registry performance benchmark executable
add_executable(benchmark_registry
    runtime/tests/benchmark_registry.c
)
target_link_libraries(benchmark_registry PRIVATE memoryguard)

# Find clang, opt, and llvm-link to build and run the end-to-end compiler instrumentation test
find_program(CLANG_TOOL NAMES clang-18 clang PATHS ${LLVM_TOOLS_BINARY_DIR})
find_program(OPT_TOOL NAMES opt-18 opt PATHS ${LLVM_TOOLS_BINARY_DIR})
find_program(LLVM_LINK_TOOL NAMES llvm-link-18 llvm-link PATHS ${LLVM_TOOLS_BINARY_DIR})

if(CLANG_TOOL AND OPT_TOOL AND LLVM_LINK_TOOL)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_instrumentation_bin
        COMMAND ${CLANG_TOOL} -O0 -Xclang -disable-O0-optnone -emit-llvm -S
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_instrumentation.c
                -I ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include
                -o ${CMAKE_CURRENT_BINARY_DIR}/test_instrumentation_c.ll
        COMMAND ${LLVM_LINK_TOOL}
                ${CMAKE_CURRENT_BINARY_DIR}/test_instrumentation_c.ll
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_aggregates.ll
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_atomic_volatile.ll
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_advanced_regression.ll
                -S -o ${CMAKE_CURRENT_BINARY_DIR}/test_instrumentation.ll
        COMMAND ${OPT_TOOL} -load-pass-plugin=$<TARGET_FILE:MemoryGuardPass>
                -passes=memoryguard
                -S ${CMAKE_CURRENT_BINARY_DIR}/test_instrumentation.ll
                -o ${CMAKE_CURRENT_BINARY_DIR}/test_instrumentation_inst.ll
        COMMAND ${CLANG_TOOL} ${CMAKE_CURRENT_BINARY_DIR}/test_instrumentation_inst.ll
                $<TARGET_FILE:memoryguard>
                -pthread
                -o ${CMAKE_CURRENT_BINARY_DIR}/test_instrumentation_bin
        DEPENDS examples/test_instrumentation.c examples/test_aggregates.ll examples/test_atomic_volatile.ll examples/test_advanced_regression.ll MemoryGuardPass memoryguard
        COMMENT "Compiling, instrumenting, and linking end-to-end integration test"
    )

    add_custom_target(build_test_instrumentation ALL
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/test_instrumentation_bin
    )

    add_test(NAME test_instrumentation COMMAND ${CMAKE_CURRENT_BINARY_DIR}/test_instrumentation_bin)
endif()


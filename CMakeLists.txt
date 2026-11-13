cmake_minimum_required(VERSION 3.20)
project(MemoryGuard C)

# Specify C standard (C11)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Enable compiler warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Build MemoryGuard runtime as a static library
add_library(memoryguard STATIC
    runtime/src/memoryguard.c
)

# Export runtime include directory
target_include_directories(memoryguard PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/include>
    $<INSTALL_INTERFACE:include>
)

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

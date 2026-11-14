# Find the installed LLVM CMake package
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Include LLVM headers (marked as SYSTEM to suppress header warnings) and definitions
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS})

# Add LLVM CMake modules path if available
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")

# Build the MemoryGuard pass plugin as a loadable module
add_library(MemoryGuardPass MODULE
    src/MemoryGuardPass.cpp
)

# Configure target properties for LLVM plugin compatibility
set_target_properties(MemoryGuardPass PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    PREFIX ""
    SUFFIX ".so"
)

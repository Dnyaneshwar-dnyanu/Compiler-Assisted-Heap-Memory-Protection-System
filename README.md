# MemoryGuard: Compiler-Assisted Heap Memory Protection System

MemoryGuard is an experimental compiler-assisted heap protection prototype for C/C++ applications. It combines an LLVM 18 New Pass Manager function plugin with a dedicated C runtime to automatically detect, validate, and enforce heap memory boundaries at runtime.

By analyzing pointer provenance and selectively instrumenting heap-derived memory operations, MemoryGuard catches spatial violations (out-of-bounds reads and writes) and temporal violations (use-after-free) before memory corruption can compromise application safety.

---

## Architecture Overview

MemoryGuard operates in two primary phases:

1. **Compile-Time Analysis and Instrumentation (LLVM 18 Pass Plugin)**
   - Traverses function control-flow graphs and instruction streams.
   - Detects heap allocation call sites (`malloc`, `calloc`, `realloc`).
   - Performs recursive pointer-origin analysis (`HEAP_CANDIDATE`, `NON_HEAP`, `UNKNOWN`).
   - Inserts call instructions to `mg_check_access()` immediately preceding qualified `LoadInst` and `StoreInst` operations.
   - Preserves dominance information and applies metadata tags to guarantee pass idempotency.

2. **Run-Time Boundary Validation (C Runtime Library)**
   - Maintains an internal allocation registry tracking active heap buffers and bounds.
   - Supports native allocations (`mg_malloc`, `mg_free`) and external heap tracking (`mg_register_external`, `mg_unregister_external`, `mg_realloc_external`).
   - Validates memory operations with interior pointer resolution and overflow-safe arithmetic.
   - Emits structured error diagnostics to `stderr` and terminates unsafe executions via `abort()`.

```
                    +-----------------------------+
                    |      Source Code (C/C++)     |
                    +-----------------------------+
                                   |
                                   v  (clang -emit-llvm)
                    +-----------------------------+
                    |       LLVM Bitcode / IR     |
                    +-----------------------------+
                                   |
                                   v  (opt -passes=memoryguard)
               +---------------------------------------+
               |        MemoryGuard LLVM Pass          |
               |  - Heap Allocation Detection         |
               |  - Pointer-Origin Analysis            |
               |  - Size Calculation (DataLayout)      |
               |  - Instrumentation Placement          |
               +---------------------------------------+
                                   |
                                   v
                    +-----------------------------+
                    |    Instrumented LLVM IR     |
                    | (call @mg_check_access ...) |
                    +-----------------------------+
                                   |
                                   v  (clang + libmemoryguard.a)
                    +-----------------------------+
                    |     Protected Executable    |
                    +-----------------------------+
                                   |
           +-----------------------+-----------------------+
           | (Valid Access)                                | (Violation)
           v                                               v
    Executes Normally                      MemoryGuard Runtime Diagnostic
                                           Violating Pointer & Size logged
                                           abort() terminates execution
```

---

## Implementation Progress

| Step | Component | Status | Description |
| :---: | :--- | :---: | :--- |
| **Step 1** | Runtime Registry | Completed | Implemented core linked-list allocation registry (`mg_malloc`, `mg_free`, `mg_is_tracked`). |
| **Step 2** | Heap Boundary Validation | Completed | Added `mg_is_valid_access()` supporting interior pointers, overflow-safe arithmetic, and zero-size access semantics. |
| **Step 3** | LLVM Pass Plugin | Completed | Minimal LLVM 18 New Pass Manager function plugin registered under the name `memoryguard`. |
| **Step 4** | Load/Store Analysis | Completed | Instruction traversal detecting `LoadInst` and `StoreInst`, computing access size via `DataLayout`. |
| **Step 5** | Allocation Detection | Completed | Inspected calls to standard allocation APIs (`malloc`, `calloc`, `realloc`) with compile-time constant arithmetic. |
| **Step 6** | External Heap Tracking | Completed | Added `mg_register_external()`, `mg_unregister_external()`, and `mg_realloc_external()` for existing buffers. |
| **Step 7** | Pointer-Origin Analysis | Completed | Traced pointer provenance through casts, GEPs, selects, PHIs, and dominating alloca stores. Classified into `HEAP_CANDIDATE`, `NON_HEAP`, and `UNKNOWN`. |
| **Step 8** | Runtime Instrumentation | Completed | Inserted `mg_check_access()` calls before `HEAP_CANDIDATE` loads and stores with idempotency protection and abort verification. |

---

## Project Structure

```
.
├── CMakeLists.txt                 # Top-level build configuration (C11, C++17, CTest)
├── compiler/
│   ├── CMakeLists.txt             # LLVM plugin build configuration
│   └── src/
│       └── MemoryGuardPass.cpp    # LLVM 18 Function Pass implementation
├── runtime/
│   ├── include/
│   │   └── memoryguard.h          # Public runtime C API declarations
│   ├── src/
│   │   └── memoryguard.c          # Allocation registry and boundary validation
│   └── tests/
│       └── test_allocation.c      # Unit test suite (193 assertions)
├── examples/
│   └── test_instrumentation.c     # End-to-end compiler pass integration tests
└── README.md                      # Project documentation
```

---

## Prerequisites & Development Environment

The project is developed and tested on **Ubuntu on WSL2** (Windows Subsystem for Linux) with the following toolchain:

- **Clang / LLVM:** 18.1.3
- **CMake:** 3.28.3 or newer (minimum 3.20)
- **Ninja:** 1.11.1
- **C/C++ Standards:** C11 for the runtime, C++17 for the LLVM pass

---

## Building MemoryGuard

Clone or navigate to the repository directory, then configure and build using CMake and Ninja:

```bash
# Create and navigate to the build directory
mkdir build && cd build

# Configure with Ninja generator
cmake -G Ninja ..

# Compile the runtime library and compiler pass plugin
ninja
```

This generates:
- `build/libmemoryguard.a` — Static C runtime library.
- `build/compiler/MemoryGuardPass.so` — Loadable LLVM New Pass Manager plugin.
- `build/test_allocation` — Runtime unit test executable.

---

## Running the Test Suites

### 1. Runtime Unit Tests (CTest)
MemoryGuard includes an extensive test suite verifying allocation tracking, interior pointer support, boundary checks, external registrations, overflow prevention, and child-process abort behavior:

```bash
# From the build directory
ctest --output-on-failure
# Or run the binary directly:
./test_allocation
```

Expected output:
```
========================================
  MemoryGuard Allocation Registry Tests 
========================================
[RUN]  test_successful_allocation
[PASS] test_successful_allocation
[RUN]  test_allocation_size_tracking
[PASS] test_allocation_size_tracking
[RUN]  test_multiple_simultaneous_allocations
[PASS] test_multiple_simultaneous_allocations
[RUN]  test_successful_deallocation
[PASS] test_successful_deallocation
[RUN]  test_tracking_after_deallocation
[PASS] test_tracking_after_deallocation
[RUN]  test_null_allocation_handling
[PASS] test_null_allocation_handling
[RUN]  test_boundary_validation
[PASS] test_boundary_validation
[RUN]  test_external_allocations
[PASS] test_external_allocations
[RUN]  test_check_access
[PASS] test_check_access
========================================
Summary: 193 assertions, 0 failed
========================================
All tests PASSED!
```

### 2. End-to-End Compiler Integration Test
The standalone integration test validates the complete toolchain: emitting LLVM IR with Clang, instrumenting with the `MemoryGuardPass` plugin via `opt`, linking against the runtime, and executing under isolated child processes:

```bash
# 1. Compile C code to LLVM IR (-O0 without optnone to allow pass execution)
clang-18 -O0 -Xclang -disable-O0-optnone -emit-llvm -S \
  ../examples/test_instrumentation.c -I ../runtime/include -o /tmp/test.ll

# 2. Run the MemoryGuard pass plugin
opt-18 -load-pass-plugin=./compiler/MemoryGuardPass.so \
  -passes=memoryguard -S /tmp/test.ll -o /tmp/test_instrumented.ll

# 3. Compile instrumented IR and link with the runtime
clang-18 /tmp/test_instrumented.ll ./libmemoryguard.a -o /tmp/test_instrumentation_bin

# 4. Execute the verification suite
/tmp/test_instrumentation_bin
```

Output:
```
====================================================
 MemoryGuard Step 8: Instrumentation Validation Test
====================================================
[TEST] Valid heap read/write...
  [PASS] Valid heap accesses completed normally.
[TEST] Out-of-bounds heap write...
  [PASS] Out-of-bounds heap write caught and aborted by MemoryGuard.
[TEST] Out-of-bounds heap read...
  [PASS] Out-of-bounds heap read caught and aborted by MemoryGuard.
[TEST] Access after free (use-after-free)...
  [PASS] Use-after-free write caught and aborted by MemoryGuard.
[TEST] Stack and global accesses...
  [PASS] Stack and global accesses executed without instrumentation.
[TEST] Unknown pointer origins...
  [PASS] Unknown pointer parameter executed without instrumentation.
====================================================
Result: 6 / 6 tests passed
====================================================
```

---

## How It Works: IR Transformation Example

Given a heap allocation access:

```c
int *arr = (int *)malloc(32);
mg_register_external(arr, 32);
arr[8] = 999; /* Out of bounds write (index 8 is at offset 32 bytes) */
```

### Uninstrumented LLVM IR
```llvm
%4 = load ptr, ptr %1, align 8
%5 = getelementptr inbounds i32, ptr %4, i64 8
store i32 999, ptr %5, align 4
```

### Instrumented LLVM IR
```llvm
%4 = load ptr, ptr %1, align 8
%5 = getelementptr inbounds i32, ptr %4, i64 8
call void @mg_check_access(ptr %5, i64 4)
store i32 999, ptr %5, align 4, !memoryguard.checked !0
```

When this store executes, `mg_check_access(%5, 4)` checks `allocation_registry`. It detects that the 4-byte access starting at offset 32 spans `[32, 36)`, which exceeds the 32-byte allocated range `[0, 32)`.

MemoryGuard outputs:
```
MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x5b38d38ba2c0, access_size=4 bytes
Aborted (core dumped)
```

---

### Current Limitations

- Heap allocations must be registered with MemoryGuard before runtime boundary validation.
- Standard `malloc()`, `calloc()`, and `realloc()` calls are not automatically registered or intercepted.
- Standard `free()` calls are not automatically synchronized with the allocation registry.
- External allocations require explicit registration and unregistration.
- `mg_malloc()` and `mg_realloc_external()` allocation recognition in the LLVM pass is planned for the next development step.
- Pointer-origin analysis is intraprocedural and conservatively skips unknown pointers.
- The allocation registry is not thread-safe.
- Atomic and volatile memory operations are not fully supported.
- Runtime boundary validation currently uses a linear linked-list lookup.

MemoryGuard is a prototype for compiler-assisted heap-boundary checking, not a complete memory-safety solution.

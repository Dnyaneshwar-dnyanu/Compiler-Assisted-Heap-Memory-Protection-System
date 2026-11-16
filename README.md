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
| **Step 9-12** | Automatic Interception & Robustness | Completed | Rewrote standard allocators, supported zero-size buffers, thread safety, and relocation. |
| **Step 13** | Volatile & Atomic Accesses | Completed | Safely instrumented volatile accesses while conservatively skipping atomics to preserve memory order. |
| **Step 14** | AVL Interval Tree Optimization | Completed | Replaced O(N) linked list with an AVL interval tree supporting O(log N) interior-pointer searches. |
| **Step 15-16** | Regression & Negative Testing | Completed | Added comprehensive pointer arithmetic, multi-dimensional array, control flow, and negative tests. |
| **Step 17** | Compiler Diagnostics | Completed | Structured per-function and module-level analysis metrics and diagnostics. |
| **Step 18** | Audited Performance Benchmarks | Completed | Monotonic nanosecond timing, compiler-optimization barriers, batching, and empirical slowdown analysis. |
| **Step 19** | Node.js Backend Foundation | Completed | Modular Express backend with health, system diagnostics, validated compile placeholder, and test suite. |
| **Step 20** | LLVM Compiler Integration | Completed | Integrated Clang 18, Opt, and MemoryGuard pass plugin with Express backend for automated compilation, instrumentation, and linking. |

---

## Project Structure

```
.
├── CMakeLists.txt                 # Top-level build configuration (C11, C++17, CTest)
├── backend/                       # Node.js Express backend (Steps 19 & 20)
│   ├── package.json               # Backend dependencies, scripts, and engine specifications
│   ├── .env.example               # Template environment configuration with LLVM toolchain
│   ├── .gitignore                 # Backend-specific ignore rules (workspaces, logs, uploads)
│   ├── src/
│   │   ├── config/                # Centralized configuration loader (dotenv)
│   │   ├── controllers/           # HTTP request handlers (health, system, compile)
│   │   ├── middleware/            # Error handling, 404 handler, request validation
│   │   ├── routes/                # Express router definitions
│   │   ├── services/              # Pure business logic and compiler coordinator
│   │   │   └── compiler/          # LLVM 18 toolchain integration services (Step 20)
│   │   │       ├── compilerConfig.js    # Toolchain path resolution & diagnostics sanitizer
│   │   │       ├── compilerRunner.js    # Safe child_process.spawn with timeout & output caps
│   │   │       ├── workspaceManager.js  # UUID workspace isolation & path-traversal safety
│   │   │       └── compilerService.js   # 3-stage pipeline (Clang IR -> Opt pass -> Linker)
│   │   ├── utils/                 # Structured response formatters and logger
│   │   ├── app.js                 # Express app initialization (Helmet, CORS, Morgan)
│   │   └── server.js              # Server entry point with graceful shutdown
│   └── tests/                     # Automated test suite (node:test + supertest)
│       ├── compile.test.js        # POST /api/compile C & C++ integration tests
│       ├── compilerPipeline.test.js # Toolchain, workspace safety & runner unit tests
│       ├── errorHandler.test.js   # Centralized error handler tests
│       ├── health.test.js         # GET /api/health tests
│       ├── notFound.test.js       # 404 JSON fallback tests
│       └── system.test.js         # GET /api/system/info diagnostic tests
├── compiler/
│   ├── CMakeLists.txt             # LLVM plugin build configuration
│   └── src/
│       └── MemoryGuardPass.cpp    # LLVM 18 Function Pass implementation
├── runtime/
│   ├── include/
│   │   └── memoryguard.h          # Public runtime C API declarations
│   ├── src/
│   │   └── memoryguard.c          # AVL interval tree allocation registry
│   └── tests/
│       └── test_allocation.c      # Unit test suite (54,940 assertions)
├── benchmarks/                    # Audited benchmark suite and automated runner
│   ├── benchmark_suite.c
│   ├── run_benchmarks.py
│   └── results/
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

## Node.js Backend & LLVM Compiler Integration (Steps 19 & 20)

### 1. Purpose & Architecture
The MemoryGuard backend provides a modular REST API built with Node.js and Express. In Step 20, the backend connects directly to the MemoryGuard LLVM compiler pipeline to compile, instrument, and link user-submitted C and C++ programs into memory-safe executables.

The compiler integration follows a strict 3-stage pipeline executed in isolated workspaces:

```
[POST /api/compile]
        │
        ▼ (Stage 1: Frontend Compilation)
  clang-18 / clang++-18 -O0 -Xclang -disable-O0-optnone -emit-llvm -S source.c -o source.ll
        │
        ▼ (Stage 2: LLVM Pass Instrumentation)
  opt-18 -load-pass-plugin=MemoryGuardPass.so -passes=memoryguard -S source.ll -o source_inst.ll
        │
        ▼ (Stage 3: Linking with Runtime)
  clang-18 / clang++-18 source_inst.ll libmemoryguard.a -pthread -lm -o program.bin
        │
        ▼
[Structured Response with Diagnostics & Artifact Availability]
```

### 2. Technologies Used & Toolchain Requirements
- **Runtime**: Node.js (>= 18.0.0, validated on v22.18.0)
- **Framework**: Express.js 4.x with ECMAScript Modules (ESM)
- **Compiler Toolchain**: Clang 18.1.3 and LLVM 18.1.3 (`clang-18`, `clang++-18`, `opt-18`)
- **Plugin Artifacts**: `build/compiler/MemoryGuardPass.so` and `build/libmemoryguard.a`
- **Security**:
  - `child_process.spawn()` with strict argument arrays (zero shell interpolation)
  - Configurable timeouts (`MG_COMPILER_TIMEOUT_MS`) with automatic SIGTERM/SIGKILL escalation
  - Output truncation bounds (`MG_MAX_OUTPUT_BYTES`) to prevent memory exhaustion
  - UUID workspace isolation (`MG_WORKSPACE_ROOT`) with directory traversal protection
  - Host path sanitization (strips server paths from compiler diagnostics)
  - Helmet HTTP security headers and CORS controls

### 3. Setup & Installation
```bash
# Navigate to the backend directory
cd backend

# Install dependencies
npm install
```

### 4. Environment Configuration
Copy the template configuration file:
```bash
cp .env.example .env
```

Available environment variables:
| Variable | Default | Description |
|:---|:---|:---|
| `PORT` | `5000` | Port on which the HTTP server listens |
| `NODE_ENV` | `development` | Environment tier (`development`, `production`, `test`) |
| `CORS_ORIGIN` | `*` | Allowed CORS origins (wildcard `*` or comma-separated URLs) |
| `MAX_REQUEST_SIZE` | `1mb` | Maximum JSON request body size limit |
| `MG_CLANG` | `clang-18` | Clang C compiler executable |
| `MG_CLANGXX` | `clang++-18` | Clang C++ compiler executable |
| `MG_OPT` | `opt-18` | LLVM opt optimizer tool |
| `MG_PASS_PLUGIN` | `../build/compiler/MemoryGuardPass.so` | Path to compiled MemoryGuard LLVM pass plugin |
| `MG_RUNTIME_LIBRARY` | `../build/libmemoryguard.a` | Path to compiled MemoryGuard static runtime library |
| `MG_RUNTIME_INCLUDE` | `../runtime/include` | Path to MemoryGuard runtime include headers |
| `MG_WORKSPACE_ROOT` | `.workspaces` | Directory where compilation workspaces are isolated |
| `MG_COMPILER_TIMEOUT_MS` | `15000` | Process timeout for compiler stages (in milliseconds) |
| `MG_MAX_SOURCE_BYTES` | `524288` | Maximum allowed source code payload (512 KB) |
| `MG_MAX_OUTPUT_BYTES` | `262144` | Maximum compiler output captured before truncation (256 KB) |
| `MG_USE_WSL` | `auto` | Set `true` on Windows to run Linux LLVM 18 tools via WSL |

> [!NOTE]
> Never commit a `.env` file containing secrets or environment-specific values. `.env` is intentionally ignored by Git.

### 5. Running the Backend
```bash
# Start in development mode with auto-reload (nodemon)
npm run dev

# Start in production mode
npm start
```

### 6. Executing Automated Tests
The backend includes 31 automated integration and unit tests covering health monitoring, system diagnostics, request validation, error formatting, route fallback, and real C/C++ LLVM compilation pipelines:
```bash
# Run the complete test suite
npm test
```

### 7. Current API Endpoints

#### `GET /api/health`
Returns service availability, application metadata, timestamp, and process uptime.
- **Status Code**: `200 OK`
- **Response Example**:
```json
{
  "success": true,
  "message": "MemoryGuard Backend is healthy and operational",
  "data": {
    "status": "healthy",
    "application": "MemoryGuard Backend API",
    "version": "1.0.0",
    "timestamp": "2026-11-16T17:50:00.000Z",
    "uptimeSeconds": 42
  }
}
```

#### `GET /api/system/info`
Returns sanitized runtime diagnostics and platform metrics. Does not expose filesystem paths, usernames, or sensitive environment variables.
- **Status Code**: `200 OK`
- **Response Example**:
```json
{
  "success": true,
  "message": "System diagnostics retrieved successfully",
  "data": {
    "nodeVersion": "v22.18.0",
    "platform": "win32",
    "arch": "x64",
    "environment": "development",
    "uptimeSeconds": 42,
    "memoryUsageMB": {
      "rss": 62,
      "heapTotal": 24,
      "heapUsed": 15
    }
  }
}
```

#### `POST /api/compile`
Compiles, instruments, and links C or C++ source code using the LLVM 18 MemoryGuard toolchain.
- **Request Body (C Example)**:
```json
{
  "sourceCode": "#include <stdlib.h>\nint main() {\n    int *p = (int *)malloc(32);\n    p[0] = 42;\n    free(p);\n    return 0;\n}",
  "language": "c"
}
```
- **Success Response (`200 OK`)**:
```json
{
  "success": true,
  "message": "Compilation and MemoryGuard instrumentation succeeded.",
  "data": {
    "jobId": "7f3b891a-4d2c-4e89-9a1b-123456789abc",
    "status": "completed",
    "language": "c",
    "stages": [
      { "name": "FRONTEND_COMPILATION", "status": "completed", "durationMs": 142 },
      { "name": "INSTRUMENTATION", "status": "completed", "durationMs": 85 },
      { "name": "LINKING", "status": "completed", "durationMs": 118 }
    ],
    "diagnostics": {
      "summary": {
        "functionsAnalyzed": ["main"],
        "totalAccessesExamined": 7,
        "heapAccessesIdentified": 2,
        "checksInserted": 2,
        "volatileAccessesInstrumented": 0,
        "nonHeapSkipped": 5,
        "unknownOriginSkipped": 0,
        "atomicOperationsSkipped": 0,
        "unsupportedAddressSpaceSkipped": 0,
        "scalableVectorSkipped": 0,
        "allocatorCallsRewritten": 2,
        "incompatibleAllocatorCallsSkipped": 0
      },
      "raw": "[MemoryGuard][Summary] Module Analysis Summary:\n  Total load and store instructions examined: 7\n  Heap-origin accesses identified: 2\n  Runtime checks inserted: 2\n  Allocator calls detected and successfully rewritten: 2"
    },
    "artifacts": {
      "executableAvailable": true,
      "bitcodeAvailable": true,
      "instrumentedBitcodeAvailable": true
    },
    "metadata": {
      "sourceBytes": 128,
      "totalDurationMs": 352,
      "executed": false
    }
  }
}
```

- **Compilation Syntax Error (`422 Unprocessable Entity`)**:
```json
{
  "success": false,
  "error": {
    "code": "COMPILATION_ERROR",
    "message": "Frontend compilation failed with errors.",
    "stage": "FRONTEND_COMPILATION",
    "jobId": "9a2c3d4e-5f6a-7b8c-9d0e-123456789def",
    "diagnostics": "<workspace>/source.c:3:16: error: expected ';' after expression",
    "stages": [
      { "name": "FRONTEND_COMPILATION", "status": "failed", "durationMs": 82 }
    ]
  }
}
```

> [!IMPORTANT]
> **Program Execution Notice**: In Step 20, `POST /api/compile` performs full compile-time instrumentation and linking, generating a valid executable artifact in the workspace. However, the generated binary is **not executed** in this step. Program execution in an isolated execution sandbox will be implemented in Step 21+.

---

### Current Limitations & Future Roadmap

- **Sandbox Execution**: Generated executables are stored within unique workspaces ready for execution; safe process sandboxing (e.g. Docker, seccomp, or resource cgroups) will be integrated in subsequent phases.
- **Authentication**: Endpoints currently operate without user authentication or persistent database queues.
- **Frontend Integration**: Web UI components will be connected to these backend endpoints in Step 22+.


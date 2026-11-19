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
| **Step 21** | Secure Docker Runtime Execution | Completed | Isolated container runtime execution service (`POST /api/execute`) enforcing strict Docker security hardening, read-only binary mounts, resource quotas, and diagnostic parsing. |
| **Step 22** | Modern React Developer Frontend | Completed | Polished developer UI with Monaco Editor (C/C++), real-time compilation pipeline, analysis metrics dashboard, and Docker execution sandbox monitoring. |
| **Step 23** | End-to-End Integration & Security Verification | Completed | Comprehensive integration suite covering browser-to-container workflows, security regression scenarios, job/workspace isolation, Docker hardening, and empirical performance metrics. |
| **Step 24** | Docker Security Lockdown & Tamper Resistance | Completed | Hardened Docker sandbox orchestration by removing client-controlled `customConfig` from the HTTP API, freezing non-negotiable security flags (`IMMUTABLE_SECURITY`), and adding tamper-resistance regression tests. |
| **Step 25** | Fix realloc() Pointer Invalidation | Completed | Eliminated post-realloc pointer evaluation undefined behavior by preserving the original allocation address as integer (`old_start`) before `realloc()`, adding an internal allocator test seam, and validating zero-size/in-place/relocating realloc contracts under ASan/UBSan/TSan. |
| **Step 26** | Docker-Isolated Compiler Pipeline | Completed | Moved C/C++ compilation, LLVM pass instrumentation, and runtime linking into a dedicated restricted Docker container (`memoryguard-compiler:latest`) with immutable security flags, workspace containment, separate `/src:ro` and `/out:rw` bind mounts, and offline test fallback. |

---

## Project Structure

```
.
├── CMakeLists.txt                 # Top-level build configuration (C11, C++17, CTest)
├── docker/
│   ├── Dockerfile.compiler        # Dedicated container for Clang 18, Opt 18, and LLVM pass plugin (Step 26)
│   ├── Dockerfile.runner          # Minimal root Dockerfile for debian:bookworm-slim runner
│   └── compile_runner.sh          # Container-internal 3-stage compiler orchestrator (Step 26)
├── frontend/                      # React 18 + Vite developer frontend (Step 22)
│   ├── package.json               # Frontend dependencies (React, Monaco, Lucide, Tailwind, Vitest)
│   ├── vite.config.js             # Vite configuration with /api reverse proxy and Vitest setup
│   ├── tailwind.config.js         # Tailwind CSS styling configuration
│   ├── index.html                 # HTML application entry point
│   ├── .env.example               # Configurable VITE_API_BASE_URL
│   ├── src/
│   │   ├── components/            # Modular React UI components
│   │   │   ├── Header/            # Brand header, title, and backend/Docker status indicators
│   │   │   ├── Toolbar/           # Language switcher (C/C++), preset samples, compile & run buttons
│   │   │   ├── Editor/            # Monaco code editor with syntax highlighting and presets
│   │   │   │   ├── CodeEditor.jsx # Monaco integration with test environment fallback
│   │   │   │   └── sampleCodes.js # Preset security test programs (clean, OOB, UAF, arithmetic)
│   │   │   ├── OutputPanel/       # Container output streams, exit metrics, and violation card
│   │   │   ├── DiagnosticsPanel/  # Compiler pipeline stages (Clang, Opt, Linker) & error logs
│   │   │   ├── AnalysisSummary/   # LLVM pass analysis metrics dashboard (11 actual counters)
│   │   │   └── StatusIndicator/   # Reusable status badges and stage indicators
│   │   ├── services/              # API abstraction layer
│   │   │   ├── api.js             # Standardized fetch client with ApiError handling
│   │   │   ├── compilerService.js # POST /api/compile service
│   │   │   └── executionService.js# POST /api/execute and GET /api/execute/health service
│   │   ├── hooks/                 # Custom React hooks
│   │   │   └── useBackendHealth.js# Health probe monitoring backend and Docker readiness
│   │   ├── utils/                 # Formatting utilities
│   │   │   └── formatters.js      # Duration, byte, pointer, and status formatting
│   │   ├── App.jsx                # Main split-pane application shell and state coordinator
│   │   ├── main.jsx               # React DOM root mounting
│   │   └── index.css              # Tailwind directives and custom scrollbars
│   └── tests/                     # Frontend automated test suite (30 tests via Vitest)
│       ├── setup.js               # Vitest DOM polyfills and test cleanup
│       ├── apiServices.test.js    # API client and service unit tests
│       ├── Toolbar.test.jsx       # Toolbar and action button unit tests
│       ├── OutputPanel.test.jsx   # Runtime output and violation card tests
│       ├── AnalysisSummary.test.jsx# Metrics dashboard rendering and N/A fallback tests
│       ├── App.test.jsx           # Frontend core integration tests
│       └── EndToEndWorkflow.test.jsx # Complete browser-to-runtime workflow tests (Step 23)
├── backend/                       # Node.js Express backend (Steps 19, 20 & 21)
│   ├── package.json               # Backend dependencies, scripts, and engine specifications
│   ├── .env.example               # Template environment configuration (LLVM toolchain & Docker settings)
│   ├── .gitignore                 # Backend-specific ignore rules (workspaces, logs, uploads)
│   ├── scripts/                   # Performance & benchmarking scripts
│   │   └── measurePerformance.js  # Automated pipeline timing & concurrent job benchmark (Step 23)
│   ├── docker/
│   │   └── Dockerfile.runner      # Minimal runner container definition (non-root mguser: 10001:10001)
│   ├── src/
│   │   ├── config/                # Centralized configuration loader (dotenv)
│   │   ├── controllers/           # HTTP request handlers (health, system, compile, execute)
│   │   ├── middleware/            # Error handling, 404 handler, request validation
│   │   ├── routes/                # Express router definitions (compile, execute, health, system)
│   │   ├── services/              # Pure business logic and modular orchestrators
│   │   │   ├── compiler/          # LLVM 18 toolchain integration services (Step 20)
│   │   │   │   ├── compilerConfig.js    # Toolchain path resolution & diagnostics sanitizer
│   │   │   │   ├── compilerRunner.js    # Safe child_process.spawn with timeout & output caps
│   │   │   │   ├── workspaceManager.js  # UUID workspace isolation & path-traversal safety
│   │   │   │   └── compilerService.js   # 3-stage pipeline (Clang IR -> Opt pass -> Linker)
│   │   │   └── execution/         # Secure Docker execution services (Step 21)
│   │   │       ├── executionConfig.js   # Docker flags, resource limits & availability check
│   │   │       ├── memoryGuardParser.js # Violation regex extractor & diagnostic categorizer
│   │   │       ├── dockerRunner.js      # Isolated container spawn with resource caps & cleanup
│   │   │       └── executionService.js  # Workspace validator, binary existence & execution coordinator
│   │   ├── utils/                 # Structured response formatters and logger
│   │   ├── app.js                 # Express app initialization (Helmet, CORS, Morgan)
│   │   └── server.js              # Server entry point with graceful shutdown
│   └── tests/                     # Automated test suite (83 tests: 57 unit + 26 integration)
│       ├── compile.test.js        # POST /api/compile C & C++ integration tests
│       ├── compilerPipeline.test.js # Toolchain, workspace safety & runner unit tests
│       ├── dockerRunner.test.js   # Docker argument hardening, resource caps & mock tests
│       ├── errorHandler.test.js   # Centralized error handler tests
│       ├── executionApi.test.js   # POST /api/execute integration tests & edge cases
│       ├── health.test.js         # GET /api/health tests
│       ├── memoryGuardParser.test.js # Violation parsing & status classification tests
│       ├── notFound.test.js       # 404 JSON fallback tests
│       ├── system.test.js         # GET /api/system/info diagnostic tests
│       └── integration/           # Dedicated End-to-End & Security Regression Suite (Step 23)
│           ├── endToEndSecurity.test.js # Comprehensive memory safety & compiler regression
│           ├── jobIsolation.test.js     # Path traversal, UUID checks, concurrent jobs & cleanup
│           └── dockerSecurity.test.js   # Docker hardening flags, socket probing & client aborts
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

---

### Step 21: Secure Docker-Based Runtime Execution

MemoryGuard enforces a strict zero-trust security architecture for running compiled user binaries. **Compiled binaries are NEVER executed directly on the host machine via Node.js `child_process`.** All executions are isolated inside ephemeral, single-use Docker containers with locked-down security profiles.

#### Security Hardening Guarantees

Every execution spawned through `docker run` applies the following container security flags:

| Security Flag | Enforcement Purpose |
| :--- | :--- |
| `--network none` | Completely disables container networking (no ingress, no egress, no LAN/WAN access). |
| `--read-only` | Root filesystem is mounted read-only; container cannot modify system files or binaries. |
| `--user 10001:10001` | Drops all root privileges; runs strictly as unprivileged user `mguser`. |
| `--cap-drop ALL` | Drops all Linux capabilities (no raw sockets, no ptrace, no mount, no chown). |
| `--security-opt no-new-privileges` | Prevents privilege escalation through `setuid` or `setgid` binaries. |
| `--memory 64m` | Strict memory ceiling prevents container-level memory exhaustion attacks. |
| `--cpus 0.5` | Restricts CPU quota to half a core to prevent CPU exhaustion and infinite loop denial-of-service. |
| `--pids-limit 32` | Prevents fork bombs and unbounded process creation. |
| `--tmpfs /tmp:rw,noexec,nosuid,size=16m` | Temporary in-memory scratch space with `noexec` and `nosuid` enforcement. |
| `-v <workspace>/program.bin:/app/program.bin:ro` | Only the specific compiled binary is mounted, strictly read-only. Host workspaces, source code, and secrets are inaccessible. |
| `--rm` | Guarantees automatic container destruction upon process exit or termination. |
| `--init` | Uses lightweight init process to reap zombie processes and forward termination signals cleanly. |
| Execution Timeout (default: 5000ms) | Kills stuck processes via `SIGTERM` followed by Docker-level `docker kill` if uncooperative. |
| Max Output Cap (default: 65,536 bytes) | Protects memory and response sizes by truncating oversized stdout/stderr streams. |

> [!CAUTION]
> **Strict Isolation Rule**: The Node.js backend does **NOT** provide any host-side fallback execution mechanism. If Docker is not installed or the Docker daemon is unreachable, `POST /api/execute` fails gracefully with HTTP status `503 Service Unavailable` (`DOCKER_UNAVAILABLE`).

#### Building the Execution Runner Image

Build the lightweight runner image prior to executing binaries:

```bash
# From the project root
docker build -f docker/Dockerfile.runner -t memoryguard-runner:latest .

# Or from within the backend directory
docker build -f backend/docker/Dockerfile.runner -t memoryguard-runner:latest .
```

The runner image uses `debian:bookworm-slim`, installs only `libstdc++6` and `ca-certificates` for dynamic C++ runtime support, creates the non-root user `mguser` (`10001:10001`), and sets `/app/program.bin` as its immutable `ENTRYPOINT`.

#### `POST /api/execute`
Executes a previously compiled binary within the secure Docker sandbox and returns structured execution metrics and MemoryGuard runtime violation diagnostics.

- **Request Body**:
```json
{
  "jobId": "7f3b891a-4d2c-4e89-9a1b-123456789abc"
}
```

- **Clean Execution Response (`200 OK` - `no_violations_detected`)**:
```json
{
  "success": true,
  "message": "Program execution completed inside secure Docker sandbox.",
  "data": {
    "jobId": "7f3b891a-4d2c-4e89-9a1b-123456789abc",
    "status": "no_violations_detected",
    "execution": {
      "exitCode": 0,
      "signal": null,
      "timedOut": false,
      "durationMs": 48,
      "stdout": "Computation finished successfully.\n",
      "stderr": "",
      "outputTruncated": false
    },
    "violation": null
  }
}
```

- **MemoryGuard Violation Detected (`200 OK` - `violation_detected`)**:
When an instrumented program triggers a spatial out-of-bounds or use-after-free violation, MemoryGuard logs the offending address and size to `stderr` and aborts (`SIGABRT`). The backend captures and categorizes this without marking the API call as a 500 error:
```json
{
  "success": true,
  "message": "Program execution completed inside secure Docker sandbox.",
  "data": {
    "jobId": "7f3b891a-4d2c-4e89-9a1b-123456789abc",
    "status": "violation_detected",
    "execution": {
      "exitCode": 134,
      "signal": "SIGABRT",
      "timedOut": false,
      "durationMs": 35,
      "stdout": "",
      "stderr": "MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x55d1a8f9c2a0, access_size=4 bytes\nAborted\n",
      "outputTruncated": false
    },
    "violation": {
      "violationType": "OUT_OF_BOUNDS_OR_UAF",
      "offendingAddress": "0x55d1a8f9c2a0",
      "accessSizeBytes": 4,
      "rawMessage": "MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x55d1a8f9c2a0, access_size=4 bytes"
    }
  }
}
```

- **Timeout Error (`504 Gateway Timeout`)**:
```json
{
  "success": false,
  "error": {
    "code": "TIMEOUT_ERROR",
    "message": "Execution timed out after 5000ms.",
    "jobId": "7f3b891a-4d2c-4e89-9a1b-123456789abc",
    "details": {
      "timedOut": true,
      "outputTruncated": false
    }
  }
}
```

- **Docker Unavailable Error (`503 Service Unavailable`)**:
```json
{
  "success": false,
  "error": {
    "code": "DOCKER_UNAVAILABLE",
    "message": "Docker daemon is not running or unreachable.",
    "jobId": "7f3b891a-4d2c-4e89-9a1b-123456789abc",
    "details": {
      "timedOut": false,
      "outputTruncated": false
    }
  }
}
```

#### `GET /api/execute/health`
Checks whether the Docker daemon is accessible and reports container execution readiness.
- **Status Code**: `200 OK`
- **Response Example**:
```json
{
  "success": true,
  "message": "Docker execution environment is available and operational.",
  "data": {
    "available": true,
    "version": "Docker version 29.2.1, build a4835de",
    "image": "memoryguard-runner:latest"
  }
}
```

---

### Step 22: MemoryGuard React Developer Frontend

MemoryGuard provides a high-performance web interface built with **React 18**, **Vite**, **Tailwind CSS**, and **Monaco Editor**, enabling interactive source editing, real-time compilation, memory-safety inspection, and secure containerized execution.

#### Core Capabilities

1. **Monaco Code Editor**:
   - Full syntax highlighting for C (C11) and C++ (C++17).
   - Independent code buffers preserving edits across language switching.
   - Built-in sample programs illustrating clean memory access, out-of-bounds writes, use-after-free, and pointer arithmetic.
2. **Real-Time Compiler & Pass Diagnostics**:
   - Visual tracking of the 3-stage compilation pipeline (`FRONTEND_COMPILATION`, `INSTRUMENTATION`, `LINKING`) with millisecond timings.
   - Raw Clang and LLVM pass build logs with syntax highlighting.
3. **Analysis Dashboard**:
   - Displays all 11 empirical MemoryGuard analysis counters:
     - `checksInserted` (calls to `@mg_check_access`)
     - `heapAccessesIdentified`
     - `allocatorCallsRewritten`
     - `totalAccessesExamined`
     - `nonHeapSkipped`, `unknownOriginSkipped`, `volatileAccessesInstrumented`, `atomicOperationsSkipped`, `unsupportedAddressSpaceSkipped`, `scalableVectorSkipped`, `incompatibleAllocatorCallsSkipped`
   - Highlights analyzed function control-flow graphs.
4. **Runtime Output & Violation Explorer**:
   - Isolated execution within ephemeral Docker containers.
   - Automatic classification: `no_violations_detected`, `violation_detected`, `execution_failed`, and `output_truncated`.
   - Dedicated MemoryGuard violation card displaying the offending pointer address, access size, and SIGABRT diagnostic.
   - Filterable console output tabs (Combined, Stdout, Stderr).
5. **Live Health Monitoring**:
   - Real-time indicator pills for backend Node.js server connectivity and Docker daemon readiness.

#### Frontend Scripts

```bash
# Start Vite development server (proxies /api requests to http://localhost:5000)
npm --prefix frontend run dev

# Run automated Vitest test suite (30 unit & integration tests)
npm --prefix frontend test

# Build production bundle
npm --prefix frontend run build
```

---

### Step 23: End-to-End Integration & Security Verification

Step 23 establishes a dedicated, comprehensive integration-testing layer verifying the complete MemoryGuard architecture across the compiler plugin, C runtime, Node.js backend pipeline, isolated Docker sandbox, and React developer frontend.

#### 1. End-to-End User Workflows Verified

The complete browser-to-runtime cycle was tested across both C (C11) and C++ (C++17):
1. **Source Submission**: User submits code via frontend Monaco Editor or API client.
2. **Frontend Compilation**: Express backend invokes Clang/Clang++ in the isolated workspace.
3. **LLVM IR Instrumentation**: MemoryGuard New Pass Manager pass analyzes instruction provenance, rewrites standard allocation call sites (`malloc`, `calloc`, `realloc`, `free`), and injects `@mg_check_access` calls before qualifying loads and stores.
4. **Runtime Linking**: Instrumented bitcode is linked against `libmemoryguard.a` to produce `program.bin`.
5. **Diagnostics Surface**: Backend returns pass counters (`checksInserted`, `heapAccessesIdentified`, `allocatorCallsRewritten`, `nonHeapSkipped`), displayed in the frontend Analysis Dashboard.
6. **Execution Dispatch**: User triggers Run (`POST /api/execute`), verifying that compilation and execution remain completely independent operations.
7. **Containerized Validation**: Binary is mounted strictly read-only into an ephemeral `memoryguard-runner:latest` container.
8. **Runtime Diagnostic Parsing**: Container output is parsed for MemoryGuard violation signatures or clean completion, rendered cleanly in the frontend console.

#### 2. Tested Security Regression Scenarios

| Scenario | Language | Compile Classification | Runtime Behavior | Verification Result |
| :--- | :---: | :---: | :---: | :--- |
| **Heap Out-of-Bounds Write** | C | Checks inserted (`checksInserted >= 3`) | Intercepted; SIGABRT triggered | Offending ptr & access size parsed; `violation_detected` |
| **Heap Out-of-Bounds Read** | C | Checks inserted (`checksInserted >= 2`) | Intercepted; SIGABRT triggered | Violating byte access captured; `violation_detected` |
| **Heap Use-After-Free** | C | Allocator rewritten; checks inserted | Intercepted; SIGABRT triggered | Dangling pointer store caught; `violation_detected` |
| **Heap Use-After-Free** | C++ | `std::malloc`/`std::free` rewritten | Intercepted; SIGABRT triggered | C++ dangling pointer write caught; `violation_detected` |
| **Interior Pointer Arithmetic** | C | Checks inserted (`checksInserted >= 2`) | In-bounds verified via AVL tree | Valid offset dereferenced cleanly; `no_violations_detected` |
| **Heap Array Traversal** | C | Allocator rewritten; checks inserted | In-bounds verified across all elements | Full array read/write validated; `no_violations_detected` |
| **Stack-Only Access** | C | Classified as `NON_HEAP` (0 checks) | Normal uninhibited execution | `checksInserted = 0`; `nonHeapSkipped > 0` |
| **Global-Only Access** | C | Classified as `NON_HEAP` (0 checks) | Normal uninhibited execution | `checksInserted = 0`; `nonHeapSkipped > 0` |
| **Infinite Loop Timeout** | C | Compiled successfully | Container killed after timeout | 504 `TIMEOUT_ERROR`; `timedOut = true` |
| **Excessive Stdout Output** | C | Compiled successfully | Stdout stream capped at byte ceiling | Status `output_truncated`; payload truncated safely |
| **Excessive Stderr Output** | C | Compiled successfully | Stderr stream capped at byte ceiling | Status `output_truncated`; buffer preserved |
| **Nonzero Application Exit** | C | Compiled successfully | Normal exit code (e.g., `exit(42)`) | Status `no_violations_detected`; exitCode preserved |
| **Invalid Syntax (C)** | C | Failed at `FRONTEND_COMPILATION` | Not executed; Run disabled | 422 `COMPILATION_ERROR`; compiler diagnostics returned |
| **Invalid Syntax (C++)** | C++ | Failed at `FRONTEND_COMPILATION` | Not executed; Run disabled | 422 `COMPILATION_ERROR`; compiler diagnostics returned |

#### 3. API & Job Isolation Guarantees

- **UUID Integrity**: Rejects malformed, non-UUID job parameters with `400 INVALID_JOB_ID` (`SAFE_ID_REGEX`).
- **Path Traversal Defense**: All workspace operations enforce `assertPathWithin()`, preventing access to paths outside `backend/.workspaces/`.
- **Target Restriction**: Clients can only request execution by `jobId`. Arbitrary executable paths (e.g. `/bin/sh`) are discarded.
- **Concurrent Isolation**: Simultaneous compilation and execution requests operate in strictly disjoint workspace folders with zero crosstalk.
- **Repeated Execution**: Successfully compiled binaries can be executed repeatedly without workspace corruption or artifact deletion.
- **Workspace Expiration**: `cleanupExpiredWorkspaces()` safely prunes workspaces older than a configured threshold (default 1 hour) without affecting active jobs.

#### 4. Docker Security Hardening

Container dispatch in `backend/src/services/execution/dockerRunner.js` applies 12 non-negotiable security flags:
1. `--network none` (Disables network stack entirely)
2. `--read-only` (Root filesystem is immutable)
3. `--user 10001:10001` (Drops root, runs as `mguser`)
4. `--cap-drop ALL` (Drops all Linux capabilities)
5. `--security-opt no-new-privileges` (Blocks setuid escalation)
6. `--memory 64m` (Enforces memory ceiling)
7. `--cpus 0.5` (Enforces CPU core limit)
8. `--pids-limit 32` (Prevents fork bombs)
9. `--init` (Reaps zombie processes cleanly)
10. `--tmpfs /tmp:rw,noexec,nosuid,size=16m` (Scratch space with noexec)
11. `-v <workspace>/program.bin:/app/program.bin:ro` (Mounts only the single binary read-only)
12. `--rm` (Guarantees automatic cleanup upon exit)

Forbidden options (`--privileged`, `/var/run/docker.sock`) are strictly blocked. On client disconnect, `AbortController` triggers immediate container termination and removal.

#### 5. Empirical Performance Measurements

Benchmarked using `backend/scripts/measurePerformance.js` on 1,000-element heap read/write workloads:

| Pipeline Metric | Mean Duration | Median Duration | Min Duration | Max Duration |
| :--- | :---: | :---: | :---: | :---: |
| **Stage 1: Frontend Clang** | 501.6 ms | 330.0 ms | 249.0 ms | 1279.0 ms |
| **Stage 2: MemoryGuard Pass** | 235.6 ms | 239.0 ms | 198.0 ms | 274.0 ms |
| **Stage 3: Runtime Linking** | 866.8 ms | 844.0 ms | 784.0 ms | 1006.0 ms |
| **Total Compilation Pipeline** | 1606.7 ms | 1422.5 ms | 1268.2 ms | 2386.0 ms |
| **5 Concurrent Compilations** | 1742.9 ms (total) | ~348.6 ms / job | 100% success rate | Zero interference |
| **Container Dispatch Overhead** | 211.0 ms | 213.0 ms | 195.9 ms | 223.7 ms |

#### 6. Supported Safety Cases & Known Limitations

> [!IMPORTANT]
> **No Claim of Complete Memory Safety**: MemoryGuard is a dynamic compiler-assisted verification prototype. It instruments and inspects memory operations executed at runtime. Unexecuted code branches, uncalled functions, or allocations outside the tracked registry cannot be proven safe.

- **Supported Allocators**: `malloc`, `calloc`, `realloc`, `free`, and C++ `std::malloc`/`std::free`.
- **Supported Provenance**: Interior pointers via GEP, pointer arithmetic, struct field offsets, multi-dimensional array offsets, PHI and Select instructions combining candidate origins.
- **Conservatively Skipped / Unsupported**:
  - Raw C++ `new`/`delete` expressions when not routed through standard C allocator interception.
  - `IntToPtr` and `PtrToInt` address-space casts.
  - Atomic read-modify-write operations (preserved without instrumentation to avoid breaking memory ordering).
  - Scalable vector operations and non-standard address spaces.

#### 7. Comprehensive Test Commands

```bash
# 1. Run all backend tests (94 tests: 57 unit + 37 integration)
npm --prefix backend test

# 2. Run backend unit tests only (57 tests)
npm --prefix backend run test:unit

# 3. Run backend integration & security tests only (37 tests)
npm --prefix backend run test:integration

# 4. Run frontend tests (30 unit & E2E workflow tests via Vitest)
npm --prefix frontend test

# 5. Run LLVM Pass and C Runtime CTest regression suite
wsl bash -c "cd '/mnt/d/Notes/SEM VII/Compiler Design/Project' && ctest --test-dir build --output-on-failure"

# 6. Run automated performance & reliability measurement script
node backend/scripts/measurePerformance.js
```

---

### Step 24: Docker Security Lockdown & Hardened Tamper Resistance

During security auditing, a critical vulnerability was addressed where `POST /api/execute` accepted a `customConfig` object directly from the client request body and merged it over default runner options, potentially allowing attackers to override Docker isolation settings.

#### Key Security Remediations

1. **Complete Removal of API-Controlled Configuration**:
   - `backend/src/controllers/executionController.js` strictly extracts only `{ jobId }` from `req.body`.
   - Any client payload attempting to supply `customConfig`, `--privileged`, `network`, `userUidGid`, or arbitrary mounts is completely dropped.
2. **Immutable Security Architecture (`IMMUTABLE_SECURITY`)**:
   - In `backend/src/services/execution/dockerRunner.js`, non-negotiable security flags are defined in a frozen constant and directly applied to `dockerArgs`:
     - `--network none` (Complete network isolation)
     - `--read-only` (Immutable root filesystem)
     - `--cap-drop ALL` (Zero Linux capabilities)
     - `--security-opt no-new-privileges` (No privilege escalation)
     - `--user 10001:10001` (Unprivileged runner user `mguser`)
     - `--memory 64m` (Hard memory quota)
     - `--cpus 0.5` (Hard CPU quota)
     - `--pids-limit 32` (Fork bomb prevention)
     - `--init` & `--rm` (Ephemeral execution and zombie process reaping)
     - `--tmpfs /tmp:rw,noexec,nosuid,size=16m` (Restricted writable scratch space)
     - `-v <executable>:/app/program.bin:ro` (Strict single-file read-only mount)
   - These flags cannot be overridden by any parameter, payload, or module caller.
3. **Internal Test Isolation**:
   - Internal test harnesses requiring non-security overrides (such as `executionTimeoutMs` or `maxOutputBytes`) use private runner injection (`setRunnerTestOverrides` / `resetRunnerTestOverrides`) or `_internalTestOverrides`.
   - Security parameters are strictly ignored even if passed to internal test hooks.
4. **Dedicated Tamper-Resistance Regression Suite**:
   - `backend/tests/integration/dockerSecurityLockdown.test.js` provides 11 targeted regression tests confirming that attempts to override network, user privileges, read-only rootfs, capabilities, privileged mode, resource quotas, or volume mounts via HTTP or direct invocation are fully neutralized.

---

### Step 25: Fix realloc() Pointer Invalidation & Registry Failure in C Runtime

In standard C (C11 / C17 / C23), when `realloc(ptr, size)` relocates or deallocates a memory block, the original pointer value `ptr` becomes indeterminate. Evaluating or casting `ptr` thereafter (such as evaluating `(uintptr_t)ptr` to delete the old AVL tree entry) constitutes undefined behavior. Furthermore, if a registry node cannot be allocated after a successful reallocation, returning the pointer untracked creates a silent memory safety hole.

#### Key Remediations

1. **Pre-Reallocation Address Preservation (`old_start`)**:
   - In `runtime/src/memoryguard.c` (`mg_realloc_external`), the base address is preserved as an integer (`uintptr_t old_start = (uintptr_t)ptr;`) before invoking the underlying allocator.
   - After successful reallocation, `old_start` is passed to `delete_avl()` to detach the old registry entry.
   - The pointer `ptr` is never evaluated, converted, or dereferenced after calling `realloc()`.
2. **Guaranteed Node Availability Before Realloc**:
   - `mg_realloc_external()` pre-allocates an `Allocation` node via `mg_call_alloc_node_locked()` **before** invoking the underlying `realloc` operation.
   - If node allocation fails, the function aborts immediately before calling `realloc`, preserving the existing allocation and all its metadata intact in the registry, and returns `NULL`.
   - On successful reallocation, if the detached AVL node (`deleted`) is available, it is recycled and the reserved node is freed immediately (`free(reserved_node)`). If `deleted` was `NULL`, the reserved node is used.
   - Tracking is guaranteed never to be silently lost for successfully reallocated memory.
3. **Reallocation Failure Safety**:
   - If `realloc()` returns `NULL` on non-zero size requests, the reserved node is freed, `delete_avl()` is never called, and the original allocation entry remains completely untouched in the AVL tree.
4. **Zero-Size Reallocation Platform Contracts**:
   - When `new_size == 0`:
     - If the platform frees memory and returns `NULL` (e.g. standard glibc), the reserved node is freed, the entry at `old_start` is removed, and `total_allocations` decremented.
     - If the platform returns a distinct zero-sized non-NULL pointer, the old entry is detached, the reserved node is appropriately freed or reused, and the entry is tracked with `size = 0`.
5. **Synchronized Test Seams & Node Failure Injection**:
   - Added `mg_test_set_realloc_fn()` and `mg_test_reset_realloc_fn()` synchronized under `registry_mutex` to allow test suites to test guaranteed relocation, in-place realloc, failure, and zero-size behaviors without non-deterministic libc heap layouts.
   - Added narrow test seam `mg_test_set_node_alloc_fail()` to test registry node allocation failure deterministically.
   - In default/production operation, hooks are inactive and MemoryGuard routes directly to the system allocator.
6. **Sanitizer Verification**:
   - Verified clean execution under AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), and ThreadSanitizer (TSan) across 55,039 assertions with zero warnings, zero data races, and strict `-Wall -Wextra -Wpedantic -Werror`.

---

### Step 26: Docker-Isolated Compiler Pipeline

Previously, the runtime execution service (`POST /api/execute`) ran compiled binaries inside a restricted Docker container, but the compilation and instrumentation pipeline (`POST /api/compile`) executed Clang and Opt directly on the host or WSL environment. In Step 26, the entire C/C++ compilation and LLVM instrumentation toolchain was containerized into a dedicated restricted Docker sandbox (`memoryguard-compiler:latest`).

#### Architecture: Compiler Sandbox vs. Runtime Runner

| Dimension | Compiler Sandbox (`memoryguard-compiler`) | Runtime Runner (`memoryguard-runner`) |
| :--- | :--- | :--- |
| **Base Image** | Ubuntu 24.04 LTS (pinned with Clang 18 & Opt 18) | Debian 12 (bookworm-slim) minimal base |
| **Installed Tooling** | Clang 18, Opt 18, libc6-dev, libstdc++-14-dev, `MemoryGuardPass.so`, `libmemoryguard.a` | Minimal C runtime libraries; zero compiler tools, headers, or build utilities |
| **Network** | `--network none` (immutable, zero external egress) | `--network none` (immutable, zero external egress) |
| **Privileges** | `--cap-drop ALL`, `--security-opt no-new-privileges` | `--cap-drop ALL`, `--security-opt no-new-privileges` |
| **User Identity** | Unprivileged `mgcompiler` (`10001:10001`) | Unprivileged `mguser` (`10001:10001`) |
| **Filesystem Mounts** | Separate `/src:ro` (source code) and `/out:rw` (output binary & LLVM IR artifacts) | Single-file read-only mount: `-v <bin>:/app/program.bin:ro` |
| **Root Filesystem** | `--read-only` | `--read-only` |
| **Scratch Space** | `--tmpfs /tmp:rw,noexec,nosuid,size=64m` | `--tmpfs /tmp:rw,noexec,nosuid,size=16m` |
| **Resource Quotas** | Memory: `512m`, CPU: `1.0`, PIDs: `64` | Memory: `64m`, CPU: `0.5`, PIDs: `32` |
| **Lifecycle** | `--init`, `--rm` (ephemeral container cleanup) | `--init`, `--rm` (ephemeral container cleanup) |

#### Container Image Build Instructions

```bash
# 1. Build the compiler sandbox image
docker build -t memoryguard-compiler:latest -f docker/Dockerfile.compiler .

# 2. Build the runtime runner image
docker build -t memoryguard-runner:latest -f docker/Dockerfile.runner .
```

#### Key Implementation Details

1. **Dedicated Container Orchestrator (`docker/compile_runner.sh`)**:
   - Executes the 3-stage pipeline in strict sequence:
     - Stage 1: `clang-18` (or `clang++-18`) emits unoptimized LLVM IR (`/out/source.ll`).
     - Stage 2: `opt-18` loads `MemoryGuardPass.so`, runs pointer-provenance analysis, and emits instrumented IR (`/out/source_inst.ll`).
     - Stage 3: `clang-18` (or `clang++-18`) links instrumented IR with `libmemoryguard.a`, `-pthread`, and `-lm` into `/out/program.bin`.
   - Explicitly sets execution permissions `chmod 0755 /out/program.bin`.
   - Emits structured stage markers (`=== STAGE_FAILED: <STAGE> ===` or `=== COMPILATION_SUCCESS ===`) mapped to HTTP error categories.

2. **Immutable Server-Side Security (`IMMUTABLE_COMPILER_SECURITY`)**:
   - `backend/src/services/compiler/compilerConfig.js` enforces frozen security parameters that cannot be tampered with or modified via API request payloads.
   - Child process execution invokes `docker` with `shell: false` and strict array-based argument vectors.

3. **Workspace Isolation & Path Containment**:
   - Workspaces separate source and output directories (`<workspace>/src` and `<workspace>/out`).
   - Diagnostic sanitizer strips both host workspace paths and container-internal mount paths (`/src/` and `/out/`) before returning messages to the client.

4. **Production Fail-Closed Compilation & Test-Only Explicit Fallback**:
   - In production (`NODE_ENV === 'production'`), compilation **always** executes inside the restricted `memoryguard-compiler:latest` Docker container.
   - If the Docker daemon is unavailable, unreachable, or fails in production, the compilation pipeline fails closed immediately with `TOOLCHAIN_ERROR` / `DOCKER_UNAVAILABLE` (`503 Service Unavailable`). It **never** silently falls back to host-side Clang, Opt, or compiler binaries.
   - Any local host/WSL compilation used during automated testing must be explicitly enabled via `enableCompilerTestFallback()` or `_internalTestOverrides: { allowTestFallback: true }`. Calling test fallback in production strictly throws a security violation.
   - HTTP request parameters, headers, and query arguments are strictly prohibited from activating test fallback or modifying security parameters.
   - Pre-existing or stale `program.bin` binaries are proactively purged prior to compilation and on all failure/abort paths to guarantee that a failed compilation can never leave a stale binary eligible for execution.

5. **Security Audit & Real-Container Verification Status**:
   - The security configuration of `docker run` (`--network none`, `--read-only`, `--cap-drop ALL`, `--security-opt no-new-privileges`, `--user 10001:10001`, resource limits, read-only `/src` mount, isolated `/out` mount) was formally audited and verified across 17 test suites (113 tests passing or properly skipped).
   - *Host Verification Note*: Docker daemon is unavailable on host. Real container integration was not verified. All real-container integration tests automatically detect daemon availability and skip gracefully when the daemon is offline.

---

### Current Limitations & Future Roadmap

- **Interactive Standard Input**: Container execution currently supports static execution; interactive terminal multiplexing (PTY) may be explored in future extensions.
- **Persistent Job History**: Workspaces are currently tied to ephemeral job UUIDs; persistent history with database backends may be added.
- **Authentication**: Endpoints currently operate in a local single-tenant mode without API key authentication.


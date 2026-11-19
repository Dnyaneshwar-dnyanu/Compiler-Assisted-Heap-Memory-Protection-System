# MemoryGuard Architecture Specification

MemoryGuard is an end-to-end, compiler-assisted heap memory protection platform designed for C and C++ programs. It couples an **LLVM 18 New Pass Manager** instrumentation plugin with an **augmented C runtime library**, coordinated by a **Node.js/Express backend**, isolated within **hardened Docker containers**, and operated through an interactive **React/Vite web application**.

---

## 1. System Architecture Overview

The MemoryGuard architecture is divided into three distinct execution domains:

1. **User & Orchestration Domain**:
   - **Frontend (React 18 + Vite + Monaco Editor)**: Code authoring, syntax highlighting, sample loading, compilation triggers, execution invocation, and visual diagnostics reporting.
   - **Backend (Node.js 20 + Express)**: API gateway, job lifecycle management, UUID workspace isolation, compiler orchestration, runtime container runner, and structured diagnostics parsing.
2. **Compiler Domain (Docker Sandbox: `memoryguard-compiler`)**:
   - **Clang 18 Toolchain**: Front-end compilation to LLVM intermediate representation (IR).
   - **MemoryGuard LLVM 18 Pass Plugin (`MemoryGuardPass.so`)**: Static pointer-origin analysis, heap allocation interception, and runtime boundary check injection.
   - **Runtime Linker**: Static linkage against `libmemoryguard.a` to produce the hardened executable (`program.bin`).
3. **Execution Domain (Docker Sandbox: `memoryguard-runner`)**:
   - **Unprivileged Sandbox**: Non-root execution (`10001:10001`), read-only root filesystem, `--network none`, dropped Linux capabilities, and strict CPU/memory/PIDs limits.
   - **MemoryGuard C Runtime (`libmemoryguard.a`)**: Thread-safe AVL interval tree allocation registry, real-time boundary verification (`mg_check_access`), signal handling, and formatted violation reporting.

```mermaid
flowchart TD
    subgraph Client ["Client Browser"]
        UI["React 18 / Vite UI<br/>(Monaco Editor & Diagnostics)"]
    end

    subgraph Host ["Node.js API Gateway (Host)"]
        API["Express REST API<br/>(/api/compile, /api/execute)"]
        WM["Workspace Manager<br/>(UUID Workspaces & Symlink Guard)"]
        CR["Compiler Runner<br/>(Docker CLI Controller)"]
        ER["Execution Runner<br/>(Docker CLI Controller)"]
        DP["Diagnostic Parser<br/>(JSON & Log Normalizer)"]
    end

    subgraph CompilerSandbox ["Compiler Sandbox Container (memoryguard-compiler:latest)"]
        CLANG["Clang 18<br/>(C/C++ to LLVM IR)"]
        PASS["MemoryGuardPass.so<br/>(LLVM 18 NPM Plugin)"]
        LINK["Linker / Clang<br/>(+ libmemoryguard.a)"]
        BIN["Target Executable<br/>(program.bin + manifest.json)"]
    end

    subgraph RuntimeSandbox ["Runtime Sandbox Container (memoryguard-runner:latest)"]
        EXEC["Isolated Binary Execution<br/>(No Network, Read-Only, Non-Root)"]
        AVL["AVL Interval Tree Registry<br/>(O(log N) Interior Pointer Lookup)"]
        CHECK["mg_check_access()<br/>(Boundary & UAF Guard)"]
        DIAG["Runtime Diagnostics<br/>(SIGABRT Violation Output)"]
    end

    UI -->|"POST /api/compile (Source)"| API
    API --> WM
    WM -->|"Mount Workspace (ro)"| CR
    CR -->|"Run mg-compile"| CLANG
    CLANG -->|"Emit LLVM IR"| PASS
    PASS -->|"Instrumented IR"| LINK
    LINK -->|"Output Binary"| BIN
    BIN -->|"Artifact Verified"| CR
    CR -->|"Compilation Results"| API
    API -->|"Compiler Output & Diagnostics"| UI

    UI -->|"POST /api/execute (jobId)"| API
    API -->|"Validate Manifest & Executable"| ER
    ER -->|"Mount Executable (ro)"| EXEC
    EXEC --> CHECK
    CHECK <--> AVL
    CHECK -->|"Violation Detected"| DIAG
    DIAG -->|"stdout / stderr / exit code"| ER
    ER --> DP
    DP -->|"Execution Response"| API
    API -->|"Status, Logs & Violations"| UI
```

---

## 2. LLVM Instrumentation Pass Architecture

The compiler pass is implemented as a modern LLVM 18 New Pass Manager plugin (`llvm::PassInfoMixin<MemoryGuardPass>`), dynamically loaded via `opt -load-pass-plugin=MemoryGuardPass.so -passes=memoryguard`.

### 2.1 Pass Pipeline & Traversal

1. **Module & Function Scanning**: Traverses all functions within the LLVM Module (`Module::functions()`). External declarations and MemoryGuard runtime helper functions (`mg_*`) are skipped to prevent recursive instrumentation loops.
2. **Allocation Call Interception**:
   - Identifies standard heap allocation calls: `malloc`, `calloc`, `realloc`, and `free`.
   - Injects call-site rewrites replacing `malloc(sz)` with `mg_intercept_malloc(sz)`, `calloc(n, sz)` with `mg_intercept_calloc(n, sz)`, `realloc(ptr, sz)` with `mg_intercept_realloc(ptr, sz)`, and `free(ptr)` with `mg_intercept_free(ptr)`.
   - Interceptors automatically register allocated heap intervals into the runtime's AVL tree.
3. **Pointer-Origin & Provenance Analysis**:
   - For every memory access instruction (`llvm::LoadInst` and `llvm::StoreInst`), recursively traces the pointer operand backwards through the LLVM def-use chain.
   - Classification categories:
     - `HEAP_CANDIDATE`: Traced directly to an allocation call, a heap-derived `GetElementPtrInst` (GEP), or a pointer loaded from a heap buffer.
     - `NON_HEAP`: Statically proven stack allocation (`AllocaInst`) or global/static variable (`GlobalVariable`).
     - `UNKNOWN`: Function parameters, opaque external calls, complex phi-nodes, or int-to-ptr casts.
4. **Boundary Check Insertion**:
   - For qualified pointer candidates, calculates the accessed element size using the target machine's `llvm::DataLayout`.
   - Emits a direct call to `mg_check_access(ptr, size, is_write, access_type)` immediately prior to the `LoadInst` or `StoreInst`.
   - Applies LLVM metadata tags (`!mg.checked`) to inserted check instructions to ensure pass idempotency.

```mermaid
flowchart TD
    A["Instruction in Function"] --> B{"Is LoadInst or StoreInst?"}
    B -- No --> C{"Is Heap Allocator Call?"}
    C -- Yes --> D["Rewrite to mg_intercept_* API"]
    C -- No --> E["Skip Instruction"]
    B -- Yes --> F{"Has !mg.checked Metadata?"}
    F -- Yes --> E
    F -- No --> G["Trace Pointer Operand Provenance"]
    G --> H{"Provenance Category"}
    H -- "NON_HEAP (Alloca / Global)" --> E
    H -- "HEAP_CANDIDATE" --> I["Calculate Access Size (DataLayout)"]
    H -- "UNKNOWN" --> J{"Conservative Mode Active?"}
    J -- "Instrument Unknown" --> I
    J -- "Skip Unknown" --> E
    I --> K["Create mg_check_access() Call"]
    K --> L["Insert Before Load/Store"]
    L --> M["Attach !mg.checked Metadata"]
    M --> N["Pass Success / Counter Updated"]
```

---

## 3. C Runtime Library Architecture

The runtime library (`libmemoryguard.a`) provides the memory allocation tracking engine and dynamic validation logic.

### 3.1 Allocation Registry: Self-Balancing AVL Interval Tree

Rather than using an $O(N)$ linear linked list, MemoryGuard maintains an augmented AVL interval tree:

- **Node Attributes**:
  ```c
  typedef struct mg_alloc_node {
      uintptr_t start;             /* Allocation base address */
      uintptr_t end;               /* Allocation upper bound: start + size */
      size_t size;                 /* Payload size in bytes */
      int is_active;               /* Active status flag (1 = active, 0 = freed) */
      int owns_payload;            /* 1 if runtime allocated, 0 if external */
      int height;                  /* AVL tree subtree height */
      struct mg_alloc_node *left;  /* Left subtree */
      struct mg_alloc_node *right; /* Right subtree */
  } mg_alloc_node_t;
  ```
- **Disjoint Interval Search ($O(\log N)$)**:
  Given an interior pointer `P` and access size `S`, the registry locates the enclosing allocation interval:
  $$\text{start} \le P \quad \land \quad P + S \le \text{end}$$
  If $P + S < P$, an integer overflow has occurred and the check fails immediately.
  *Algorithmic Guarantee*: Because active heap allocations in valid program memory are mutually disjoint (non-overlapping), at most one allocation can contain address `P`. Searching ordered `start` addresses with subtree `max_end` pruning guarantees a single descent path bounded by tree height $h \le 1.44 \log_2 N = O(\log N)$. (For arbitrary, non-disjoint overlapping intervals, interval overlap queries without disjointness would require subtree backtracking).
- **Self-Balancing Invariants**: Standard AVL rotations (Left-Left, Right-Right, Left-Right, Right-Left) ensure $O(\log N)$ worst-case insertion, deletion, and lookup time.
- **Concurrency**: Thread-safe access is guaranteed via internal POSIX pthread mutexes (`pthread_mutex_t registry_mutex`), protecting registry operations against race conditions in multi-threaded programs.

### 3.2 Violation Detection and Reporting

When `mg_check_access(ptr, size, is_write, access_type)` detects a safety breach:
1. It formats a standardized diagnostic message to `stderr`:
   ```
   =================================================================
   == MEMORYGUARD VIOLATION DETECTED ==
   Violation Type : Out-of-Bounds Write
   Target Pointer : 0x00007ffc8213b028
   Access Size    : 4 bytes
   Allocation Base: 0x00007ffc8213b000 - 0x00007ffc8213b020 (Size: 32)
   Status         : Access extends 8 bytes beyond allocation boundary
   =================================================================
   ```
2. The runtime issues `abort()`, terminating execution with signal `SIGABRT` (exit code 134).
3. The execution service captures `stderr`, parses the diagnostic fields, and maps them to structured JSON for the frontend.

---

## 4. Container Isolation Architecture

Compilation and execution are decoupled into two distinct, security-hardened Docker containers:

| Dimension | Compiler Sandbox (`memoryguard-compiler:latest`) | Runtime Sandbox (`memoryguard-runner:latest`) |
|:---|:---|:---|
| **Base Image** | Ubuntu 24.04 (Noble) | Ubuntu 24.04 (Noble, Minimal) |
| **Installed Packages** | `clang-18`, `llvm-18`, `build-essential` | Minimal C/C++ runtime (`libc6`, `libstdc++6`) |
| **Image Size** | 798 MB | 127 MB |
| **User Identity** | `mgcompiler` (UID: 10001, GID: 10001) | `mgrunner` (UID: 10001, GID: 10001) |
| **Networking** | `--network none` | `--network none` |
| **Root Filesystem** | `--read-only` | `--read-only` |
| **Linux Capabilities** | Dropped all (`--cap-drop ALL`) | Dropped all (`--cap-drop ALL`) |
| **Security Options** | `no-new-privileges:true` | `no-new-privileges:true` |
| **Memory Limit** | 512 MB | 128 MB |
| **CPU Limit** | 1.5 cores | 1.0 core |
| **PIDs Limit** | 128 | 32 |
| **Execution Timeout**| 15,000 ms | 5,000 ms |
| **Workspace Mount** | Assigned UUID workspace directory only | Read-only mount of `program.bin` |

---

## 5. End-to-End Execution Lifecycle

```mermaid
sequenceDiagram
    autonumber
    actor User as Developer
    participant UI as React UI (Browser)
    participant Server as Express Backend
    participant WM as Workspace Manager
    participant CDocker as Compiler Container
    participant RDocker as Runner Container

    User->>UI: Enter C/C++ code & click "Compile"
    UI->>Server: POST /api/compile { language, sourceCode }
    Server->>WM: Create /tmp/memoryguard-workspaces/{jobId}
    WM-->>Server: Workspace verified (symlink & path safe)
    Server->>CDocker: Run mg-compile inside compiler container
    CDocker->>CDocker: Clang -> IR -> MemoryGuardPass -> Linker
    CDocker-->>Server: Compilation finished (exit 0, manifest.json)
    Server-->>UI: Return 200 OK { jobId, stages, diagnostics }
    UI->>User: Display compiler diagnostics & enable "Run"

    User->>UI: Click "Run"
    UI->>Server: POST /api/execute { jobId }
    Server->>WM: Validate jobId & verify manifest.json integrity
    Server->>RDocker: Run program.bin inside runner container
    RDocker->>RDocker: Boundary check triggers SIGABRT on violation
    RDocker-->>Server: Execution complete (exit code 134, stderr)
    Server->>Server: Parse MemoryGuard violation tokens
    Server-->>UI: Return 200 OK { status: "violation", violations: [...] }
    UI->>User: Display red violation banner & error details
```

This multi-layer architecture guarantees that untrusted user code can neither compromise the host filesystem nor tamper with compiler artifacts or other concurrent execution jobs.

# MemoryGuard Technical Limitations & Safety Boundary Scope

This document provides a candid, technical assessment of the capabilities, partial support zones, and known limitations of the MemoryGuard heap memory protection system.

---

## 1. Safety Paradigm & Fundamental Scope

> [!IMPORTANT]
> MemoryGuard is a **compiler-assisted heap memory-boundary checking system with dynamic runtime validation**.
>
> It is **NOT** a formal mathematical memory-safety proof system, nor does it guarantee total memory safety for arbitrary C and C++ programs.

MemoryGuard operates by instrumenting heap allocation call-sites and inserting runtime boundary checks before heap-derived memory access operations. It provides high-probability spatial and temporal heap error detection with lower overhead than heavyweight binary-translation emulators (e.g., Valgrind), but carries specific architectural boundaries.

---

## 2. Capability Matrix

| Feature / Memory Area | Support Level | Implementation Notes |
|:---|:---:|:---|
| **Heap Spatial Bounds (OOB Write)** | **Fully Supported** | Catches off-by-one, positive index overflow, and negative offsets beyond allocation base. |
| **Heap Spatial Bounds (OOB Read)** | **Fully Supported** | Catches reading unallocated heap memory beyond buffer limits. |
| **Heap Temporal Safety (Use-After-Free)** | **Fully Supported** | Dereferencing pointers after `free()` or `delete` triggers registry lookup failure and `SIGABRT`. |
| **Heap Temporal Safety (Double Free)** | **Fully Supported** | Calling `free()` on an already-freed pointer is caught by interceptor. |
| **Interior Pointers** | **Fully Supported** | Pointers pointing inside a buffer (e.g. `&ptr[5]`) resolve correctly via AVL range queries. |
| **Calloc & Realloc** | **Fully Supported** | Tracks zero-initialized buffers and reallocated memory without pointer invalidation bugs. |
| **Interprocedural Provenance** | **Partially Supported** | Tracked across functions within the same LLVM module; degraded across opaque dynamic libraries. |
| **C++ Standard Library Containers** | **Partially Supported** | Standard heap allocations (`std::vector`, dynamic allocations) tracked; stack SSO buffers skipped. |
| **Stack Buffer Overflows** | **Unsupported** | MemoryGuard explicitly targets the heap. Local stack variables (`char buf[16]`) are not tracked. |
| **Global / Static Variable Bounds** | **Unsupported** | Global `.data` and `.bss` structures are identified as `NON_HEAP` and skipped. |
| **Raw Memory Intrinsics** | **Unsupported** | `llvm.memcpy`, `llvm.memmove`, and `llvm.memset` lowered to inline assembly bypass checks unless wrapped. |
| **Custom Memory Allocators** | **Unsupported** | Custom slab, arena, or bump allocators using `mmap`/`sbrk` directly bypass `malloc` interceptors. |
| **Concurrent TOCTOU Races** | **Unsupported** | Deallocation between a valid `mg_check_access()` check and the CPU access instruction cannot be blocked. |
| **Type Confusion & Uninitialized Reads** | **Unsupported** | Reading uninitialized heap memory within valid bounds is not detected. |

---

## 3. Detailed Technical Limitations

### 3.1 Non-Heap Memory Exclusions (Stack & Global Variables)
MemoryGuard focuses specifically on the dynamic heap. The LLVM pass performs pointer-origin analysis:
- If a pointer resolves to an `llvm::AllocaInst`, it is classified as `NON_HEAP` and no checks are inserted.
- If a pointer resolves to a `llvm::GlobalVariable`, it is classified as `NON_HEAP` and skipped.
Consequently, stack buffer overflows (e.g., `char stack_buf[10]; stack_buf[20] = 'a';`) will not trigger a MemoryGuard runtime violation. Traditional defenses (such as Clang `-fstack-protector-strong` or AddressSanitizer) should be used for stack protection.

### 3.2 Pointer Provenance Ambiguity & Unknown Origins
In complex C/C++ code, static analysis cannot always resolve pointer provenance:
- **Indirect Function Calls & Virtual Tables**: When functions are invoked via opaque function pointers, parameter provenance across call boundaries cannot be statically resolved.
- **Integer-to-Pointer Casts**: Code performing pointer arithmetic by casting to `uintptr_t` and back creates an opaque origin (`UNKNOWN`).
- **External Shared Libraries**: Pointers received from or passed into pre-compiled shared libraries (`.so`) where LLVM IR / bitcode is unavailable cannot be traced.
*Current Strategy*: MemoryGuard classifies these as `UNKNOWN`. In default mode, accesses to `UNKNOWN` pointers are instrumented defensively, relying on the runtime AVL tree to determine if the address falls within tracked heap intervals.

### 3.3 Disjoint Heap Interval vs. Arbitrary Overlap Query Complexity
The $O(\log N)$ point-containment search complexity in the allocation registry depends on the invariant that valid active heap allocations are mutually disjoint ($[\text{start}_i, \text{end}_i) \cap [\text{start}_j, \text{end}_j) = \emptyset$). If applied to general geometric or interval-scheduling problems where arbitrary overlapping intervals occur, enclosing interval search with subtree backtracking is not guaranteed $O(\log N)$ in the worst case. MemoryGuard's performance guarantee is strictly tied to disjoint dynamic heap allocations.

### 3.3 Low-Level Memory Intrinsics
Compilers frequently lower operations like `memcpy(&dst, &src, size)` directly to LLVM intrinsic instructions (`@llvm.memcpy.p0.p0.i64`). Because these are intrinsic instructions rather than standard function calls or individual `LoadInst`/`StoreInst` sequences, they bypass standard MemoryGuard load/store instrumentation unless explicitly expanded into checked memory loops.

### 3.4 Custom and Embedded Allocators
MemoryGuard intercepts standard C runtime allocation symbols (`malloc`, `calloc`, `realloc`, `free`, `aligned_alloc`) and C++ operators (`new`, `delete`). If an application implements its own custom memory allocator via `sbrk()` or direct `mmap()` system calls without invoking standard allocation symbols, those heap regions will not be registered in the AVL interval tree.

### 3.5 Multithreaded Time-Of-Check to Time-Of-Use (TOCTOU) Races
The MemoryGuard allocation registry is thread-safe, utilizing a POSIX pthread mutex to serialize registry modifications. However, MemoryGuard does not enforce atomic memory access at the CPU hardware level:
```
Thread 1: mg_check_access(ptr, 4, 1) -> SUCCESS (Node is active)
Thread 2: free(ptr)                  -> SUCCESS (Node unlinked from AVL)
Thread 1: *ptr = 42;                 -> STORE EXECUTED (Use-After-Free at hardware level)
```
If Thread 2 frees a buffer immediately after Thread 1 executes `mg_check_access()` but before Thread 1's CPU executes the `str` / `mov` assembly instruction, a hardware-level race occurs. Mitigating this would require full lock-synchronized memory loads and stores, which incurs unacceptable performance degradation.

### 3.6 Memory Overhead of the Allocation Registry
For every active heap allocation, the runtime allocates an internal AVL tree node (`mg_alloc_node_t`):
- Each node consumes approximately 48 bytes on 64-bit architectures (base pointer, size, active flag, ownership flag, subtree pointers, height).
- For programs with millions of tiny allocations (e.g. 4-byte nodes), metadata overhead can exceed the size of the payload itself.

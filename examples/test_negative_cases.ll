; ModuleID = 'test_negative_cases.ll'
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

; External standard C allocators
declare noalias ptr @malloc(i64 noundef)
declare noalias ptr @calloc(i64 noundef, i64 noundef)
declare noalias ptr @realloc(ptr noundef, i64 noundef)
declare void @free(ptr noundef)
declare void @exit(i32 noundef)

; Incompatible / variadic allocator declarations
declare ptr @variadic_malloc(i64 noundef, ...)
declare void @variadic_free(ptr noundef, ...)

; Global variable for unknown memory load tests
@external_unknown_ptr = external global ptr, align 8
@global_table = internal global [4 x i32] [i32 10, i32 20, i32 30, i32 40], align 16

; ============================================================================
; 1. Unsupported Pointer Transformations
; ============================================================================

; 1a. IntToPtr and PtrToInt round-trip
define dso_local void @test_neg_inttoptr_ptrtoint() {
  %p = call ptr @malloc(i64 32)
  %intval = ptrtoint ptr %p to i64
  %recast = inttoptr i64 %intval to ptr
  ; MUST remain uninstrumented (origin UNKNOWN)
  store i32 100, ptr %recast, align 4
  %v = load i32, ptr %recast, align 4
  call void @free(ptr %p)
  ret void
}

; 1b. Unsupported address-space conversion (AddrSpaceCast)
define dso_local void @test_neg_addrspace_conversion() {
  %p = call ptr @malloc(i64 32)
  %as1 = addrspacecast ptr %p to ptr addrspace(1)
  ; Access in addrspace(1) MUST remain uninstrumented
  store i32 200, ptr addrspace(1) %as1, align 4
  %v = load i32, ptr addrspace(1) %as1, align 4
  call void @free(ptr %p)
  ret void
}

; 1c. Pointer values originating from unsupported constant expressions
define dso_local void @test_neg_unsupported_expr() {
  ; inttoptr constant expression
  %const_ptr = inttoptr i64 12345678 to ptr
  store i32 300, ptr %const_ptr, align 4
  %v = load i32, ptr %const_ptr, align 4
  ret void
}

; 1d. Pointer loaded from unknown memory (external global)
define dso_local void @test_neg_load_from_unknown_memory() {
  %loaded_p = load ptr, ptr @external_unknown_ptr, align 8
  ; MUST remain uninstrumented because source memory is untracked
  store i32 400, ptr %loaded_p, align 4
  %v = load i32, ptr %loaded_p, align 4
  ret void
}

; ============================================================================
; 2. Ambiguous Pointer Origins
; ============================================================================

; 2a. PHI node combining heap, stack, and unknown pointers
define dso_local void @test_neg_phi_heap_stack_unknown(i32 %selector) {
entry:
  %stack_buf = alloca i32, align 4
  switch i32 %selector, label %sw_unknown [
    i32 0, label %sw_heap
    i32 1, label %sw_stack
  ]

sw_heap:
  %hp = call ptr @malloc(i64 32)
  br label %merge

sw_stack:
  br label %merge

sw_unknown:
  %unk = inttoptr i64 888888 to ptr
  br label %merge

merge:
  %phi_p = phi ptr [ %hp, %sw_heap ], [ %stack_buf, %sw_stack ], [ %unk, %sw_unknown ]
  ; 3-way conflicting PHI: MUST remain uninstrumented
  store i32 500, ptr %phi_p, align 4
  %v = load i32, ptr %phi_p, align 4
  ret void
}

; 2b. Select instruction with conflicting origins (heap vs stack)
define dso_local void @test_neg_select_conflicting(i1 %cond) {
  %hp = call ptr @malloc(i64 32)
  %stack_buf = alloca i32, align 4
  %sel = select i1 %cond, ptr %hp, ptr %stack_buf
  ; Conflicting select arms: MUST remain uninstrumented
  store i32 600, ptr %sel, align 4
  %v = load i32, ptr %sel, align 4
  call void @free(ptr %hp)
  ret void
}

; 2c. Multiple call sites with conflicting pointer origins
define internal void @internal_callee_conflicting(ptr %arg_p) {
  ; Callee has mixed call sites: MUST remain uninstrumented
  store i32 700, ptr %arg_p, align 4
  %v = load i32, ptr %arg_p, align 4
  ret void
}

define dso_local void @test_neg_multi_callsite_conflicting() {
  %hp = call ptr @malloc(i64 32)
  %stack_buf = alloca i32, align 4
  ; Call site 1: passes heap
  call void @internal_callee_conflicting(ptr %hp)
  ; Call site 2: passes stack
  call void @internal_callee_conflicting(ptr %stack_buf)
  call void @free(ptr %hp)
  ret void
}

; 2d. Function parameter with unresolved / external callers
define dso_local void @test_neg_unresolved_external_caller(ptr %external_arg) {
  ; Non-static function: callers unknown -> MUST remain uninstrumented
  store i32 800, ptr %external_arg, align 4
  %v = load i32, ptr %external_arg, align 4
  ret void
}

; 2e. Function parameter with address-taken (indirect) caller
define internal void @internal_callee_addr_taken(ptr %p) {
  ; Address taken: could be called indirectly -> MUST remain uninstrumented
  store i32 900, ptr %p, align 4
  %v = load i32, ptr %p, align 4
  ret void
}

@fn_ptr_storage = internal global ptr @internal_callee_addr_taken, align 8

define dso_local void @test_neg_indirect_caller_addr_taken() {
  %hp = call ptr @malloc(i64 32)
  %fp = load ptr, ptr @fn_ptr_storage, align 8
  call void %fp(ptr %hp)
  call void @free(ptr %hp)
  ret void
}

; ============================================================================
; 3. Unsupported Call Patterns
; ============================================================================

; 3a. Indirect calls to allocation functions
@indirect_malloc_storage = internal global ptr @malloc, align 8
@indirect_free_storage = internal global ptr @free, align 8

define dso_local void @test_neg_indirect_alloc_call() {
  %fn_malloc = load ptr, ptr @indirect_malloc_storage, align 8
  ; Indirect call: MUST NOT be rewritten to mg_intercept_malloc
  %p = call ptr %fn_malloc(i64 32)
  ; Access through indirect alloc: MUST NOT receive mg_check_access
  store i32 111, ptr %p, align 4
  %v = load i32, ptr %p, align 4
  %fn_free = load ptr, ptr @indirect_free_storage, align 8
  ; Indirect free: MUST NOT be rewritten to mg_intercept_free
  call void %fn_free(ptr %p)
  ret void
}

; 3b. Variadic allocator declarations
define dso_local void @test_neg_variadic_allocator() {
  ; Variadic call: MUST NOT be rewritten to mg_intercept_malloc
  %p = call ptr (i64, ...) @variadic_malloc(i64 32, i32 99)
  ; Access MUST NOT receive mg_check_access
  store i32 222, ptr %p, align 4
  %v = load i32, ptr %p, align 4
  call void (ptr, ...) @variadic_free(ptr %p, i32 1)
  ret void
}

; 3c. Non-C calling conventions
define dso_local void @test_neg_non_c_calling_convention() {
  ; fastcc: MUST NOT be rewritten
  %p = call fastcc ptr @malloc(i64 32)
  ; Access MUST NOT receive mg_check_access
  store i32 333, ptr %p, align 4
  %v = load i32, ptr %p, align 4
  call fastcc void @free(ptr %p)
  ret void
}

; ============================================================================
; 4. Unsupported Memory Operations
; ============================================================================

; 4a. Atomic loads and stores
define dso_local void @test_neg_atomic_load_store() {
  %p = call ptr @malloc(i64 32)
  ; Atomic write and read: MUST remain unchanged without mg_check_access
  store atomic i32 444, ptr %p release, align 4
  %v = load atomic i32, ptr %p acquire, align 4
  call void @free(ptr %p)
  ret void
}

; 4b. Atomic RMW and compare-exchange
define dso_local void @test_neg_atomic_rmw_cmpxchg() {
  %p = call ptr @malloc(i64 32)
  ; Atomic RMW: MUST remain unchanged without mg_check_access
  %old = atomicrmw add ptr %p, i32 10 seq_cst, align 4
  ; Atomic cmpxchg: MUST remain unchanged without mg_check_access
  %res = cmpxchg ptr %p, i32 10, i32 20 seq_cst seq_cst, align 4
  call void @free(ptr %p)
  ret void
}

; 4c. Atomic fences
define dso_local void @test_neg_atomic_fence() {
  ; Atomic fence has no pointer: MUST remain untouched
  fence seq_cst
  fence release
  fence acquire
  ret void
}

; 4d. Scalable vector accesses
define dso_local void @test_neg_scalable_vector() {
  %p = call ptr @malloc(i64 64)
  ; Scalable vector: MUST NOT receive mg_check_access
  %v = load <vscale x 4 x i32>, ptr %p, align 16
  store <vscale x 4 x i32> %v, ptr %p, align 16
  call void @free(ptr %p)
  ret void
}

; 4e. Non-default address-space accesses
define dso_local void @test_neg_non_default_addrspace(ptr addrspace(1) %p_as1) {
  ; Non-zero address space: MUST NOT receive mg_check_access
  store i32 555, ptr addrspace(1) %p_as1, align 4
  %v = load i32, ptr addrspace(1) %p_as1, align 4
  ret void
}

; ============================================================================
; 5. Malformed or Unusual IR Handling
; ============================================================================

; 5a. Empty basic blocks and unusual branch topologies
define dso_local void @test_neg_unusual_empty_blocks(i1 %c1, i1 %c2) {
entry:
  br i1 %c1, label %bb1, label %bb2

bb1:
  br label %merge

bb2:
  br i1 %c2, label %bb3, label %merge

bb3:
  br label %merge

merge:
  ret void
}

; 5b. Unreachable blocks
define dso_local void @test_neg_unusual_unreachable() {
entry:
  ret void

dead:
  %p = call ptr @malloc(i64 32)
  store i32 999, ptr %p, align 4
  unreachable
}

; 5c. Inline assembly
define dso_local void @test_neg_unusual_inline_asm() {
  call void asm sideeffect "nop", ""()
  ret void
}

; ============================================================================
; 6. Positive Baseline (Guarantees Pass Is Functional and Non-Trivial)
; ============================================================================

; Standard compatible heap access: MUST be rewritten to mg_intercept_malloc
; and MUST receive EXACTLY 2 calls to mg_check_access (for store and load)
define dso_local void @test_pos_supported_heap_access() {
  %p = call ptr @malloc(i64 32)
  store i32 777, ptr %p, align 4
  %v = load i32, ptr %p, align 4
  call void @free(ptr %p)
  ret void
}

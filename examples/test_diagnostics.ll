; ModuleID = 'test_diagnostics.ll'
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

; External standard C allocators
declare noalias ptr @malloc(i64 noundef)
declare noalias ptr @calloc(i64 noundef, i64 noundef)
declare noalias ptr @realloc(ptr noundef, i64 noundef)
declare void @free(ptr noundef)

; Incompatible variadic allocator declarations
declare ptr @variadic_malloc(i64 noundef, ...)
declare void @variadic_free(ptr noundef, ...)

; Global memory
@global_var = internal global [4 x i32] [i32 1, i32 2, i32 3, i32 4], align 16

; ----------------------------------------------------------------------------
; Function 1: Heap and stack accesses
; 4 accesses examined (2 stack non-heap, 2 heap candidate)
; 2 checks inserted, 2 allocators rewritten (malloc, free)
; ----------------------------------------------------------------------------
define dso_local void @fn_heap_and_stack() {
entry:
  %stack_var = alloca i32, align 4
  %hp = call ptr @malloc(i64 32)
  ; Non-heap accesses
  store i32 10, ptr %stack_var, align 4
  %v1 = load i32, ptr %stack_var, align 4
  ; Heap candidate accesses
  store i32 %v1, ptr %hp, align 4
  %v2 = load i32, ptr %hp, align 4
  call void @free(ptr %hp)
  ret void
}

; ----------------------------------------------------------------------------
; Function 2: Volatile heap accesses and unknown-origin accesses
; 4 accesses examined (2 volatile heap, 2 unknown origin)
; 2 checks inserted (2 volatile), 2 allocators rewritten (calloc, free)
; ----------------------------------------------------------------------------
define dso_local void @fn_volatile_and_unknown() {
entry:
  %cp = call ptr @calloc(i64 4, i64 8)
  ; Volatile heap candidate accesses
  store volatile i64 42, ptr %cp, align 8
  %v1 = load volatile i64, ptr %cp, align 8
  ; Unknown pointer via inttoptr
  %intval = ptrtoint ptr %cp to i64
  %recast = inttoptr i64 %intval to ptr
  store i64 %v1, ptr %recast, align 8
  %v2 = load i64, ptr %recast, align 8
  call void @free(ptr %cp)
  ret void
}

; ----------------------------------------------------------------------------
; Function 3: Unsupported operations
; 6 accesses examined (2 atomic load/store, 2 scalable vector, 2 addrspace)
; 0 checks inserted
; 5 atomic ops (store, load, rmw, cmpxchg, fence)
; 2 scalable vector, 2 addrspace, 2 incompatible allocators skipped
; Total unsupported = 11
; ----------------------------------------------------------------------------
define dso_local void @fn_unsupported_ops(ptr %p, ptr %vec_p, ptr addrspace(1) %as1_p) {
entry:
  ; Incompatible allocator call 1
  %bad_alloc = call ptr (i64, ...) @variadic_malloc(i64 32, i32 99)
  ; Atomic store and load
  store atomic i32 100, ptr %p release, align 4
  %a_val = load atomic i32, ptr %p acquire, align 4
  ; Atomic RMW and CmpXchg
  %rmw_val = atomicrmw add ptr %p, i32 1 seq_cst
  %cmpx_val = cmpxchg ptr %p, i32 1, i32 2 seq_cst seq_cst
  ; Atomic fence
  fence seq_cst
  ; Scalable vector accesses
  store <vscale x 4 x i32> zeroinitializer, ptr %vec_p, align 16
  %vec_val = load <vscale x 4 x i32>, ptr %vec_p, align 16
  ; Non-default address-space accesses
  store i32 200, ptr addrspace(1) %as1_p, align 4
  %as1_val = load i32, ptr addrspace(1) %as1_p, align 4
  ; Incompatible allocator call 2
  call void (ptr, ...) @variadic_free(ptr %p, i32 0)
  ret void
}

; ----------------------------------------------------------------------------
; Function 4: Realloc and global memory
; 4 accesses examined (2 global non-heap, 2 heap candidate)
; 2 checks inserted, 3 allocators rewritten (malloc, realloc, free)
; ----------------------------------------------------------------------------
define dso_local void @fn_realloc_and_globals() {
entry:
  ; Non-heap accesses to global array
  store i32 50, ptr @global_var, align 4
  %g_val = load i32, ptr @global_var, align 4
  ; Heap allocation and reallocation
  %p1 = call ptr @malloc(i64 16)
  %p2 = call ptr @realloc(ptr %p1, i64 64)
  ; Heap candidate accesses
  store i32 %g_val, ptr %p2, align 4
  %h_val = load i32, ptr %p2, align 4
  call void @free(ptr %p2)
  ret void
}

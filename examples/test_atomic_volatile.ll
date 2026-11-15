; ModuleID = 'test_atomic_volatile.ll'
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

declare noalias ptr @malloc(i64 noundef)
declare void @free(ptr noundef)
declare void @exit(i32 noundef)

; --------------------------------------------------------------------------
; 1. Heap-origin volatile read and write: valid access
; --------------------------------------------------------------------------
define dso_local void @mg_test_volatile_heap_valid() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  store volatile i32 1234, ptr %p, align 4
  %v0 = load volatile i32, ptr %p, align 4
  %cmp0 = icmp eq i32 %v0, 1234
  br i1 %cmp0, label %step2, label %err

step2:
  %gep7 = getelementptr inbounds i32, ptr %p, i64 7
  store volatile i32 5678, ptr %gep7, align 4
  %v7 = load volatile i32, ptr %gep7, align 4
  %cmp7 = icmp eq i32 %v7, 5678
  br i1 %cmp7, label %done, label %err

done:
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 2. Heap-origin volatile write: out-of-bounds access (MUST abort with SIGABRT)
; --------------------------------------------------------------------------
define dso_local void @mg_test_volatile_heap_oob() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  %gep8 = getelementptr inbounds i32, ptr %p, i64 8
  store volatile i32 999, ptr %gep8, align 4
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 3. Atomic load and store (heap origin): skipped for synchronization preservation
; --------------------------------------------------------------------------
define dso_local void @mg_test_atomic_load_store() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  store atomic i32 42, ptr %p release, align 4
  %v = load atomic i32, ptr %p acquire, align 4
  %cmp_val = icmp eq i32 %v, 42
  br i1 %cmp_val, label %done, label %err

done:
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 4. Atomic RMW (heap origin): skipped for synchronization preservation
; --------------------------------------------------------------------------
define dso_local void @mg_test_atomic_rmw() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  store i32 10, ptr %p, align 4
  %old = atomicrmw add ptr %p, i32 5 seq_cst, align 4
  %cmp_old = icmp eq i32 %old, 10
  br i1 %cmp_old, label %step2, label %err

step2:
  %cur = load i32, ptr %p, align 4
  %cmp_cur = icmp eq i32 %cur, 15
  br i1 %cmp_cur, label %done, label %err

done:
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 5. Atomic compare-exchange (heap origin): skipped for synchronization preservation
; --------------------------------------------------------------------------
define dso_local void @mg_test_atomic_cmpxchg() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  store i32 100, ptr %p, align 4
  %res = cmpxchg ptr %p, i32 100, i32 200 seq_cst monotonic, align 4
  %val = extractvalue { i32, i1 } %res, 0
  %ok_flag = extractvalue { i32, i1 } %res, 1
  %cmp_val = icmp eq i32 %val, 100
  br i1 %cmp_val, label %step2, label %err

step2:
  br i1 %ok_flag, label %step3, label %err

step3:
  %cur = load i32, ptr %p, align 4
  %cmp_cur = icmp eq i32 %cur, 200
  br i1 %cmp_cur, label %done, label %err

done:
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 6. Atomic fence: skipped explicitly (no pointer operand)
; --------------------------------------------------------------------------
define dso_local void @mg_test_atomic_fence() {
  fence seq_cst
  fence release
  fence acquire
  fence acq_rel
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 7. Unknown-origin atomic pointer: skipped for synchronization preservation
; --------------------------------------------------------------------------
define internal void @callee_atomic_unknown(ptr %unknown_p) {
  store atomic i32 77, ptr %unknown_p release, align 4
  %v = load atomic i32, ptr %unknown_p acquire, align 4
  %old = atomicrmw add ptr %unknown_p, i32 1 seq_cst, align 4
  ret void
}

define dso_local void @mg_test_atomic_unknown_origin() {
  %stack_buf = alloca i32, align 4
  store i32 0, ptr %stack_buf, align 4
  call void @callee_atomic_unknown(ptr %stack_buf)
  %final_val = load i32, ptr %stack_buf, align 4
  %cmp = icmp eq i32 %final_val, 78
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  call void @exit(i32 noundef 0)
  unreachable
}

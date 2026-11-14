; ModuleID = 'test_aggregates.ll'
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

declare noalias ptr @malloc(i64 noundef)
declare void @free(ptr noundef)
declare void @exit(i32 noundef)

; --------------------------------------------------------------------------
; 1. Struct insertion and extraction: valid and OOB
; --------------------------------------------------------------------------
define dso_local void @mg_test_aggregate_struct_valid() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  %agg0 = insertvalue { ptr, i32 } undef, ptr %p, 0
  %agg1 = insertvalue { ptr, i32 } %agg0, i32 42, 1
  %extracted = extractvalue { ptr, i32 } %agg1, 0
  store i32 111, ptr %extracted, align 4
  %gep7 = getelementptr inbounds i32, ptr %extracted, i64 7
  store i32 777, ptr %gep7, align 4
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

define dso_local void @mg_test_aggregate_struct_oob() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  %agg0 = insertvalue { ptr, i32 } undef, ptr %p, 0
  %agg1 = insertvalue { ptr, i32 } %agg0, i32 42, 1
  %extracted = extractvalue { ptr, i32 } %agg1, 0
  %gep8 = getelementptr inbounds i32, ptr %extracted, i64 8
  store i32 999, ptr %gep8, align 4
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 2. Nested aggregate: struct containing struct
; --------------------------------------------------------------------------
define dso_local void @mg_test_aggregate_nested_valid() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  %inner = insertvalue { ptr } undef, ptr %p, 0
  %nested = insertvalue { { ptr }, i32 } undef, { ptr } %inner, 0
  %extracted = extractvalue { { ptr }, i32 } %nested, 0, 0
  store i32 333, ptr %extracted, align 4
  %gep7 = getelementptr inbounds i32, ptr %extracted, i64 7
  store i32 444, ptr %gep7, align 4
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

define dso_local void @mg_test_aggregate_nested_oob() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  %inner = insertvalue { ptr } undef, ptr %p, 0
  %nested = insertvalue { { ptr }, i32 } undef, { ptr } %inner, 0
  %extracted = extractvalue { { ptr }, i32 } %nested, 0, 0
  %gep10 = getelementptr inbounds i32, ptr %extracted, i64 10
  store i32 999, ptr %gep10, align 4
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 3. Nested aggregate: struct containing array of pointers
; --------------------------------------------------------------------------
define dso_local void @mg_test_aggregate_array_valid() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  %agg0 = insertvalue { [2 x ptr], i32 } undef, ptr %p, 0, 1
  %extracted = extractvalue { [2 x ptr], i32 } %agg0, 0, 1
  store i32 555, ptr %extracted, align 4
  %gep7 = getelementptr inbounds i32, ptr %extracted, i64 7
  store i32 666, ptr %gep7, align 4
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

define dso_local void @mg_test_aggregate_array_oob() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  %agg0 = insertvalue { [2 x ptr], i32 } undef, ptr %p, 0, 1
  %extracted = extractvalue { [2 x ptr], i32 } %agg0, 0, 1
  %gep9 = getelementptr inbounds i32, ptr %extracted, i64 9
  store i32 999, ptr %gep9, align 4
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 4. Disjoint index propagation
; --------------------------------------------------------------------------
define dso_local void @mg_test_aggregate_disjoint() {
  %heap_p = call noalias ptr @malloc(i64 noundef 32)
  %stack_var = alloca i32, align 4
  store i32 10, ptr %stack_var, align 4

  ; Insert heap into field 0, stack into field 1
  %agg0 = insertvalue { ptr, ptr } undef, ptr %heap_p, 0
  %agg1 = insertvalue { ptr, ptr } %agg0, ptr %stack_var, 1

  ; Extract field 0 (disjoint from field 1 insert, follows to agg0) -> HEAP_CANDIDATE
  %ext0 = extractvalue { ptr, ptr } %agg1, 0
  store i32 123, ptr %ext0, align 4

  ; Extract field 1 -> NON_HEAP (stack)
  %ext1 = extractvalue { ptr, ptr } %agg1, 1
  store i32 456, ptr %ext1, align 4

  call void @free(ptr noundef %heap_p)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 5. Stack pointer in aggregate remains NON_HEAP (uninstrumented)
; --------------------------------------------------------------------------
define dso_local void @mg_test_aggregate_stack_non_heap() {
  %stack_var = alloca i32, align 4
  store i32 0, ptr %stack_var, align 4
  %agg0 = insertvalue { ptr, i32 } undef, ptr %stack_var, 0
  %extracted = extractvalue { ptr, i32 } %agg0, 0
  store i32 100, ptr %extracted, align 4
  %val = load i32, ptr %stack_var, align 4
  %cmp = icmp eq i32 %val, 100
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 6. Interprocedural aggregate argument: all callers heap -> HEAP_CANDIDATE
; --------------------------------------------------------------------------
define internal void @callee_heap_agg({ ptr, i32 } %agg) {
  %p = extractvalue { ptr, i32 } %agg, 0
  store i32 777, ptr %p, align 4
  ret void
}

define internal void @callee_heap_agg_oob({ ptr, i32 } %agg) {
  %p = extractvalue { ptr, i32 } %agg, 0
  %gep8 = getelementptr inbounds i32, ptr %p, i64 8
  store i32 999, ptr %gep8, align 4
  ret void
}

define dso_local void @mg_test_aggregate_interproc_valid() {
  %heap_buf = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %heap_buf, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  %agg = insertvalue { ptr, i32 } undef, ptr %heap_buf, 0
  call void @callee_heap_agg({ ptr, i32 } %agg)
  call void @free(ptr noundef %heap_buf)
  call void @exit(i32 noundef 0)
  unreachable
}

define dso_local void @mg_test_aggregate_interproc_oob() {
  %heap_buf = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %heap_buf, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  %agg = insertvalue { ptr, i32 } undef, ptr %heap_buf, 0
  call void @callee_heap_agg_oob({ ptr, i32 } %agg)
  call void @free(ptr noundef %heap_buf)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 7. Interprocedural aggregate argument: mixed callers remain UNKNOWN
; --------------------------------------------------------------------------
define internal void @callee_mixed_agg({ ptr, i32 } %agg) {
  %p = extractvalue { ptr, i32 } %agg, 0
  store i32 888, ptr %p, align 4
  ret void
}

define dso_local void @mg_test_aggregate_mixed() {
  ; Caller 1: heap pointer passed in aggregate
  %heap_buf = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %heap_buf, null
  br i1 %cmp, label %call1, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

call1:
  %agg1 = insertvalue { ptr, i32 } undef, ptr %heap_buf, 0
  call void @callee_mixed_agg({ ptr, i32 } %agg1)
  call void @free(ptr noundef %heap_buf)

  ; Caller 2: stack pointer passed in aggregate
  %stack_buf = alloca i32, align 4
  store i32 0, ptr %stack_buf, align 4
  %agg2 = insertvalue { ptr, i32 } undef, ptr %stack_buf, 0
  call void @callee_mixed_agg({ ptr, i32 } %agg2)
  %val = load i32, ptr %stack_buf, align 4
  %cmp2 = icmp eq i32 %val, 888
  br i1 %cmp2, label %ok, label %err

ok:
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 8. Pointer-preserving casts and constant expression propagation
; --------------------------------------------------------------------------
define dso_local void @mg_test_pointer_cast_valid() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  store i32 1234, ptr %p, align 4
  %gep7 = getelementptr inbounds i32, ptr %p, i64 7
  store i32 5678, ptr %gep7, align 4
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

define dso_local void @mg_test_pointer_cast_oob() {
  %p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  %gep8 = getelementptr inbounds i32, ptr %p, i64 8
  store i32 9999, ptr %gep8, align 4
  call void @free(ptr noundef %p)
  call void @exit(i32 noundef 0)
  unreachable
}

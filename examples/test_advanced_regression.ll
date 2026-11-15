; ModuleID = 'test_advanced_regression.ll'
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

declare noalias ptr @malloc(i64 noundef)
declare void @free(ptr noundef)
declare void @exit(i32 noundef)

; --------------------------------------------------------------------------
; 1. Select instruction with matching heap origins: valid access
; --------------------------------------------------------------------------
define dso_local void @mg_test_select_matching_heap_valid() {
  %p1 = call noalias ptr @malloc(i64 noundef 32)
  %p2 = call noalias ptr @malloc(i64 noundef 64)
  %cmp1 = icmp ne ptr %p1, null
  %cmp2 = icmp ne ptr %p2, null
  %both_ok = and i1 %cmp1, %cmp2
  br i1 %both_ok, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  ; Select between two heap candidate pointers
  %sel = select i1 true, ptr %p1, ptr %p2
  store i32 111, ptr %sel, align 4
  %gep7 = getelementptr inbounds i32, ptr %sel, i64 7
  store i32 222, ptr %gep7, align 4
  call void @free(ptr noundef %p1)
  call void @free(ptr noundef %p2)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 2. Select instruction with matching heap origins: out-of-bounds access (abort)
; --------------------------------------------------------------------------
define dso_local void @mg_test_select_matching_heap_oob() {
  %p1 = call noalias ptr @malloc(i64 noundef 32)
  %p2 = call noalias ptr @malloc(i64 noundef 64)
  %cmp1 = icmp ne ptr %p1, null
  %cmp2 = icmp ne ptr %p2, null
  %both_ok = and i1 %cmp1, %cmp2
  br i1 %both_ok, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  ; Select p1 (size 32 = 8 ints). Access index 8 (offset 32) must abort.
  %sel = select i1 true, ptr %p1, ptr %p2
  %gep8 = getelementptr inbounds i32, ptr %sel, i64 8
  store i32 999, ptr %gep8, align 4
  call void @free(ptr noundef %p1)
  call void @free(ptr noundef %p2)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 3. Select instruction with conflicting origins (heap vs stack): UNINSTRUMENTED
; --------------------------------------------------------------------------
define dso_local void @mg_test_select_conflicting_heap_stack() {
  %heap_p = call noalias ptr @malloc(i64 noundef 32)
  %stack_var = alloca i32, align 4
  %cmp = icmp ne ptr %heap_p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  ; Select between heap and non-heap (stack) -> must be UNKNOWN and uninstrumented
  %sel = select i1 false, ptr %heap_p, ptr %stack_var
  store i32 777, ptr %sel, align 4
  %val = load i32, ptr %stack_var, align 4
  %cmp_val = icmp eq i32 %val, 777
  call void @free(ptr noundef %heap_p)
  br i1 %cmp_val, label %done, label %err

done:
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 4. PHI node with matching heap origins: valid access
; --------------------------------------------------------------------------
define dso_local void @mg_test_phi_matching_heap_valid() {
entry:
  br i1 true, label %left, label %right

left:
  %p1 = call noalias ptr @malloc(i64 noundef 32)
  br label %merge

right:
  %p2 = call noalias ptr @malloc(i64 noundef 64)
  br label %merge

merge:
  %phi_p = phi ptr [ %p1, %left ], [ %p2, %right ]
  %cmp = icmp ne ptr %phi_p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  store i32 333, ptr %phi_p, align 4
  %gep7 = getelementptr inbounds i32, ptr %phi_p, i64 7
  store i32 444, ptr %gep7, align 4
  call void @free(ptr noundef %phi_p)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 5. PHI node with matching heap origins: out-of-bounds access (abort)
; --------------------------------------------------------------------------
define dso_local void @mg_test_phi_matching_heap_oob() {
entry:
  br i1 true, label %left, label %right

left:
  %p1 = call noalias ptr @malloc(i64 noundef 32)
  br label %merge

right:
  %p2 = call noalias ptr @malloc(i64 noundef 64)
  br label %merge

merge:
  %phi_p = phi ptr [ %p1, %left ], [ %p2, %right ]
  %cmp = icmp ne ptr %phi_p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  ; Index 8 on 32-byte allocation must abort!
  %gep8 = getelementptr inbounds i32, ptr %phi_p, i64 8
  store i32 999, ptr %gep8, align 4
  call void @free(ptr noundef %phi_p)
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 6. PHI node with conflicting origins (heap vs stack): UNINSTRUMENTED
; --------------------------------------------------------------------------
define dso_local void @mg_test_phi_conflicting_heap_stack() {
entry:
  %stack_val = alloca i32, align 4
  store i32 0, ptr %stack_val, align 4
  br i1 false, label %left, label %right

left:
  %heap_p = call noalias ptr @malloc(i64 noundef 32)
  br label %merge

right:
  br label %merge

merge:
  %phi_p = phi ptr [ %heap_p, %left ], [ %stack_val, %right ]
  ; Conflicting origins -> UNKNOWN -> uninstrumented
  store i32 555, ptr %phi_p, align 4
  %loaded = load i32, ptr %stack_val, align 4
  %cmp = icmp eq i32 %loaded, 555
  br i1 %cmp, label %done, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

done:
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 7. PHI node with unknown origin branch: UNINSTRUMENTED
; --------------------------------------------------------------------------
define dso_local void @mg_test_phi_unknown_origin_branch() {
entry:
  %dummy_stack = alloca i32, align 4
  br i1 true, label %left, label %right

left:
  ; IntToPtr is an unsupported cast -> origin UNKNOWN
  %int_val = ptrtoint ptr %dummy_stack to i64
  %int_ptr = inttoptr i64 %int_val to ptr
  br label %merge

right:
  %heap_p = call noalias ptr @malloc(i64 noundef 32)
  br label %merge

merge:
  ; One incoming is UNKNOWN -> phi must be UNKNOWN and uninstrumented
  %phi_p = phi ptr [ %int_ptr, %left ], [ %heap_p, %right ]
  store i32 888, ptr %phi_p, align 4
  %res = load i32, ptr %dummy_stack, align 4
  %cmp = icmp eq i32 %res, 888
  br i1 %cmp, label %done, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

done:
  call void @exit(i32 noundef 0)
  unreachable
}

; --------------------------------------------------------------------------
; 8. Unsupported pointer transformation (PtrToInt / IntToPtr): UNINSTRUMENTED
; --------------------------------------------------------------------------
define dso_local void @mg_test_inttoptr_unsupported_cast() {
  %heap_p = call noalias ptr @malloc(i64 noundef 32)
  %cmp = icmp ne ptr %heap_p, null
  br i1 %cmp, label %ok, label %err

err:
  call void @exit(i32 noundef 1)
  unreachable

ok:
  ; Convert heap pointer to integer and back to pointer
  %as_int = ptrtoint ptr %heap_p to i64
  %recast_p = inttoptr i64 %as_int to ptr
  ; Must remain UNKNOWN and uninstrumented
  store i32 999, ptr %recast_p, align 4
  %val = load i32, ptr %recast_p, align 4
  %cmp_val = icmp eq i32 %val, 999
  call void @free(ptr noundef %heap_p)
  br i1 %cmp_val, label %done, label %err

done:
  call void @exit(i32 noundef 0)
  unreachable
}

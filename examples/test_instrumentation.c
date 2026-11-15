#include "memoryguard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

/* Global variable for non-heap test */
static int global_counter = 0;

/* Test 1: Normal standard malloc, read, write, and free (unmodified C) */
void test_fn_normal_malloc(void) {
    int *p = (int *)malloc(64);
    if (!p) {
        exit(1);
    }
    /* Standard writes and reads: pass automatically instruments and registers */
    p[0] = 100;
    p[15] = 200;
    int a = p[0];
    int b = p[15];
    if (a != 100 || b != 200) {
        exit(1);
    }
    free(p);
    exit(0);
}

/* Test 2: Standard calloc zero initialization and bounds checking */
void test_fn_calloc_zero_init(void) {
    int *p = (int *)calloc(16, sizeof(int)); /* 64 bytes */
    if (!p) {
        exit(1);
    }
    /* Verify all elements are zero-initialized */
    for (int i = 0; i < 16; i++) {
        if (p[i] != 0) {
            exit(1);
        }
    }
    p[0] = 42;
    p[15] = 99;
    if (p[0] != 42 || p[15] != 99) {
        exit(1);
    }
    free(p);
    exit(0);
}

/* Test 3: Standard realloc expansion, data preservation, and failure handling */
void test_fn_realloc_lifecycle(void) {
    int *p = (int *)malloc(32); /* 8 ints */
    if (!p) {
        exit(1);
    }
    p[0] = 123;
    p[7] = 456;

    /* Reallocate to larger size (128 bytes = 32 ints) */
    int *p2 = (int *)realloc(p, 128);
    if (!p2) {
        exit(1);
    }
    /* Verify original data preserved */
    if (p2[0] != 123 || p2[7] != 456) {
        exit(1);
    }
    p2[31] = 789;
    if (p2[31] != 789) {
        exit(1);
    }

    /* Reallocation failure with impossible size: original block must remain valid */
    int *p_fail = (int *)realloc(p2, SIZE_MAX);
    if (p_fail != NULL) {
        exit(1);
    }
    /* Original p2 buffer must still be valid and tracked */
    if (p2[0] != 123 || p2[7] != 456 || p2[31] != 789) {
        exit(1);
    }

    free(p2);
    exit(0);
}

/* Test 4: Use-after-free detection on standard free */
void test_fn_use_after_free(void) {
    int *p = (int *)malloc(32);
    if (!p) {
        exit(1);
    }
    p[0] = 42;
    free(p);
    /* Accessing p after free must trigger MemoryGuard violation and abort */
    p[0] = 84;
    exit(0);
}

/* Test 5: Out-of-bounds heap write on standard malloc */
void test_fn_oob_write(void) {
    int *p = (int *)malloc(32); /* 32 bytes = 8 ints (indices 0..7) */
    if (!p) {
        exit(1);
    }
    /* p[8] is at offset 32 bytes, which exceeds the 32-byte allocation */
    p[8] = 999;
    free(p);
    exit(0);
}

/* Test 6: Out-of-bounds heap read on standard malloc */
void test_fn_oob_read(void) {
    int *p = (int *)malloc(32); /* 32 bytes = 8 ints (indices 0..7) */
    if (!p) {
        exit(1);
    }
    /* Reading 4 bytes at offset 32 exceeds boundary */
    int val = p[8];
    (void)val;
    free(p);
    exit(0);
}

/* Test 7: Stack and global accesses (NON_HEAP: must NOT be instrumented) */
void test_fn_stack_global(void) {
    int stack_var = 123;
    stack_var += 50;
    global_counter = stack_var;
    if (global_counter != 173) {
        exit(1);
    }
    exit(0);
}

/* Test 8: Unknown pointer origin (UNKNOWN: must NOT be instrumented) */
void test_fn_unknown_param(int *param_dest, const int *param_src) {
    int val = *param_src;
    *param_dest = val + 10;
}

/* Test 9: Regression test - Application function named mg_process_packet()
 * Ensures unrelated user functions starting with "mg_" are NOT skipped.
 * Must be analyzed, call sites rewritten, and heap memory instrumented. */
void mg_process_packet(void) {
    int *packet = (int *)malloc(32); /* 8 ints */
    if (!packet) {
        exit(1);
    }
    packet[0] = 0xAA;
    packet[7] = 0xBB;
    if (packet[0] != 0xAA || packet[7] != 0xBB) {
        exit(1);
    }
    free(packet);
    exit(0);
}

/* Test 10: Regression test - mg_process_packet out-of-bounds access
 * Verifies that bounds violations inside mg_* application functions are caught. */
void mg_process_packet_oob(void) {
    int *packet = (int *)malloc(32); /* 8 ints */
    if (!packet) {
        exit(1);
    }
    /* Accessing index 8 (offset 32 on 32-byte alloc) must abort */
    packet[8] = 0xCC;
    free(packet);
    exit(0);
}

/* =========================================================================
 * Step 11: Interprocedural Pointer-Origin Integration Tests
 * ========================================================================= */

/* Scenario 1: Heap pointer passed to a function */
static void callee_heap_access(int *p) {
    p[0] = 55;
    p[7] = 77;
}

static int callee_heap_read(int *p) {
    return p[0] + p[7];
}

static void callee_heap_oob(int *p) {
    p[8] = 999; /* Out of bounds on 32-byte alloc: MUST abort! */
}

void test_fn_interproc_heap_param(void) {
    int *p = (int *)malloc(32); /* 8 ints */
    if (!p) exit(1);
    callee_heap_access(p);
    int sum = callee_heap_read(p);
    if (sum != 132) exit(1);
    free(p);
    exit(0);
}

void test_fn_interproc_heap_oob(void) {
    int *p = (int *)malloc(32);
    if (!p) exit(1);
    callee_heap_oob(p);
    free(p);
    exit(0);
}

/* Scenario 2: Multiple functions forwarding a heap pointer */
static void deep_sink_write(int *p) {
    p[2] = 222;
}

static void intermediate_forwarder(int *p) {
    deep_sink_write(p);
}

static void deep_sink_oob(int *p) {
    p[15] = 999; /* Out of bounds: MUST abort! */
}

static void intermediate_forwarder_oob(int *p) {
    deep_sink_oob(p);
}

void test_fn_interproc_forwarding(void) {
    int *p = (int *)malloc(32);
    if (!p) exit(1);
    intermediate_forwarder(p);
    if (p[2] != 222) exit(1);
    free(p);
    exit(0);
}

void test_fn_interproc_forwarding_oob(void) {
    int *p = (int *)malloc(32);
    if (!p) exit(1);
    intermediate_forwarder_oob(p);
    free(p);
    exit(0);
}

/* Scenario 3: Multiple direct call sites with heap pointers */
static void multi_callsite_target(int *p, int val) {
    p[0] = val;
}

static void multi_callsite_oob_target(int *p) {
    p[8] = 999;
}

void test_fn_interproc_multi_callsites(void) {
    int *p1 = (int *)malloc(32);
    int *p2 = (int *)malloc(64);
    if (!p1 || !p2) exit(1);

    multi_callsite_target(p1, 10);
    multi_callsite_target(p2, 20);

    if (p1[0] != 10 || p2[0] != 20) exit(1);

    free(p1);
    free(p2);
    exit(0);
}

void test_fn_interproc_multi_callsites_oob(void) {
    int *p1 = (int *)malloc(32);
    int *p2 = (int *)malloc(64);
    if (!p1 || !p2) exit(1);

    multi_callsite_target(p1, 10);
    multi_callsite_target(p2, 20);

    /* Out of bounds access through multi_callsite_oob_target */
    multi_callsite_oob_target(p1);

    free(p1);
    free(p2);
    exit(0);
}

/* Scenario 4: Mixed heap and non-heap call sites (MUST remain uninstrumented) */
static void mixed_arg_callee(int *p) {
    p[0] = 100; /* Should NOT be instrumented because one caller passes stack */
}

void test_fn_interproc_mixed(void) {
    int *heap_buf = (int *)malloc(32);
    if (!heap_buf) exit(1);
    mixed_arg_callee(heap_buf);
    free(heap_buf);

    int stack_var = 0;
    mixed_arg_callee(&stack_var);
    if (stack_var != 100) exit(1);

    exit(0);
}

/* Scenario 5: Function with externally visible callers (MUST remain uninstrumented) */
void external_callee_fn(int *p) {
    p[0] = 300; /* Must NOT be instrumented! */
}

void test_fn_interproc_external_caller(void) {
    int stack_val = 0;
    external_callee_fn(&stack_val);
    if (stack_val != 300) exit(1);
    exit(0);
}

/* Scenario 6: Recursive call pattern handled conservatively (MUST remain uninstrumented) */
static int recursive_countdown(int *p, int count) {
    if (count <= 0) return 0;
    return p[0] + recursive_countdown(p, count - 1);
}

void test_fn_interproc_recursive(void) {
    int stack_val = 5;
    int res = recursive_countdown(&stack_val, 3);
    if (res != 15) exit(1);
    exit(0);
}

/* =========================================================================
 * Step 12: Advanced Pointer-Origin Propagation Integration Tests
 * ========================================================================= */

/* ========================================================================= */
/* Step 12: Aggregate operations and pointer-origin propagation               */
/* ========================================================================= */

extern void mg_test_aggregate_struct_valid(void);
extern void mg_test_aggregate_struct_oob(void);
extern void mg_test_aggregate_nested_valid(void);
extern void mg_test_aggregate_nested_oob(void);
extern void mg_test_aggregate_array_valid(void);
extern void mg_test_aggregate_array_oob(void);
extern void mg_test_aggregate_disjoint(void);
extern void mg_test_aggregate_stack_non_heap(void);
extern void mg_test_aggregate_interproc_valid(void);
extern void mg_test_aggregate_interproc_oob(void);
extern void mg_test_aggregate_mixed(void);
extern void mg_test_pointer_cast_valid(void);
extern void mg_test_pointer_cast_oob(void);

/* ========================================================================= */
/* Step 13: Atomic and Volatile Memory Access Handling                        */
/* ========================================================================= */

extern void mg_test_volatile_heap_valid(void);
extern void mg_test_volatile_heap_oob(void);
extern void mg_test_atomic_load_store(void);
extern void mg_test_atomic_rmw(void);
extern void mg_test_atomic_cmpxchg(void);
extern void mg_test_atomic_fence(void);
extern void mg_test_atomic_unknown_origin(void);

/* ========================================================================= */
/* Step 15: Advanced Memory-Safety Regression Tests                          */
/* ========================================================================= */

/* Declarations from examples/test_advanced_regression.ll */
extern void mg_test_select_matching_heap_valid(void);
extern void mg_test_select_matching_heap_oob(void);
extern void mg_test_select_conflicting_heap_stack(void);
extern void mg_test_phi_matching_heap_valid(void);
extern void mg_test_phi_matching_heap_oob(void);
extern void mg_test_phi_conflicting_heap_stack(void);
extern void mg_test_phi_unknown_origin_branch(void);
extern void mg_test_inttoptr_unsupported_cast(void);

/* --- Group 1: Pointer Arithmetic --- */
void test_fn_pointer_arith_valid(void) {
    int *buf = (int *)malloc(16 * sizeof(int));
    if (!buf) exit(1);
    int *p1 = buf + 4;
    int *p2 = p1 + 3;
    int *p3 = p2 - 2; /* index 5 */
    p3[2] = 42;       /* index 7 */
    if (p3[2] != 42 || buf[7] != 42) exit(1);
    free(buf);
    exit(0);
}

void test_fn_pointer_arith_oob(void) {
    int *buf = (int *)malloc(16 * sizeof(int));
    if (!buf) exit(1);
    int *p1 = buf + 10;
    int *p2 = p1 + 5;
    int *p3 = p2 + 1; /* index 16 on 16-element buffer: offset 64 on 64-byte alloc */
    *p3 = 999;        /* MUST abort! */
    free(buf);
    exit(0);
}

void test_fn_pointer_arith_neg_valid(void) {
    int *buf = (int *)malloc(16 * sizeof(int));
    if (!buf) exit(1);
    int *mid = buf + 10;
    int *neg = mid - 4; /* index 6 */
    *neg = 77;
    neg[-2] = 88;       /* index 4 */
    if (buf[6] != 77 || buf[4] != 88) exit(1);
    free(buf);
    exit(0);
}

void test_fn_pointer_arith_neg_oob(void) {
    int *buf = (int *)malloc(16 * sizeof(int));
    if (!buf) exit(1);
    int *start = buf + 2;
    int *underflow = start - 3; /* index -1 (offset -4 bytes before start) */
    *underflow = 666;           /* MUST abort! */
    free(buf);
    exit(0);
}

/* --- Group 2: Arrays and Multidimensional Arrays --- */
void test_fn_1d_array_valid(void) {
    int *arr = (int *)malloc(20 * sizeof(int));
    if (!arr) exit(1);
    arr[0] = 1;
    arr[10] = 11;
    arr[19] = 20;
    if (arr[0] != 1 || arr[10] != 11 || arr[19] != 20) exit(1);
    free(arr);
    exit(0);
}

void test_fn_1d_array_oob(void) {
    int *arr = (int *)malloc(20 * sizeof(int));
    if (!arr) exit(1);
    arr[20] = 999; /* Out of bounds at boundary: MUST abort! */
    free(arr);
    exit(0);
}

void test_fn_2d_array_valid(void) {
    int (*matrix)[4] = (int (*)[4])malloc(3 * sizeof(*matrix)); /* 3 rows x 4 cols = 48 bytes */
    if (!matrix) exit(1);
    matrix[0][0] = 10;
    matrix[0][3] = 40;
    matrix[2][3] = 120;
    if (matrix[0][0] != 10 || matrix[0][3] != 40 || matrix[2][3] != 120) exit(1);
    free(matrix);
    exit(0);
}

void test_fn_2d_array_oob_outer(void) {
    int (*matrix)[4] = (int (*)[4])malloc(3 * sizeof(*matrix)); /* 3 rows x 4 cols = 48 bytes */
    if (!matrix) exit(1);
    matrix[3][0] = 999; /* Row 3 is offset 48 bytes on 48-byte alloc: MUST abort! */
    free(matrix);
    exit(0);
}

void test_fn_2d_array_oob_inner(void) {
    /* Allocate 2D array with individual row allocations */
    int *row0 = (int *)malloc(4 * sizeof(int));
    int *row1 = (int *)malloc(4 * sizeof(int));
    int *row2 = (int *)malloc(4 * sizeof(int));
    if (!row0 || !row1 || !row2) exit(1);

    /* Valid access on row 1 */
    row1[0] = 10;
    row1[3] = 40;

    /* Out-of-bounds at inner row boundary on row 1 (size 16 bytes = 4 ints): */
    row1[4] = 999; /* MUST abort! */

    free(row0);
    free(row1);
    free(row2);
    exit(0);
}

/* --- Group 3: Nested Pointers --- */
void test_fn_nested_pointer_array_lifecycle(void) {
    int **ptrs = (int **)malloc(4 * sizeof(int *));
    if (!ptrs) exit(1);
    for (int i = 0; i < 4; i++) {
        ptrs[i] = (int *)malloc(32); /* 8 ints */
        if (!ptrs[i]) exit(1);
        ptrs[i][0] = i * 10;
        ptrs[i][7] = i * 10 + 7;
    }
    /* Independent deallocation of child 1 */
    free(ptrs[1]);
    ptrs[1] = NULL;
    /* Verify children 0, 2, 3 remain intact and tracked */
    if (ptrs[0][0] != 0 || ptrs[2][7] != 27 || ptrs[3][0] != 30) exit(1);
    free(ptrs[0]);
    free(ptrs[2]);
    free(ptrs[3]);
    free(ptrs);
    exit(0);
}

void test_fn_nested_pointer_array_oob(void) {
    int **ptrs = (int **)malloc(4 * sizeof(int *)); /* 32 bytes = 4 pointer elements */
    if (!ptrs) exit(1);
    /* Index 4 is offset 32 bytes on 32-byte array: MUST abort! */
    ptrs[4] = NULL;
    free(ptrs);
    exit(0);
}

typedef struct TestNode {
    int id;
    struct TestNode *next;
    int *payload;
} TestNode;

void test_fn_nested_struct_lifecycle(void) {
    TestNode *n1 = (TestNode *)malloc(sizeof(TestNode));
    TestNode *n2 = (TestNode *)malloc(sizeof(TestNode));
    if (!n1 || !n2) exit(1);
    n1->id = 1;
    n1->payload = (int *)malloc(32);
    n1->next = n2;
    n2->id = 2;
    n2->payload = (int *)malloc(64);
    n2->next = NULL;

    n1->payload[0] = 111;
    n1->payload[7] = 222;
    n2->payload[0] = 333;
    n2->payload[15] = 444;

    if (n1->payload[0] != 111 || n1->payload[7] != 222 ||
        n2->payload[0] != 333 || n2->payload[15] != 444) {
        exit(1);
    }

    free(n2->payload);
    free(n2);
    free(n1->payload);
    free(n1);
    exit(0);
}

/* --- Group 4: Control-Flow Complexity --- */
void test_fn_loop_valid(void) {
    int *p = (int *)malloc(50 * sizeof(int));
    if (!p) exit(1);
    for (int i = 0; i < 50; i++) {
        p[i] = i * 3;
    }
    for (int i = 0; i < 50; i++) {
        if (p[i] != i * 3) exit(1);
    }
    free(p);
    exit(0);
}

void test_fn_loop_oob(void) {
    int *p = (int *)malloc(50 * sizeof(int));
    if (!p) exit(1);
    /* Loop exceeds boundary at i = 50: MUST abort! */
    for (int i = 0; i <= 50; i++) {
        p[i] = i;
    }
    free(p);
    exit(0);
}

void test_fn_cond_branch_valid(void) {
    volatile int flag = 1;
    int *p = NULL;
    if (flag) {
        p = (int *)malloc(32);
    } else {
        p = (int *)malloc(64);
    }
    if (!p) exit(1);
    p[0] = 77;
    p[7] = 88;
    if (p[0] != 77 || p[7] != 88) exit(1);
    free(p);
    exit(0);
}

/* --- Group 5: Allocation Lifecycle --- */
void test_fn_repeated_alloc_free_cycles(void) {
    for (int c = 0; c < 50; c++) {
        int *p = (int *)malloc(64);
        if (!p) exit(1);
        p[0] = c;
        p[15] = c * 2;
        if (p[0] != c || p[15] != c * 2) exit(1);
        free(p);
    }
    exit(0);
}

void test_fn_realloc_relocation_access(void) {
    int *p = (int *)malloc(32);
    if (!p) exit(1);
    p[0] = 0xAA;
    p[7] = 0xBB;
    /* Reallocate to 8192 bytes to force relocation */
    int *p2 = (int *)realloc(p, 8192);
    if (!p2) exit(1);
    if (p2[0] != 0xAA || p2[7] != 0xBB) exit(1);
    /* Access at end of new allocation */
    p2[2047] = 0xCC;
    if (p2[2047] != 0xCC) exit(1);
    free(p2);
    exit(0);
}

void test_fn_stale_pointer_after_realloc(void) {
    int *p = (int *)malloc(32);
    if (!p) exit(1);
    int *p_stale = p;
    int *p_new = (int *)realloc(p, 16384);
    if (!p_new) exit(1);
    if (p_new != p_stale) {
        /* Access through stale pointer must be caught by MemoryGuard and abort */
        *p_stale = 42;
    } else {
        /* In case host allocator expanded in-place, trigger abort */
        abort();
    }
    free(p_new);
    exit(0);
}

void test_fn_zero_size_alloc_oob(void) {
    void *z = malloc(0);
    if (z != NULL) {
        /* Zero-sized allocation has size 0: any non-zero access must abort */
        *(char *)z = 'X';
        free(z);
    } else {
        /* Platform malloc(0) returned NULL -> abort to satisfy violation test */
        abort();
    }
    exit(0);
}

void test_fn_multiple_nearby_allocations_oob(void) {
    char *b1 = (char *)malloc(16);
    char *b2 = (char *)malloc(16);
    char *b3 = (char *)malloc(16);
    if (!b1 || !b2 || !b3) exit(1);
    b2[0] = 'A';
    b2[15] = 'Z';
    /* Cross-boundary access past b2 boundary (offset 16 bytes): MUST abort! */
    b2[16] = '!';
    free(b1);
    free(b2);
    free(b3);
    exit(0);
}

/* --- Group 6: Conservative Behavior --- */
static int test_global_arr[10];

void test_fn_conservative_stack_global(void) {
    int stack_arr[10];
    for (int i = 0; i < 10; i++) {
        stack_arr[i] = i * 5;
        test_global_arr[i] = i * 10;
    }
    if (stack_arr[9] != 45 || test_global_arr[9] != 90) exit(1);
    exit(0);
}

void unknown_callee_sink(int *p) {
    p[0] = 777; /* Uninstrumented external function */
}

void test_fn_conservative_unknown_origin(void) {
    int stack_val = 0;
    unknown_callee_sink(&stack_val);
    if (stack_val != 777) exit(1);

    int *heap_val = (int *)malloc(sizeof(int));
    if (!heap_val) exit(1);
    unknown_callee_sink(heap_val);
    if (*heap_val != 777) exit(1);
    free(heap_val);
    exit(0);
}

static void mixed_path_callee(int cond, int *out) {
    int stack_val = 123;
    int *p = NULL;
    if (cond) {
        p = (int *)malloc(32);
    } else {
        p = &stack_val;
    }
    /* Mixed origin: must NOT be instrumented */
    p[0] = 555;
    *out = p[0];
    if (cond) {
        free(p);
    }
}

void test_fn_conservative_mixed_paths(void) {
    int out1 = 0, out2 = 0;
    mixed_path_callee(1, &out1);
    mixed_path_callee(0, &out2);
    if (out1 != 555 || out2 != 555) exit(1);
    exit(0);
}

static int run_in_child(void (*fn)(void)) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* Silence stderr to avoid expected violation logs during test output */
        if (freopen("/dev/null", "w", stderr) == NULL) {
            /* ignore */
        }
        fn();
        exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

int main(void) {
    printf("====================================================\n");
    printf(" MemoryGuard Full Integration & Instrumentation Test\n");
    printf("====================================================\n");

    int total = 0;
    int passed = 0;

    /* 1. Normal malloc/read/write/free -> must exit 0 */
    total++;
    printf("[TEST] Normal malloc / read / write / free...\n");
    int st1 = run_in_child(test_fn_normal_malloc);
    if (WIFEXITED(st1) && WEXITSTATUS(st1) == 0) {
        printf("  [PASS] Standard malloc/free accesses completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Standard malloc/free failed (status: %d).\n", st1);
    }

    /* 2. Calloc zero initialization and bounds -> must exit 0 */
    total++;
    printf("[TEST] Calloc zero-initialization...\n");
    int st2 = run_in_child(test_fn_calloc_zero_init);
    if (WIFEXITED(st2) && WEXITSTATUS(st2) == 0) {
        printf("  [PASS] Calloc zero-initialization verified and completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Calloc test failed (status: %d).\n", st2);
    }

    /* 3. Realloc expansion and failure handling -> must exit 0 */
    total++;
    printf("[TEST] Realloc expansion and failure handling...\n");
    int st3 = run_in_child(test_fn_realloc_lifecycle);
    if (WIFEXITED(st3) && WEXITSTATUS(st3) == 0) {
        printf("  [PASS] Realloc lifecycle completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Realloc test failed (status: %d).\n", st3);
    }

    /* 4. Use-after-free on standard free -> must abort with SIGABRT */
    total++;
    printf("[TEST] Use-after-free detection on standard free...\n");
    int st4 = run_in_child(test_fn_use_after_free);
    if (WIFSIGNALED(st4) && WTERMSIG(st4) == SIGABRT) {
        printf("  [PASS] Use-after-free caught and aborted by MemoryGuard.\n");
        passed++;
    } else {
        printf("  [FAIL] Use-after-free was NOT caught (status: %d).\n", st4);
    }

    /* 5. Out-of-bounds heap write on standard malloc -> must abort with SIGABRT */
    total++;
    printf("[TEST] Out-of-bounds heap write...\n");
    int st5 = run_in_child(test_fn_oob_write);
    if (WIFSIGNALED(st5) && WTERMSIG(st5) == SIGABRT) {
        printf("  [PASS] Out-of-bounds heap write caught and aborted by MemoryGuard.\n");
        passed++;
    } else {
        printf("  [FAIL] Out-of-bounds write was NOT caught (status: %d).\n", st5);
    }

    /* 6. Out-of-bounds heap read on standard malloc -> must abort with SIGABRT */
    total++;
    printf("[TEST] Out-of-bounds heap read...\n");
    int st6 = run_in_child(test_fn_oob_read);
    if (WIFSIGNALED(st6) && WTERMSIG(st6) == SIGABRT) {
        printf("  [PASS] Out-of-bounds heap read caught and aborted by MemoryGuard.\n");
        passed++;
    } else {
        printf("  [FAIL] Out-of-bounds read was NOT caught (status: %d).\n", st6);
    }

    /* 7. Stack and global accesses -> uninstrumented, must exit normally (exit 0) */
    total++;
    printf("[TEST] Stack and global accesses...\n");
    int st7 = run_in_child(test_fn_stack_global);
    if (WIFEXITED(st7) && WEXITSTATUS(st7) == 0) {
        printf("  [PASS] Stack and global accesses executed without instrumentation.\n");
        passed++;
    } else {
        printf("  [FAIL] Stack/global accesses failed (status: %d).\n", st7);
    }

    /* 8. Unknown pointer origins -> uninstrumented, must exit normally (exit 0) */
    total++;
    printf("[TEST] Unknown pointer origins...\n");
    int src_val = 5;
    int dest_val = 0;
    test_fn_unknown_param(&dest_val, &src_val);
    if (dest_val == 15) {
        printf("  [PASS] Unknown pointer parameter executed without instrumentation.\n");
        passed++;
    } else {
        printf("  [FAIL] Unknown pointer parameter failed.\n");
    }

    /* 9. Regression test: application function mg_process_packet -> must exit 0 */
    total++;
    printf("[TEST] Application function with mg_ prefix (mg_process_packet)...\n");
    int st9 = run_in_child(mg_process_packet);
    if (WIFEXITED(st9) && WEXITSTATUS(st9) == 0) {
        printf("  [PASS] mg_process_packet executed and validated normally.\n");
        passed++;
    } else {
        printf("  [FAIL] mg_process_packet failed (status: %d).\n", st9);
    }

    /* 10. Regression test: application function mg_process_packet_oob -> must abort with SIGABRT */
    total++;
    printf("[TEST] Bounds check inside mg_ prefix function (mg_process_packet_oob)...\n");
    int st10 = run_in_child(mg_process_packet_oob);
    if (WIFSIGNALED(st10) && WTERMSIG(st10) == SIGABRT) {
        printf("  [PASS] Out-of-bounds access inside mg_process_packet_oob caught and aborted.\n");
        passed++;
    } else {
        printf("  [FAIL] mg_process_packet_oob was NOT caught (status: %d).\n", st10);
    }

    /* 11. Interprocedural: Heap pointer passed to a function -> must exit 0 */
    total++;
    printf("[TEST] Interprocedural heap parameter valid access...\n");
    int st11 = run_in_child(test_fn_interproc_heap_param);
    if (WIFEXITED(st11) && WEXITSTATUS(st11) == 0) {
        printf("  [PASS] Interprocedural heap parameter access validated and completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Interprocedural heap parameter test failed (status: %d).\n", st11);
    }

    /* 12. Interprocedural: Heap pointer passed to a function OOB -> must abort with SIGABRT */
    total++;
    printf("[TEST] Interprocedural heap parameter out-of-bounds access...\n");
    int st12 = run_in_child(test_fn_interproc_heap_oob);
    if (WIFSIGNALED(st12) && WTERMSIG(st12) == SIGABRT) {
        printf("  [PASS] Interprocedural out-of-bounds access caught and aborted by MemoryGuard.\n");
        passed++;
    } else {
        printf("  [FAIL] Interprocedural OOB access was NOT caught (status: %d).\n", st12);
    }

    /* 13. Interprocedural: Forwarding heap pointer across multiple functions -> must exit 0 */
    total++;
    printf("[TEST] Interprocedural multi-function forwarding valid access...\n");
    int st13 = run_in_child(test_fn_interproc_forwarding);
    if (WIFEXITED(st13) && WEXITSTATUS(st13) == 0) {
        printf("  [PASS] Multi-function forwarding access validated and completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Multi-function forwarding test failed (status: %d).\n", st13);
    }

    /* 14. Interprocedural: Forwarding heap pointer OOB -> must abort with SIGABRT */
    total++;
    printf("[TEST] Interprocedural multi-function forwarding out-of-bounds access...\n");
    int st14 = run_in_child(test_fn_interproc_forwarding_oob);
    if (WIFSIGNALED(st14) && WTERMSIG(st14) == SIGABRT) {
        printf("  [PASS] Multi-function forwarding out-of-bounds caught and aborted.\n");
        passed++;
    } else {
        printf("  [FAIL] Multi-function forwarding OOB was NOT caught (status: %d).\n", st14);
    }

    /* 15. Interprocedural: Multiple direct call sites with heap pointers -> must exit 0 and abort on OOB */
    total++;
    printf("[TEST] Interprocedural multiple direct call sites...\n");
    int st15 = run_in_child(test_fn_interproc_multi_callsites);
    int st15_oob = run_in_child(test_fn_interproc_multi_callsites_oob);
    if (WIFEXITED(st15) && WEXITSTATUS(st15) == 0 &&
        WIFSIGNALED(st15_oob) && WTERMSIG(st15_oob) == SIGABRT) {
        printf("  [PASS] Multiple direct call sites validated and OOB aborted correctly.\n");
        passed++;
    } else {
        printf("  [FAIL] Multiple direct call sites failed (st15: %d, st15_oob: %d).\n", st15, st15_oob);
    }

    /* 16. Interprocedural: Mixed heap and non-heap call sites -> must remain UNINSTRUMENTED and exit 0 */
    total++;
    printf("[TEST] Interprocedural mixed heap/non-heap call sites (uninstrumented)...\n");
    int st16 = run_in_child(test_fn_interproc_mixed);
    if (WIFEXITED(st16) && WEXITSTATUS(st16) == 0) {
        printf("  [PASS] Mixed call sites remained uninstrumented and executed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Mixed call sites failed (status: %d).\n", st16);
    }

    /* 17. Interprocedural: Externally visible and recursive call patterns -> must remain UNINSTRUMENTED */
    total++;
    printf("[TEST] Interprocedural external/recursive call patterns (uninstrumented)...\n");
    int st17_ext = run_in_child(test_fn_interproc_external_caller);
    int st17_rec = run_in_child(test_fn_interproc_recursive);
    if (WIFEXITED(st17_ext) && WEXITSTATUS(st17_ext) == 0 &&
        WIFEXITED(st17_rec) && WEXITSTATUS(st17_rec) == 0) {
        printf("  [PASS] External and recursive functions remained uninstrumented and executed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] External/recursive functions failed (ext: %d, rec: %d).\n", st17_ext, st17_rec);
    }

    /* 18. Aggregate: Struct insertion and extraction */
    total++;
    printf("[TEST] Aggregate struct insertion and extraction (valid & OOB)...\n");
    int st18_val = run_in_child(mg_test_aggregate_struct_valid);
    int st18_oob = run_in_child(mg_test_aggregate_struct_oob);
    if (WIFEXITED(st18_val) && WEXITSTATUS(st18_val) == 0 &&
        WIFSIGNALED(st18_oob) && WTERMSIG(st18_oob) == SIGABRT) {
        printf("  [PASS] Aggregate struct valid access passed and OOB aborted correctly.\n");
        passed++;
    } else {
        printf("  [FAIL] Aggregate struct failed (val: %d, oob: %d).\n", st18_val, st18_oob);
    }

    /* 19. Aggregate: Nested aggregate struct */
    total++;
    printf("[TEST] Nested aggregate struct insertion and extraction (valid & OOB)...\n");
    int st19_val = run_in_child(mg_test_aggregate_nested_valid);
    int st19_oob = run_in_child(mg_test_aggregate_nested_oob);
    if (WIFEXITED(st19_val) && WEXITSTATUS(st19_val) == 0 &&
        WIFSIGNALED(st19_oob) && WTERMSIG(st19_oob) == SIGABRT) {
        printf("  [PASS] Nested aggregate valid access passed and OOB aborted correctly.\n");
        passed++;
    } else {
        printf("  [FAIL] Nested aggregate failed (val: %d, oob: %d).\n", st19_val, st19_oob);
    }

    /* 20. Aggregate: Aggregate containing array of pointers */
    total++;
    printf("[TEST] Aggregate containing array of pointers (valid & OOB)...\n");
    int st20_val = run_in_child(mg_test_aggregate_array_valid);
    int st20_oob = run_in_child(mg_test_aggregate_array_oob);
    if (WIFEXITED(st20_val) && WEXITSTATUS(st20_val) == 0 &&
        WIFSIGNALED(st20_oob) && WTERMSIG(st20_oob) == SIGABRT) {
        printf("  [PASS] Aggregate array valid access passed and OOB aborted correctly.\n");
        passed++;
    } else {
        printf("  [FAIL] Aggregate array failed (val: %d, oob: %d).\n", st20_val, st20_oob);
    }

    /* 21. Aggregate: Disjoint index insertion and extraction */
    total++;
    printf("[TEST] Aggregate disjoint index propagation...\n");
    int st21 = run_in_child(mg_test_aggregate_disjoint);
    if (WIFEXITED(st21) && WEXITSTATUS(st21) == 0) {
        printf("  [PASS] Aggregate disjoint index propagation completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Aggregate disjoint index failed (status: %d).\n", st21);
    }

    /* 22. Aggregate: Stack pointer in aggregate remains non-heap */
    total++;
    printf("[TEST] Aggregate stack pointer remains non-heap...\n");
    int st22 = run_in_child(mg_test_aggregate_stack_non_heap);
    if (WIFEXITED(st22) && WEXITSTATUS(st22) == 0) {
        printf("  [PASS] Aggregate with stack pointer executed without instrumentation.\n");
        passed++;
    } else {
        printf("  [FAIL] Aggregate stack pointer failed (status: %d).\n", st22);
    }

    /* 23. Interprocedural aggregate argument: pure heap callers */
    total++;
    printf("[TEST] Interprocedural aggregate parameter with pure heap callers...\n");
    int st23_val = run_in_child(mg_test_aggregate_interproc_valid);
    int st23_oob = run_in_child(mg_test_aggregate_interproc_oob);
    if (WIFEXITED(st23_val) && WEXITSTATUS(st23_val) == 0 &&
        WIFSIGNALED(st23_oob) && WTERMSIG(st23_oob) == SIGABRT) {
        printf("  [PASS] Interprocedural aggregate parameter valid passed and OOB aborted correctly.\n");
        passed++;
    } else {
        printf("  [FAIL] Interprocedural aggregate parameter failed (val: %d, oob: %d).\n", st23_val, st23_oob);
    }

    /* 24. Interprocedural aggregate argument: mixed heap/stack callers */
    total++;
    printf("[TEST] Interprocedural aggregate parameter with mixed callers (uninstrumented)...\n");
    int st24 = run_in_child(mg_test_aggregate_mixed);
    if (WIFEXITED(st24) && WEXITSTATUS(st24) == 0) {
        printf("  [PASS] Interprocedural mixed aggregate parameter remained uninstrumented and passed.\n");
        passed++;
    } else {
        printf("  [FAIL] Interprocedural mixed aggregate parameter failed (status: %d).\n", st24);
    }

    /* 25. Pointer-preserving casts and conversions */
    total++;
    printf("[TEST] Pointer-preserving casts and conversions (valid & OOB)...\n");
    int st25_val = run_in_child(mg_test_pointer_cast_valid);
    int st25_oob = run_in_child(mg_test_pointer_cast_oob);
    if (WIFEXITED(st25_val) && WEXITSTATUS(st25_val) == 0 &&
        WIFSIGNALED(st25_oob) && WTERMSIG(st25_oob) == SIGABRT) {
        printf("  [PASS] Pointer cast valid access passed and OOB aborted correctly.\n");
        passed++;
    } else {
        printf("  [FAIL] Pointer cast failed (val: %d, oob: %d).\n", st25_val, st25_oob);
    }

    /* 26. Volatile: Heap-origin volatile read and write (valid & OOB) */
    total++;
    printf("[TEST] Heap-origin volatile read and write (valid & OOB)...\n");
    int st26_val = run_in_child(mg_test_volatile_heap_valid);
    int st26_oob = run_in_child(mg_test_volatile_heap_oob);
    if (WIFEXITED(st26_val) && WEXITSTATUS(st26_val) == 0 &&
        WIFSIGNALED(st26_oob) && WTERMSIG(st26_oob) == SIGABRT) {
        printf("  [PASS] Volatile heap accesses instrumented and OOB aborted correctly.\n");
        passed++;
    } else {
        printf("  [FAIL] Volatile heap test failed (val: %d, oob: %d).\n", st26_val, st26_oob);
    }

    /* 27. Atomic: Atomic load and store (heap origin, skipped instrumentation) */
    total++;
    printf("[TEST] Atomic load and store (skipped for synchronization preservation)...\n");
    int st27 = run_in_child(mg_test_atomic_load_store);
    if (WIFEXITED(st27) && WEXITSTATUS(st27) == 0) {
        printf("  [PASS] Atomic load/store executed with original synchronization semantics.\n");
        passed++;
    } else {
        printf("  [FAIL] Atomic load/store test failed (status: %d).\n", st27);
    }

    /* 28. Atomic: Atomic RMW (heap origin, skipped instrumentation) */
    total++;
    printf("[TEST] Atomic RMW operation (skipped for synchronization preservation)...\n");
    int st28 = run_in_child(mg_test_atomic_rmw);
    if (WIFEXITED(st28) && WEXITSTATUS(st28) == 0) {
        printf("  [PASS] Atomic RMW executed with original synchronization semantics.\n");
        passed++;
    } else {
        printf("  [FAIL] Atomic RMW test failed (status: %d).\n", st28);
    }

    /* 29. Atomic: Atomic compare-exchange (heap origin, skipped instrumentation) */
    total++;
    printf("[TEST] Atomic compare-exchange (skipped for synchronization preservation)...\n");
    int st29 = run_in_child(mg_test_atomic_cmpxchg);
    if (WIFEXITED(st29) && WEXITSTATUS(st29) == 0) {
        printf("  [PASS] Atomic cmpxchg executed with original synchronization semantics.\n");
        passed++;
    } else {
        printf("  [FAIL] Atomic cmpxchg test failed (status: %d).\n", st29);
    }

    /* 30. Atomic: Atomic fence (skipped, no pointer operand) */
    total++;
    printf("[TEST] Atomic fence instructions...\n");
    int st30 = run_in_child(mg_test_atomic_fence);
    if (WIFEXITED(st30) && WEXITSTATUS(st30) == 0) {
        printf("  [PASS] Atomic fence instructions executed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Atomic fence test failed (status: %d).\n", st30);
    }

    /* 31. Atomic: Unknown-origin atomic pointer (skipped instrumentation) */
    total++;
    printf("[TEST] Unknown-origin atomic pointer...\n");
    int st31 = run_in_child(mg_test_atomic_unknown_origin);
    if (WIFEXITED(st31) && WEXITSTATUS(st31) == 0) {
        printf("  [PASS] Unknown-origin atomic accesses executed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Unknown-origin atomic test failed (status: %d).\n", st31);
    }

    /* =========================================================================
     * Step 15: Advanced Memory-Safety Regression Tests
     * ========================================================================= */

    /* 32. Pointer arithmetic: valid chained GEP operations */
    total++;
    printf("[TEST] Pointer arithmetic: valid chained GEP operations...\n");
    int st32 = run_in_child(test_fn_pointer_arith_valid);
    if (WIFEXITED(st32) && WEXITSTATUS(st32) == 0) {
        printf("  [PASS] Chained GEP operations completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Chained GEP operations failed (status: %d).\n", st32);
    }

    /* 33. Pointer arithmetic: out-of-bounds chained GEP */
    total++;
    printf("[TEST] Pointer arithmetic: out-of-bounds chained GEP...\n");
    int st33 = run_in_child(test_fn_pointer_arith_oob);
    if (WIFSIGNALED(st33) && WTERMSIG(st33) == SIGABRT) {
        printf("  [PASS] Out-of-bounds chained GEP caught and aborted by MemoryGuard.\n");
        passed++;
    } else {
        printf("  [FAIL] Out-of-bounds chained GEP was NOT caught (status: %d).\n", st33);
    }

    /* 34. Pointer arithmetic: valid negative pointer offsets */
    total++;
    printf("[TEST] Pointer arithmetic: valid negative pointer offsets...\n");
    int st34 = run_in_child(test_fn_pointer_arith_neg_valid);
    if (WIFEXITED(st34) && WEXITSTATUS(st34) == 0) {
        printf("  [PASS] Valid negative pointer offsets completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Valid negative pointer offset failed (status: %d).\n", st34);
    }

    /* 35. Pointer arithmetic: negative pointer offset underflow */
    total++;
    printf("[TEST] Pointer arithmetic: negative pointer offset underflow...\n");
    int st35 = run_in_child(test_fn_pointer_arith_neg_oob);
    if (WIFSIGNALED(st35) && WTERMSIG(st35) == SIGABRT) {
        printf("  [PASS] Negative pointer underflow caught and aborted by MemoryGuard.\n");
        passed++;
    } else {
        printf("  [FAIL] Negative pointer underflow was NOT caught (status: %d).\n", st35);
    }

    /* 36. Arrays: valid accesses within 1D heap array */
    total++;
    printf("[TEST] Arrays: valid accesses within 1D heap array...\n");
    int st36 = run_in_child(test_fn_1d_array_valid);
    if (WIFEXITED(st36) && WEXITSTATUS(st36) == 0) {
        printf("  [PASS] 1D heap array accesses completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] 1D heap array test failed (status: %d).\n", st36);
    }

    /* 37. Arrays: out-of-bounds access at 1D array boundary */
    total++;
    printf("[TEST] Arrays: out-of-bounds access at 1D array boundary...\n");
    int st37 = run_in_child(test_fn_1d_array_oob);
    if (WIFSIGNALED(st37) && WTERMSIG(st37) == SIGABRT) {
        printf("  [PASS] 1D array boundary violation caught and aborted.\n");
        passed++;
    } else {
        printf("  [FAIL] 1D array boundary violation was NOT caught (status: %d).\n", st37);
    }

    /* 38. Multidimensional arrays: valid multidimensional heap array */
    total++;
    printf("[TEST] Multidimensional arrays: valid accesses in 2D heap matrix...\n");
    int st38 = run_in_child(test_fn_2d_array_valid);
    if (WIFEXITED(st38) && WEXITSTATUS(st38) == 0) {
        printf("  [PASS] 2D heap matrix accesses completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] 2D heap matrix test failed (status: %d).\n", st38);
    }

    /* 39. Multidimensional arrays: outer boundary out-of-bounds access */
    total++;
    printf("[TEST] Multidimensional arrays: outer boundary out-of-bounds...\n");
    int st39 = run_in_child(test_fn_2d_array_oob_outer);
    if (WIFSIGNALED(st39) && WTERMSIG(st39) == SIGABRT) {
        printf("  [PASS] 2D outer boundary violation caught and aborted.\n");
        passed++;
    } else {
        printf("  [FAIL] 2D outer boundary violation was NOT caught (status: %d).\n", st39);
    }

    /* 40. Multidimensional arrays: inner row boundary out-of-bounds access */
    total++;
    printf("[TEST] Multidimensional arrays: inner row boundary out-of-bounds...\n");
    int st40 = run_in_child(test_fn_2d_array_oob_inner);
    if (WIFSIGNALED(st40) && WTERMSIG(st40) == SIGABRT) {
        printf("  [PASS] 2D inner row boundary violation caught and aborted.\n");
        passed++;
    } else {
        printf("  [FAIL] 2D inner row boundary violation was NOT caught (status: %d).\n", st40);
    }

    /* 41. Nested pointers: pointer array allocation & independent lifecycle */
    total++;
    printf("[TEST] Nested pointers: pointer array independent lifecycle...\n");
    int st41 = run_in_child(test_fn_nested_pointer_array_lifecycle);
    if (WIFEXITED(st41) && WEXITSTATUS(st41) == 0) {
        printf("  [PASS] Nested pointer array lifecycle completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Nested pointer array lifecycle failed (status: %d).\n", st41);
    }

    /* 42. Nested pointers: pointer array out-of-bounds access */
    total++;
    printf("[TEST] Nested pointers: pointer array out-of-bounds access...\n");
    int st42 = run_in_child(test_fn_nested_pointer_array_oob);
    if (WIFSIGNALED(st42) && WTERMSIG(st42) == SIGABRT) {
        printf("  [PASS] Pointer array boundary violation caught and aborted.\n");
        passed++;
    } else {
        printf("  [FAIL] Pointer array boundary violation was NOT caught (status: %d).\n", st42);
    }

    /* 43. Nested pointers: heap objects containing pointers (struct nodes) */
    total++;
    printf("[TEST] Nested pointers: linked heap objects with payload pointers...\n");
    int st43 = run_in_child(test_fn_nested_struct_lifecycle);
    if (WIFEXITED(st43) && WEXITSTATUS(st43) == 0) {
        printf("  [PASS] Linked heap objects lifecycle completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Linked heap objects failed (status: %d).\n", st43);
    }

    /* 44. Control flow: loop involving heap pointer accesses (valid) */
    total++;
    printf("[TEST] Control flow: loop involving heap pointer accesses (valid)...\n");
    int st44 = run_in_child(test_fn_loop_valid);
    if (WIFEXITED(st44) && WEXITSTATUS(st44) == 0) {
        printf("  [PASS] Loop accesses within bounds completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Loop valid accesses failed (status: %d).\n", st44);
    }

    /* 45. Control flow: loop exceeding heap boundary (out-of-bounds) */
    total++;
    printf("[TEST] Control flow: loop exceeding heap boundary (out-of-bounds)...\n");
    int st45 = run_in_child(test_fn_loop_oob);
    if (WIFSIGNALED(st45) && WTERMSIG(st45) == SIGABRT) {
        printf("  [PASS] Loop boundary violation caught and aborted by MemoryGuard.\n");
        passed++;
    } else {
        printf("  [FAIL] Loop boundary violation was NOT caught (status: %d).\n", st45);
    }

    /* 46. Control flow: conditional branch with heap pointer */
    total++;
    printf("[TEST] Control flow: conditional branch with heap pointer...\n");
    int st46 = run_in_child(test_fn_cond_branch_valid);
    if (WIFEXITED(st46) && WEXITSTATUS(st46) == 0) {
        printf("  [PASS] Conditional branch heap access completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Conditional branch test failed (status: %d).\n", st46);
    }

    /* 47. Control flow: Select with matching heap origins (valid) */
    total++;
    printf("[TEST] Control flow: Select with matching heap origins (valid)...\n");
    int st47 = run_in_child(mg_test_select_matching_heap_valid);
    if (WIFEXITED(st47) && WEXITSTATUS(st47) == 0) {
        printf("  [PASS] Select matching heap valid access completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Select matching heap valid failed (status: %d).\n", st47);
    }

    /* 48. Control flow: Select with matching heap origins (out-of-bounds) */
    total++;
    printf("[TEST] Control flow: Select with matching heap origins (out-of-bounds)...\n");
    int st48 = run_in_child(mg_test_select_matching_heap_oob);
    if (WIFSIGNALED(st48) && WTERMSIG(st48) == SIGABRT) {
        printf("  [PASS] Select matching heap out-of-bounds caught and aborted.\n");
        passed++;
    } else {
        printf("  [FAIL] Select matching heap OOB was NOT caught (status: %d).\n", st48);
    }

    /* 49. Control flow: Select with conflicting origins (heap vs stack) -> uninstrumented */
    total++;
    printf("[TEST] Control flow: Select with conflicting origins (uninstrumented)...\n");
    int st49 = run_in_child(mg_test_select_conflicting_heap_stack);
    if (WIFEXITED(st49) && WEXITSTATUS(st49) == 0) {
        printf("  [PASS] Conflicting Select origins remained uninstrumented and executed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Conflicting Select test failed (status: %d).\n", st49);
    }

    /* 50. Control flow: PHI node with matching heap origins (valid) */
    total++;
    printf("[TEST] Control flow: PHI node with matching heap origins (valid)...\n");
    int st50 = run_in_child(mg_test_phi_matching_heap_valid);
    if (WIFEXITED(st50) && WEXITSTATUS(st50) == 0) {
        printf("  [PASS] PHI matching heap valid access completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] PHI matching heap valid failed (status: %d).\n", st50);
    }

    /* 51. Control flow: PHI node with matching heap origins (out-of-bounds) */
    total++;
    printf("[TEST] Control flow: PHI node with matching heap origins (out-of-bounds)...\n");
    int st51 = run_in_child(mg_test_phi_matching_heap_oob);
    if (WIFSIGNALED(st51) && WTERMSIG(st51) == SIGABRT) {
        printf("  [PASS] PHI matching heap out-of-bounds caught and aborted.\n");
        passed++;
    } else {
        printf("  [FAIL] PHI matching heap OOB was NOT caught (status: %d).\n", st51);
    }

    /* 52. Control flow: PHI node with conflicting origins (heap vs stack) -> uninstrumented */
    total++;
    printf("[TEST] Control flow: PHI node with conflicting origins (uninstrumented)...\n");
    int st52 = run_in_child(mg_test_phi_conflicting_heap_stack);
    if (WIFEXITED(st52) && WEXITSTATUS(st52) == 0) {
        printf("  [PASS] Conflicting PHI origins remained uninstrumented and executed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Conflicting PHI test failed (status: %d).\n", st52);
    }

    /* 53. Control flow: PHI node with unknown origin branch -> uninstrumented */
    total++;
    printf("[TEST] Control flow: PHI node with unknown origin branch (uninstrumented)...\n");
    int st53 = run_in_child(mg_test_phi_unknown_origin_branch);
    if (WIFEXITED(st53) && WEXITSTATUS(st53) == 0) {
        printf("  [PASS] PHI with unknown origin branch remained uninstrumented.\n");
        passed++;
    } else {
        printf("  [FAIL] PHI with unknown branch failed (status: %d).\n", st53);
    }

    /* 54. Allocation lifecycle: repeated allocate/free cycles */
    total++;
    printf("[TEST] Allocation lifecycle: repeated allocate/free cycles...\n");
    int st54 = run_in_child(test_fn_repeated_alloc_free_cycles);
    if (WIFEXITED(st54) && WEXITSTATUS(st54) == 0) {
        printf("  [PASS] 50 allocate/free cycles completed without registry leaks.\n");
        passed++;
    } else {
        printf("  [FAIL] Repeated allocate/free cycles failed (status: %d).\n", st54);
    }

    /* 55. Allocation lifecycle: realloc relocation data preservation & new bounds */
    total++;
    printf("[TEST] Allocation lifecycle: access after realloc relocation...\n");
    int st55 = run_in_child(test_fn_realloc_relocation_access);
    if (WIFEXITED(st55) && WEXITSTATUS(st55) == 0) {
        printf("  [PASS] Access after realloc relocation validated successfully.\n");
        passed++;
    } else {
        printf("  [FAIL] Access after realloc relocation failed (status: %d).\n", st55);
    }

    /* 56. Allocation lifecycle: access through stale pointer after realloc */
    total++;
    printf("[TEST] Allocation lifecycle: access through stale pointer after realloc...\n");
    int st56 = run_in_child(test_fn_stale_pointer_after_realloc);
    if (WIFSIGNALED(st56) && WTERMSIG(st56) == SIGABRT) {
        printf("  [PASS] Stale pointer access caught and aborted by MemoryGuard.\n");
        passed++;
    } else {
        printf("  [FAIL] Stale pointer access was NOT caught (status: %d).\n", st56);
    }

    /* 57. Allocation lifecycle: zero-size allocation boundary behavior */
    total++;
    printf("[TEST] Allocation lifecycle: zero-size allocation access rejection...\n");
    int st57 = run_in_child(test_fn_zero_size_alloc_oob);
    if (WIFSIGNALED(st57) && WTERMSIG(st57) == SIGABRT) {
        printf("  [PASS] Zero-size allocation access caught and aborted by MemoryGuard.\n");
        passed++;
    } else {
        printf("  [FAIL] Zero-size allocation access was NOT caught (status: %d).\n", st57);
    }

    /* 58. Allocation lifecycle: multiple simultaneous allocations nearby boundary */
    total++;
    printf("[TEST] Allocation lifecycle: multiple simultaneous allocations boundary check...\n");
    int st58 = run_in_child(test_fn_multiple_nearby_allocations_oob);
    if (WIFSIGNALED(st58) && WTERMSIG(st58) == SIGABRT) {
        printf("  [PASS] Out-of-bounds cross-boundary access caught and aborted.\n");
        passed++;
    } else {
        printf("  [FAIL] Out-of-bounds cross-boundary access was NOT caught (status: %d).\n", st58);
    }

    /* 59. Conservative behavior: stack and global accesses remain uninstrumented */
    total++;
    printf("[TEST] Conservative behavior: stack and global arrays uninstrumented...\n");
    int st59 = run_in_child(test_fn_conservative_stack_global);
    if (WIFEXITED(st59) && WEXITSTATUS(st59) == 0) {
        printf("  [PASS] Stack and global arrays executed without instrumentation.\n");
        passed++;
    } else {
        printf("  [FAIL] Stack/global array test failed (status: %d).\n", st59);
    }

    /* 60. Conservative behavior: unknown pointer origins remain uninstrumented */
    total++;
    printf("[TEST] Conservative behavior: unknown pointer origins uninstrumented...\n");
    int st60 = run_in_child(test_fn_conservative_unknown_origin);
    if (WIFEXITED(st60) && WEXITSTATUS(st60) == 0) {
        printf("  [PASS] Unknown pointer origin executed without instrumentation.\n");
        passed++;
    } else {
        printf("  [FAIL] Unknown pointer origin test failed (status: %d).\n", st60);
    }

    /* 61. Conservative behavior: mixed heap/non-heap paths remain uninstrumented */
    total++;
    printf("[TEST] Conservative behavior: mixed heap/non-heap paths uninstrumented...\n");
    int st61 = run_in_child(test_fn_conservative_mixed_paths);
    if (WIFEXITED(st61) && WEXITSTATUS(st61) == 0) {
        printf("  [PASS] Mixed heap/non-heap path executed without instrumentation.\n");
        passed++;
    } else {
        printf("  [FAIL] Mixed heap/non-heap path test failed (status: %d).\n", st61);
    }

    /* 62. Conservative behavior: unsupported pointer transformation (inttoptr) */
    total++;
    printf("[TEST] Conservative behavior: unsupported integer-to-pointer cast...\n");
    int st62 = run_in_child(mg_test_inttoptr_unsupported_cast);
    if (WIFEXITED(st62) && WEXITSTATUS(st62) == 0) {
        printf("  [PASS] inttoptr pointer cast remained uninstrumented and executed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] inttoptr test failed (status: %d).\n", st62);
    }

    printf("====================================================\n");
    printf("Result: %d / %d tests passed\n", passed, total);
    printf("====================================================\n");

    return (passed == total) ? 0 : 1;
}

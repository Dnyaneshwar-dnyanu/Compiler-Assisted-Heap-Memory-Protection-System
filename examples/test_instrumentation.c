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
    printf(" MemoryGuard Step 9: Call-Site Rewriting Test Suite \n");
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

    printf("====================================================\n");
    printf("Result: %d / %d tests passed\n", passed, total);
    printf("====================================================\n");

    return (passed == total) ? 0 : 1;
}

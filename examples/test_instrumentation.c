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

    printf("====================================================\n");
    printf("Result: %d / %d tests passed\n", passed, total);
    printf("====================================================\n");

    return (passed == total) ? 0 : 1;
}

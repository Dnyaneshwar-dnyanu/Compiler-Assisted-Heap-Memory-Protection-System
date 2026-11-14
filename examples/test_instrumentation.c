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

/* Test 1: Valid heap read and write */
void test_fn_valid_heap(void) {
    int *p = (int *)malloc(64);
    if (!p) {
        exit(1);
    }
    mg_register_external(p, 64);
    /* Instrumented writes and reads within bounds */
    p[0] = 100;
    p[15] = 200;
    int a = p[0];
    int b = p[15];
    if (a != 100 || b != 200) {
        exit(1);
    }
    mg_unregister_external(p);
    free(p);
    exit(0);
}

/* Test 2: Out-of-bounds heap write */
void test_fn_oob_write(void) {
    int *p = (int *)malloc(32); /* 32 bytes = 8 ints (indices 0..7) */
    if (!p) {
        exit(1);
    }
    mg_register_external(p, 32);
    /* p[8] is at offset 32 bytes, which exceeds the 32-byte boundary */
    p[8] = 999;
    mg_unregister_external(p);
    free(p);
    exit(0);
}

/* Test 3: Out-of-bounds heap read */
void test_fn_oob_read(void) {
    int *p = (int *)malloc(32); /* 32 bytes = 8 ints (indices 0..7) */
    if (!p) {
        exit(1);
    }
    mg_register_external(p, 32);
    /* Reading 4 bytes at offset 32 exceeds boundary */
    int val = p[8];
    (void)val;
    mg_unregister_external(p);
    free(p);
    exit(0);
}

/* Test 4: Access after free (use-after-free) */
void test_fn_use_after_free(void) {
    int *p = (int *)malloc(32);
    if (!p) {
        exit(1);
    }
    mg_register_external(p, 32);
    p[0] = 42;
    mg_unregister_external(p);
    free(p);
    /* Accessing p after free: p is no longer in the registry */
    p[0] = 84;
    exit(0);
}

/* Test 5: Stack and global accesses (NON_HEAP: must NOT be instrumented) */
void test_fn_stack_global(void) {
    int stack_var = 123;
    stack_var += 50;
    global_counter = stack_var;
    if (global_counter != 173) {
        exit(1);
    }
    exit(0);
}

/* Test 6: Unknown pointer origin (UNKNOWN: must NOT be instrumented) */
void test_fn_unknown_param(int *param_dest, const int *param_src) {
    int val = *param_src;
    *param_dest = val + 10;
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
    printf(" MemoryGuard Step 8: Instrumentation Validation Test\n");
    printf("====================================================\n");

    int total = 0;
    int passed = 0;

    /* 1. Valid heap read/write -> must succeed normally (exit 0) */
    total++;
    printf("[TEST] Valid heap read/write...\n");
    int st1 = run_in_child(test_fn_valid_heap);
    if (WIFEXITED(st1) && WEXITSTATUS(st1) == 0) {
        printf("  [PASS] Valid heap accesses completed normally.\n");
        passed++;
    } else {
        printf("  [FAIL] Valid heap accesses did not exit with 0 (status: %d).\n", st1);
    }

    /* 2. Out-of-bounds heap write -> must abort with SIGABRT */
    total++;
    printf("[TEST] Out-of-bounds heap write...\n");
    int st2 = run_in_child(test_fn_oob_write);
    if (WIFSIGNALED(st2) && WTERMSIG(st2) == SIGABRT) {
        printf("  [PASS] Out-of-bounds heap write caught and aborted by MemoryGuard.\n");
        passed++;
    } else {
        printf("  [FAIL] Out-of-bounds write was NOT caught by MemoryGuard (status: %d).\n", st2);
    }

    /* 3. Out-of-bounds heap read -> must abort with SIGABRT */
    total++;
    printf("[TEST] Out-of-bounds heap read...\n");
    int st3 = run_in_child(test_fn_oob_read);
    if (WIFSIGNALED(st3) && WTERMSIG(st3) == SIGABRT) {
        printf("  [PASS] Out-of-bounds heap read caught and aborted by MemoryGuard.\n");
        passed++;
    } else {
        printf("  [FAIL] Out-of-bounds read was NOT caught by MemoryGuard (status: %d).\n", st3);
    }

    /* 4. Access after free -> must abort with SIGABRT */
    total++;
    printf("[TEST] Access after free (use-after-free)...\n");
    int st4 = run_in_child(test_fn_use_after_free);
    if (WIFSIGNALED(st4) && WTERMSIG(st4) == SIGABRT) {
        printf("  [PASS] Use-after-free write caught and aborted by MemoryGuard.\n");
        passed++;
    } else {
        printf("  [FAIL] Use-after-free write was NOT caught by MemoryGuard (status: %d).\n", st4);
    }

    /* 5. Stack and global accesses -> uninstrumented, must exit normally (exit 0) */
    total++;
    printf("[TEST] Stack and global accesses...\n");
    int st5 = run_in_child(test_fn_stack_global);
    if (WIFEXITED(st5) && WEXITSTATUS(st5) == 0) {
        printf("  [PASS] Stack and global accesses executed without instrumentation.\n");
        passed++;
    } else {
        printf("  [FAIL] Stack/global accesses failed (status: %d).\n", st5);
    }

    /* 6. Unknown pointer origins -> uninstrumented, must exit normally (exit 0) */
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

    printf("====================================================\n");
    printf("Result: %d / %d tests passed\n", passed, total);
    printf("====================================================\n");

    return (passed == total) ? 0 : 1;
}

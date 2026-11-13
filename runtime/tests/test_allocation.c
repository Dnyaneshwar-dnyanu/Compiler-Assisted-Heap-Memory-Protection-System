#include "memoryguard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int total_assertions = 0;
static int failed_assertions = 0;

#define TEST_ASSERT(cond, msg) do { \
    total_assertions++; \
    if (!(cond)) { \
        fprintf(stderr, "  [FAIL] Line %d: %s (%s)\n", __LINE__, msg, #cond); \
        failed_assertions++; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    printf("[RUN]  %s\n", #fn); \
    int before_failures = failed_assertions; \
    fn(); \
    if (failed_assertions == before_failures) { \
        printf("[PASS] %s\n", #fn); \
    } else { \
        printf("[FAIL] %s (failures in this test: %d)\n", #fn, failed_assertions - before_failures); \
    } \
} while (0)

/**
 * @brief Tests basic successful allocation, pointer usability, and registration.
 */
static void test_successful_allocation(void) {
    size_t size = 128;
    void *ptr = mg_malloc(size);

    TEST_ASSERT(ptr != NULL, "mg_malloc returned non-NULL pointer");
    TEST_ASSERT(mg_is_tracked(ptr) == 1, "Allocated pointer is registered");
    TEST_ASSERT(mg_get_allocation_size(ptr) == size, "Recorded size matches requested size");

    /* Verify allocated memory is readable and writable */
    memset(ptr, 0xAA, size);
    unsigned char *byte_ptr = (unsigned char *)ptr;
    int data_valid = 1;
    for (size_t i = 0; i < size; i++) {
        if (byte_ptr[i] != 0xAA) {
            data_valid = 0;
            break;
        }
    }
    TEST_ASSERT(data_valid == 1, "Allocated memory successfully written and verified");

    /* Clean up */
    mg_free(ptr);
    TEST_ASSERT(mg_is_tracked(ptr) == 0, "Pointer no longer tracked after free");
}

/**
 * @brief Tests allocation size tracking for various allocation sizes.
 */
static void test_allocation_size_tracking(void) {
    size_t sizes[] = {1, 16, 64, 256, 1024, 4096};
    size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    void *ptrs[sizeof(sizes) / sizeof(sizes[0])];

    /* Allocate and verify recorded sizes */
    for (size_t i = 0; i < num_sizes; i++) {
        ptrs[i] = mg_malloc(sizes[i]);
        TEST_ASSERT(ptrs[i] != NULL, "Allocation succeeded");
        TEST_ASSERT(mg_get_allocation_size(ptrs[i]) == sizes[i], "Recorded size matches allocation size");
    }

    /* Verify all sizes remain intact while all are active */
    for (size_t i = 0; i < num_sizes; i++) {
        TEST_ASSERT(mg_get_allocation_size(ptrs[i]) == sizes[i], "Size remains consistent");
    }

    /* Free all and verify sizes become 0 */
    for (size_t i = 0; i < num_sizes; i++) {
        mg_free(ptrs[i]);
        TEST_ASSERT(mg_get_allocation_size(ptrs[i]) == 0, "Size is 0 after deallocation");
        TEST_ASSERT(mg_is_tracked(ptrs[i]) == 0, "Untracked after deallocation");
    }
}

/**
 * @brief Tests multiple simultaneous active allocations and non-sequential deallocations.
 */
static void test_multiple_simultaneous_allocations(void) {
    const int count = 5;
    void *ptrs[5];
    size_t sizes[5] = {32, 64, 128, 256, 512};

    /* Allocate all blocks */
    for (int i = 0; i < count; i++) {
        ptrs[i] = mg_malloc(sizes[i]);
        TEST_ASSERT(ptrs[i] != NULL, "Simultaneous allocation succeeded");
        TEST_ASSERT(mg_is_tracked(ptrs[i]) == 1, "Simultaneous allocation tracked");
    }

    TEST_ASSERT(mg_get_tracked_count() == 5, "Registry contains exactly 5 active allocations");

    /* Free middle block (index 2: 128 bytes) */
    mg_free(ptrs[2]);
    TEST_ASSERT(mg_is_tracked(ptrs[2]) == 0, "Middle block is no longer tracked");
    TEST_ASSERT(mg_get_allocation_size(ptrs[2]) == 0, "Middle block size is now 0");
    TEST_ASSERT(mg_get_tracked_count() == 4, "Registry count decremented to 4");

    /* Verify surrounding blocks remain tracked and unchanged */
    TEST_ASSERT(mg_is_tracked(ptrs[0]) == 1 && mg_get_allocation_size(ptrs[0]) == sizes[0], "Block 0 intact");
    TEST_ASSERT(mg_is_tracked(ptrs[1]) == 1 && mg_get_allocation_size(ptrs[1]) == sizes[1], "Block 1 intact");
    TEST_ASSERT(mg_is_tracked(ptrs[3]) == 1 && mg_get_allocation_size(ptrs[3]) == sizes[3], "Block 3 intact");
    TEST_ASSERT(mg_is_tracked(ptrs[4]) == 1 && mg_get_allocation_size(ptrs[4]) == sizes[4], "Block 4 intact");

    /* Free head block (most recently allocated: index 4) */
    mg_free(ptrs[4]);
    TEST_ASSERT(mg_is_tracked(ptrs[4]) == 0, "Head block no longer tracked");
    TEST_ASSERT(mg_get_tracked_count() == 3, "Registry count decremented to 3");

    /* Free tail block (oldest allocated: index 0) */
    mg_free(ptrs[0]);
    TEST_ASSERT(mg_is_tracked(ptrs[0]) == 0, "Tail block no longer tracked");
    TEST_ASSERT(mg_get_tracked_count() == 2, "Registry count decremented to 2");

    /* Free remaining blocks */
    mg_free(ptrs[1]);
    mg_free(ptrs[3]);
    TEST_ASSERT(mg_get_tracked_count() == 0, "All blocks freed, registry is empty");
}

/**
 * @brief Tests successful deallocation of an active pointer.
 */
static void test_successful_deallocation(void) {
    size_t size = 256;
    void *ptr = mg_malloc(size);

    TEST_ASSERT(ptr != NULL, "Allocated memory successfully");
    TEST_ASSERT(mg_is_tracked(ptr) == 1, "Pointer is tracked prior to free");

    mg_free(ptr);

    TEST_ASSERT(mg_is_tracked(ptr) == 0, "Pointer is not tracked after mg_free");
    TEST_ASSERT(mg_get_allocation_size(ptr) == 0, "Allocation size is 0 after mg_free");
}

/**
 * @brief Tests tracking status after deallocation and ensures safe double-free handling.
 */
static void test_tracking_after_deallocation(void) {
    void *ptr = mg_malloc(64);
    TEST_ASSERT(ptr != NULL, "Pointer allocated");
    TEST_ASSERT(mg_is_tracked(ptr) == 1, "Tracked before deallocation");
    TEST_ASSERT(mg_get_allocation_size(ptr) == 64, "Size is 64 before deallocation");

    /* First deallocation */
    mg_free(ptr);
    TEST_ASSERT(mg_is_tracked(ptr) == 0, "Untracked after first deallocation");
    TEST_ASSERT(mg_get_allocation_size(ptr) == 0, "Size is 0 after first deallocation");

    /* Second deallocation attempt (double free) must be safely ignored without crashing */
    mg_free(ptr);
    TEST_ASSERT(mg_is_tracked(ptr) == 0, "Still untracked after duplicate mg_free call");
    TEST_ASSERT(mg_get_allocation_size(ptr) == 0, "Size remains 0 after duplicate mg_free call");
}

/**
 * @brief Tests NULL allocation handling, NULL free, and untracked pointer handling.
 */
static void test_null_allocation_handling(void) {
    /* 1. NULL pointer checks */
    TEST_ASSERT(mg_is_tracked(NULL) == 0, "mg_is_tracked(NULL) returns 0");
    TEST_ASSERT(mg_get_allocation_size(NULL) == 0, "mg_get_allocation_size(NULL) returns 0");

    /* 2. Freeing NULL must be safe and not crash */
    mg_free(NULL);
    TEST_ASSERT(1, "mg_free(NULL) returned safely without error");

    /* 3. Handling zero-size allocation safely */
    void *zero_ptr = mg_malloc(0);
    if (zero_ptr != NULL) {
        /* If system malloc(0) returns a non-null pointer, verify tracking and cleanup */
        TEST_ASSERT(mg_is_tracked(zero_ptr) == 1, "Zero-size allocation is registered if non-null");
        TEST_ASSERT(mg_get_allocation_size(zero_ptr) == 0, "Zero-size allocation records size 0");
        mg_free(zero_ptr);
        TEST_ASSERT(mg_is_tracked(zero_ptr) == 0, "Zero-size allocation untracked after free");
    } else {
        /* If system malloc(0) returns NULL, verify NULL behavior */
        TEST_ASSERT(mg_is_tracked(NULL) == 0, "NULL result from mg_malloc(0) is not tracked");
    }

    /* 4. Untracked stack pointer must not be tracked and mg_free must not crash */
    int stack_var = 12345;
    TEST_ASSERT(mg_is_tracked(&stack_var) == 0, "Stack variable address is not tracked");
    TEST_ASSERT(mg_get_allocation_size(&stack_var) == 0, "Stack variable size is 0");
    mg_free(&stack_var);
    TEST_ASSERT(1, "mg_free on stack pointer safely ignored");

    /* 5. Interior pointer must not be recognized as base address */
    void *buf = mg_malloc(100);
    TEST_ASSERT(buf != NULL, "Allocated 100 bytes");
    void *interior = (char *)buf + 20;
    TEST_ASSERT(mg_is_tracked(interior) == 0, "Interior offset pointer is not tracked");
    TEST_ASSERT(mg_get_allocation_size(interior) == 0, "Interior offset size is 0");

    /* Freeing interior pointer must be ignored, preserving base allocation */
    mg_free(interior);
    TEST_ASSERT(mg_is_tracked(buf) == 1, "Base allocation still tracked after mg_free on interior pointer");

    /* Clean up base allocation */
    mg_free(buf);
    TEST_ASSERT(mg_is_tracked(buf) == 0, "Base allocation freed successfully");
}

int main(void) {
    printf("========================================\n");
    printf("  MemoryGuard Allocation Registry Tests \n");
    printf("========================================\n");

    RUN_TEST(test_successful_allocation);
    RUN_TEST(test_allocation_size_tracking);
    RUN_TEST(test_multiple_simultaneous_allocations);
    RUN_TEST(test_successful_deallocation);
    RUN_TEST(test_tracking_after_deallocation);
    RUN_TEST(test_null_allocation_handling);

    printf("========================================\n");
    printf("Summary: %d assertions, %d failed\n", total_assertions, failed_assertions);
    printf("========================================\n");

    if (failed_assertions == 0) {
        printf("All tests PASSED!\n");
        return 0;
    } else {
        printf("Tests FAILED: %d assertion failure(s).\n", failed_assertions);
        return 1;
    }
}

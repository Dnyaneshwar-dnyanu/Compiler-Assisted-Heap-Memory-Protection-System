#include "memoryguard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#endif

#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
/**
 * @brief Provide default AddressSanitizer options.
 *
 * Setting allocator_may_return_null=1 ensures realloc/malloc return NULL on
 * extreme sizes (such as SIZE_MAX in failure tests) rather than aborting.
 */
const char *__asan_default_options(void) {
    return "allocator_may_return_null=1";
}
#endif

#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
/**
 * @brief Provide default ThreadSanitizer options.
 */
const char *__tsan_default_options(void) {
    return "allocator_may_return_null=1";
}
#endif

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
    size_t initial_count = mg_get_tracked_count();
    void *ptr = mg_malloc(size);

    TEST_ASSERT(ptr != NULL, "mg_malloc returned non-NULL pointer");
    TEST_ASSERT(mg_is_tracked(ptr) == 1, "Allocated pointer is registered");
    TEST_ASSERT(mg_get_allocation_size(ptr) == size, "Recorded size matches requested size");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Tracked count incremented after malloc");

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

    /* Clean up safely using mg_get_tracked_count() without reusing ptr after free */
    mg_free(ptr);
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored to initial after free");
}

/**
 * @brief Tests allocation size tracking for various allocation sizes.
 */
static void test_allocation_size_tracking(void) {
    size_t sizes[] = {1, 16, 64, 256, 1024, 4096};
    size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    void *ptrs[sizeof(sizes) / sizeof(sizes[0])];
    size_t initial_count = mg_get_tracked_count();

    /* Allocate and verify recorded sizes while pointers are valid */
    for (size_t i = 0; i < num_sizes; i++) {
        ptrs[i] = mg_malloc(sizes[i]);
        TEST_ASSERT(ptrs[i] != NULL, "Allocation succeeded");
        TEST_ASSERT(mg_is_tracked(ptrs[i]) == 1, "Pointer is tracked");
        TEST_ASSERT(mg_get_allocation_size(ptrs[i]) == sizes[i], "Recorded size matches allocation size");
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count + num_sizes, "All allocations registered in registry");

    /* Verify all sizes remain intact while all are active */
    for (size_t i = 0; i < num_sizes; i++) {
        TEST_ASSERT(mg_get_allocation_size(ptrs[i]) == sizes[i], "Size remains consistent");
    }

    /* Free all and verify deallocation count decrements without reusing freed pointers */
    for (size_t i = 0; i < num_sizes; i++) {
        size_t count_before = mg_get_tracked_count();
        mg_free(ptrs[i]);
        TEST_ASSERT(mg_get_tracked_count() == count_before - 1, "Tracked count decremented after deallocation");
    }
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry count returned to initial");
}

/**
 * @brief Tests multiple simultaneous active allocations and non-sequential deallocations.
 */
static void test_multiple_simultaneous_allocations(void) {
    const int count = 5;
    void *ptrs[5];
    size_t sizes[5] = {32, 64, 128, 256, 512};
    size_t initial_count = mg_get_tracked_count();

    /* Allocate all blocks */
    for (int i = 0; i < count; i++) {
        ptrs[i] = mg_malloc(sizes[i]);
        TEST_ASSERT(ptrs[i] != NULL, "Simultaneous allocation succeeded");
        TEST_ASSERT(mg_is_tracked(ptrs[i]) == 1, "Simultaneous allocation tracked");
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count + 5, "Registry contains exactly 5 active allocations");

    /* Free middle block (index 2: 128 bytes) */
    mg_free(ptrs[2]);
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 4, "Registry count decremented to 4");

    /* Verify surrounding blocks remain tracked and unchanged while valid */
    TEST_ASSERT(mg_is_tracked(ptrs[0]) == 1 && mg_get_allocation_size(ptrs[0]) == sizes[0], "Block 0 intact");
    TEST_ASSERT(mg_is_tracked(ptrs[1]) == 1 && mg_get_allocation_size(ptrs[1]) == sizes[1], "Block 1 intact");
    TEST_ASSERT(mg_is_tracked(ptrs[3]) == 1 && mg_get_allocation_size(ptrs[3]) == sizes[3], "Block 3 intact");
    TEST_ASSERT(mg_is_tracked(ptrs[4]) == 1 && mg_get_allocation_size(ptrs[4]) == sizes[4], "Block 4 intact");

    /* Free head block (most recently allocated: index 4) */
    mg_free(ptrs[4]);
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 3, "Registry count decremented to 3");

    /* Free tail block (oldest allocated: index 0) */
    mg_free(ptrs[0]);
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 2, "Registry count decremented to 2");

    /* Free remaining blocks */
    mg_free(ptrs[1]);
    mg_free(ptrs[3]);
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "All blocks freed, registry is back to initial");
}

/**
 * @brief Tests successful deallocation of an active pointer using safe count checks.
 */
static void test_successful_deallocation(void) {
    size_t size = 256;
    size_t initial_count = mg_get_tracked_count();
    void *ptr = mg_malloc(size);

    TEST_ASSERT(ptr != NULL, "Allocated memory successfully");
    TEST_ASSERT(mg_is_tracked(ptr) == 1, "Pointer is tracked prior to free");
    TEST_ASSERT(mg_get_allocation_size(ptr) == size, "Recorded size matches requested size prior to free");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Registry count incremented by 1");

    mg_free(ptr);

    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry count decremented after mg_free");
}

/**
 * @brief Tests tracking status after deallocation using safe registry count checks.
 */
static void test_tracking_after_deallocation(void) {
    size_t initial_count = mg_get_tracked_count();
    void *ptr = mg_malloc(64);
    TEST_ASSERT(ptr != NULL, "Pointer allocated");
    TEST_ASSERT(mg_is_tracked(ptr) == 1, "Tracked before deallocation");
    TEST_ASSERT(mg_get_allocation_size(ptr) == 64, "Size is 64 before deallocation");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Tracked count incremented");

    /* Deallocation verified safely without reusing ptr */
    mg_free(ptr);
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored to initial after deallocation");
}

/**
 * @brief Tests NULL allocation handling, NULL free, and untracked pointer handling.
 */
static void test_null_allocation_handling(void) {
    /* 1. NULL pointer checks */
    TEST_ASSERT(mg_is_tracked(NULL) == 0, "mg_is_tracked(NULL) returns 0");
    TEST_ASSERT(mg_get_allocation_size(NULL) == 0, "mg_get_allocation_size(NULL) returns 0");

    /* 2. Freeing NULL must be safe and not alter registry */
    size_t count_before_null = mg_get_tracked_count();
    mg_free(NULL);
    TEST_ASSERT(mg_get_tracked_count() == count_before_null, "mg_free(NULL) did not alter registry");

    /* 3. Handling zero-size allocation safely */
    size_t count_before_zero = mg_get_tracked_count();
    void *zero_ptr = mg_malloc(0);
    if (zero_ptr != NULL) {
        /* If system malloc(0) returns a non-null pointer, verify tracking and cleanup */
        TEST_ASSERT(mg_is_tracked(zero_ptr) == 1, "Zero-size allocation is registered if non-null");
        TEST_ASSERT(mg_get_allocation_size(zero_ptr) == 0, "Zero-size allocation records size 0");
        TEST_ASSERT(mg_get_tracked_count() == count_before_zero + 1, "Zero-size allocation added to registry");
        mg_free(zero_ptr);
        TEST_ASSERT(mg_get_tracked_count() == count_before_zero, "Zero-size allocation untracked after free");
    } else {
        /* If system malloc(0) returns NULL, verify NULL behavior */
        TEST_ASSERT(mg_is_tracked(NULL) == 0, "NULL result from mg_malloc(0) is not tracked");
        TEST_ASSERT(mg_get_tracked_count() == count_before_zero, "Registry count unchanged on NULL return");
    }

    /* 4. Untracked stack pointer must not be tracked and mg_free must not crash */
    int stack_var = 12345;
    size_t count_before_stack = mg_get_tracked_count();
    TEST_ASSERT(mg_is_tracked(&stack_var) == 0, "Stack variable address is not tracked");
    TEST_ASSERT(mg_get_allocation_size(&stack_var) == 0, "Stack variable size is 0");
    mg_free(&stack_var);
    TEST_ASSERT(mg_get_tracked_count() == count_before_stack, "mg_free on stack pointer safely ignored");

    /* 5. Interior pointer must not be recognized as base address */
    void *buf = mg_malloc(100);
    TEST_ASSERT(buf != NULL, "Allocated 100 bytes");
    void *interior = (char *)buf + 20;
    TEST_ASSERT(mg_is_tracked(interior) == 0, "Interior offset pointer is not tracked");
    TEST_ASSERT(mg_get_allocation_size(interior) == 0, "Interior offset size is 0");

    /* Freeing interior pointer must be ignored, preserving base allocation */
    size_t count_before_free_interior = mg_get_tracked_count();
    mg_free(interior);
    TEST_ASSERT(mg_get_tracked_count() == count_before_free_interior, "Registry unchanged after mg_free on interior pointer");
    TEST_ASSERT(mg_is_tracked(buf) == 1, "Base allocation still tracked after mg_free on interior pointer");

    /* Clean up base allocation */
    size_t count_before_free_buf = mg_get_tracked_count();
    mg_free(buf);
    TEST_ASSERT(mg_get_tracked_count() == count_before_free_buf - 1, "Base allocation freed successfully");
}

/**
 * @brief Tests heap boundary validation (mg_is_valid_access) including base,
 *        interior pointers, boundaries, overflows, NULL, and zero-size accesses.
 */
static void test_boundary_validation(void) {
    size_t alloc_size = 100;
    size_t initial_count = mg_get_tracked_count();
    char *buf = (char *)mg_malloc(alloc_size);
    TEST_ASSERT(buf != NULL, "Allocated 100 bytes for boundary validation tests");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Tracked count incremented");

    /* 1. Valid access from base address */
    TEST_ASSERT(mg_is_valid_access(buf, 1) == 1, "Access 1 byte from base is valid");
    TEST_ASSERT(mg_is_valid_access(buf, 10) == 1, "Access 10 bytes from base is valid");
    TEST_ASSERT(mg_is_valid_access(buf, 50) == 1, "Access 50 bytes from base is valid");
    TEST_ASSERT(mg_is_valid_access(buf, 100) == 1, "Access full 100 bytes from base is valid");

    /* 2. Valid access through an interior pointer */
    TEST_ASSERT(mg_is_valid_access(buf + 20, 10) == 1, "Access 10 bytes starting at offset 20 is valid");
    TEST_ASSERT(mg_is_valid_access(buf + 20, 80) == 1, "Access 80 bytes starting at offset 20 is valid");
    TEST_ASSERT(mg_is_valid_access(buf + 99, 1) == 1, "Access 1 byte at last index 99 is valid");

    /* 3. Access ending exactly at the allocation boundary */
    TEST_ASSERT(mg_is_valid_access(buf, 100) == 1, "Access from 0 to exactly boundary (100) is valid");
    TEST_ASSERT(mg_is_valid_access(buf + 50, 50) == 1, "Access from offset 50 to boundary (100) is valid");
    TEST_ASSERT(mg_is_valid_access(buf + 99, 1) == 1, "Access from offset 99 to boundary (100) is valid");

    /* 4. Access extending beyond the boundary */
    TEST_ASSERT(mg_is_valid_access(buf, 101) == 0, "Access 101 bytes from base (exceeds boundary) is rejected");
    TEST_ASSERT(mg_is_valid_access(buf + 20, 81) == 0, "Access 81 bytes from offset 20 (exceeds boundary) is rejected");
    TEST_ASSERT(mg_is_valid_access(buf + 50, 51) == 0, "Access 51 bytes from offset 50 (exceeds boundary) is rejected");
    TEST_ASSERT(mg_is_valid_access(buf + 99, 2) == 0, "Access 2 bytes from offset 99 (exceeds boundary) is rejected");

    /* 5. Access starting at the exact allocation boundary using the valid one-past pointer expression */
    const void *boundary_ptr = buf + alloc_size;
    TEST_ASSERT(mg_is_valid_access(boundary_ptr, 1) == 0, "Access 1 byte starting at boundary is rejected");
    TEST_ASSERT(mg_is_valid_access(boundary_ptr, 10) == 0, "Access 10 bytes starting at boundary is rejected");
    TEST_ASSERT(mg_is_valid_access(boundary_ptr, 0) == 0, "Zero-size access at boundary is rejected");

    /* Untracked stack pointer (outside all tracked allocations) */
    int stack_var = 42;
    TEST_ASSERT(mg_is_valid_access(&stack_var, sizeof(stack_var)) == 0, "Stack pointer access is rejected");

    /* 6. NULL pointer checks */
    TEST_ASSERT(mg_is_valid_access(NULL, 1) == 0, "Access with NULL pointer is rejected");
    TEST_ASSERT(mg_is_valid_access(NULL, 10) == 0, "Access 10 bytes with NULL pointer is rejected");
    TEST_ASSERT(mg_is_valid_access(NULL, 0) == 0, "Zero-size access with NULL pointer is rejected");

    /* 7. Address calculation overflow check with SIZE_MAX */
    TEST_ASSERT(mg_is_valid_access(buf, SIZE_MAX) == 0, "SIZE_MAX access size causing address overflow is rejected");

    /* 8. Zero-size access semantics:
     * Valid if and only if ptr is non-NULL and points to a valid allocated byte within an active allocation.
     */
    TEST_ASSERT(mg_is_valid_access(buf, 0) == 1, "Zero-size access at allocation base is valid");
    TEST_ASSERT(mg_is_valid_access(buf + 20, 0) == 1, "Zero-size access at interior offset is valid");
    TEST_ASSERT(mg_is_valid_access(buf + 99, 0) == 1, "Zero-size access at last allocated byte is valid");
    TEST_ASSERT(mg_is_valid_access(&stack_var, 0) == 0, "Zero-size access on untracked pointer is rejected");

    /* Clean up safely using mg_get_tracked_count() without accessing buf after free */
    mg_free(buf);
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored to initial after freeing buf");
}

/**
 * @brief Tests external allocation support: registration, unregistration,
 *        reallocation, failure handling, and boundary checks.
 */
static void test_external_allocations(void) {
    size_t initial_count = mg_get_tracked_count();

    /* 1. Register a standard malloc allocation */
    size_t ext_size = 120;
    void *ext_p = malloc(ext_size);
    TEST_ASSERT(ext_p != NULL, "Standard malloc succeeded for external registration test");
    TEST_ASSERT(mg_register_external(ext_p, ext_size) == 1, "mg_register_external succeeded");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Tracked count incremented after external registration");

    /* 2. Verify mg_is_tracked and mg_get_allocation_size */
    TEST_ASSERT(mg_is_tracked(ext_p) == 1, "External pointer is tracked");
    TEST_ASSERT(mg_get_allocation_size(ext_p) == ext_size, "Recorded size matches external allocation size");

    /* 3. Verify mg_is_valid_access works with external allocations */
    char *c_ext = (char *)ext_p;
    TEST_ASSERT(mg_is_valid_access(c_ext, 1) == 1, "Access 1 byte from base is valid");
    TEST_ASSERT(mg_is_valid_access(c_ext, ext_size) == 1, "Access full external buffer from base is valid");
    TEST_ASSERT(mg_is_valid_access(c_ext + 20, 50) == 1, "Access interior range is valid");
    TEST_ASSERT(mg_is_valid_access(c_ext + 60, 60) == 1, "Access ending at boundary is valid");
    TEST_ASSERT(mg_is_valid_access(c_ext, ext_size + 1) == 0, "Access exceeding boundary is rejected");
    TEST_ASSERT(mg_is_valid_access(c_ext + 100, 21) == 0, "Access starting at offset 100 with size 21 is rejected");
    TEST_ASSERT(mg_is_valid_access(c_ext, 0) == 1, "Zero-size access at base is valid");
    TEST_ASSERT(mg_is_valid_access(c_ext + ext_size - 1, 0) == 1, "Zero-size access at last byte is valid");
    TEST_ASSERT(mg_is_valid_access(c_ext + ext_size, 0) == 0, "Zero-size access at boundary one-past is rejected");

    /* 4. Reject duplicate registration */
    TEST_ASSERT(mg_register_external(ext_p, ext_size) == 0, "Duplicate registration with same size rejected");
    TEST_ASSERT(mg_register_external(ext_p, 200) == 0, "Duplicate registration with different size rejected");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Tracked count unchanged after rejected duplicate registrations");

    /* 5. Unregister external memory without freeing payload */
    TEST_ASSERT(mg_unregister_external(ext_p) == 1, "mg_unregister_external succeeded");
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored after unregistration");
    TEST_ASSERT(mg_is_tracked(ext_p) == 0, "External pointer no longer tracked");
    TEST_ASSERT(mg_get_allocation_size(ext_p) == 0, "Recorded size is 0 after unregistration");
    TEST_ASSERT(mg_is_valid_access(ext_p, ext_size) == 0, "Access validation fails after unregistration");

    /* Verify payload memory is still valid and writable by caller */
    memset(ext_p, 0x55, ext_size);
    int payload_intact = 1;
    for (size_t i = 0; i < ext_size; i++) {
        if (((unsigned char *)ext_p)[i] != 0x55) {
            payload_intact = 0;
            break;
        }
    }
    TEST_ASSERT(payload_intact == 1, "External payload remained valid and writable after unregistration");
    free(ext_p);
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count unchanged after caller freed payload");

    /* 6. Registering an unknown or invalid pointer safely */
    TEST_ASSERT(mg_register_external(NULL, 100) == 0, "Registering NULL is rejected");
    TEST_ASSERT(mg_unregister_external(NULL) == 0, "Unregistering NULL returns 0");
    int stack_var = 999;
    TEST_ASSERT(mg_unregister_external(&stack_var) == 0, "Unregistering untracked stack pointer returns 0");
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count unchanged after invalid operations");

    /* Test zero-sized external allocation */
    void *zero_p = malloc(0);
    if (zero_p != NULL) {
        TEST_ASSERT(mg_register_external(zero_p, 0) == 1, "Registering zero-sized external allocation succeeded");
        TEST_ASSERT(mg_is_tracked(zero_p) == 1, "Zero-sized external allocation is tracked");
        TEST_ASSERT(mg_get_allocation_size(zero_p) == 0, "Zero-sized external allocation size is 0");
        TEST_ASSERT(mg_is_valid_access(zero_p, 0) == 0, "Zero-size access on zero-sized allocation is invalid");
        TEST_ASSERT(mg_unregister_external(zero_p) == 1, "Unregistering zero-sized external allocation succeeded");
        free(zero_p);
    }
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count clean after zero-sized test");

    /* 7. Reallocate a registered allocation successfully */
    size_t orig_size = 64;
    size_t new_size = 256;
    void *orig_p = malloc(orig_size);
    TEST_ASSERT(orig_p != NULL, "Allocated buffer for successful realloc test");
    memset(orig_p, 0x42, orig_size);
    TEST_ASSERT(mg_register_external(orig_p, orig_size) == 1, "Registered buffer before realloc");

    void *realloc_p = mg_realloc_external(orig_p, new_size);
    TEST_ASSERT(realloc_p != NULL, "mg_realloc_external succeeded");
    TEST_ASSERT(mg_is_tracked(realloc_p) == 1, "Reallocated pointer is tracked in registry");
    TEST_ASSERT(mg_get_allocation_size(realloc_p) == new_size, "Registry size updated to new_size");
    TEST_ASSERT(mg_is_valid_access(realloc_p, new_size) == 1, "Full new range is valid access");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Registry count maintained as 1 allocation");

    /* Verify original data preserved in reallocated block without using orig_p */
    unsigned char *realloc_bytes = (unsigned char *)realloc_p;
    int realloc_data_ok = 1;
    for (size_t i = 0; i < orig_size; i++) {
        if (realloc_bytes[i] != 0x42) {
            realloc_data_ok = 0;
            break;
        }
    }
    TEST_ASSERT(realloc_data_ok == 1, "Original data preserved across successful realloc");
    /* Write into expanded area */
    memset(realloc_bytes + orig_size, 0x7E, new_size - orig_size);

    /* Unregister and free the reallocated buffer */
    TEST_ASSERT(mg_unregister_external(realloc_p) == 1, "Unregistered reallocated buffer");
    free(realloc_p);
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored after freeing realloc_p");

    /* 8. Reallocating with failure while preserving the original registry entry */
    size_t fail_test_size = 80;
    void *fail_p = malloc(fail_test_size);
    TEST_ASSERT(fail_p != NULL, "Allocated buffer for failure realloc test");
    memset(fail_p, 0xAB, fail_test_size);
    TEST_ASSERT(mg_register_external(fail_p, fail_test_size) == 1, "Registered buffer for failure realloc test");

    /* Requesting SIZE_MAX guarantees failure on any standard allocator */
    void *fail_result = mg_realloc_external(fail_p, SIZE_MAX);
    TEST_ASSERT(fail_result == NULL, "mg_realloc_external returned NULL on impossible allocation request");
    TEST_ASSERT(mg_is_tracked(fail_p) == 1, "Original pointer is STILL tracked after realloc failure");
    TEST_ASSERT(mg_get_allocation_size(fail_p) == fail_test_size, "Original size preserved in registry after realloc failure");
    TEST_ASSERT(mg_is_valid_access(fail_p, fail_test_size) == 1, "Original boundary validation preserved after realloc failure");
    TEST_ASSERT(((unsigned char *)fail_p)[0] == 0xAB, "Original buffer content intact after realloc failure");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Registry count unchanged after realloc failure");

    /* Clean up failure test allocation */
    TEST_ASSERT(mg_unregister_external(fail_p) == 1, "Unregistered failure test pointer");
    free(fail_p);
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored after freeing fail_p");

    /* 9. Reallocating an unregistered pointer without registering it */
    void *unreg_p = malloc(32);
    TEST_ASSERT(unreg_p != NULL, "Allocated unregistered pointer");
    TEST_ASSERT(mg_is_tracked(unreg_p) == 0, "Pointer is initially untracked");

    void *unreg_realloc = mg_realloc_external(unreg_p, 128);
    TEST_ASSERT(unreg_realloc != NULL, "Reallocation of unregistered pointer succeeded");
    TEST_ASSERT(mg_is_tracked(unreg_realloc) == 0, "Reallocated untracked pointer remains untracked");
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count unchanged when reallocating unregistered pointer");
    free(unreg_realloc);

    /* Handling NULL input with mg_realloc_external */
    void *null_realloc = mg_realloc_external(NULL, 64);
    TEST_ASSERT(null_realloc != NULL, "mg_realloc_external(NULL, size) behaves like malloc");
    TEST_ASSERT(mg_is_tracked(null_realloc) == 0, "Buffer from NULL realloc is not automatically registered");
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count unchanged for NULL realloc");
    free(null_realloc);

    /* 10. Platform-specific realloc(ptr, 0) handling */
    size_t zero_realloc_size = 48;
    void *zero_target_p = malloc(zero_realloc_size);
    TEST_ASSERT(zero_target_p != NULL, "Allocated buffer for realloc(ptr, 0) test");
    TEST_ASSERT(mg_register_external(zero_target_p, zero_realloc_size) == 1, "Registered buffer for realloc(ptr, 0) test");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Registry count incremented before realloc(ptr, 0)");

    uintptr_t zero_old_addr = (uintptr_t)zero_target_p;
    void *zero_res = mg_realloc_external(zero_target_p, 0);
    if (zero_res == NULL) {
        /* Standard platform behavior (e.g. glibc): buffer was freed and NULL returned */
        TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry entry removed when realloc(ptr, 0) returns NULL");
        TEST_ASSERT(mg_is_tracked((const void *)zero_old_addr) == 0, "Original pointer no longer tracked");
    } else {
        /* Platform returned a distinct non-dereferenceable zero-sized pointer */
        TEST_ASSERT(mg_is_tracked(zero_res) == 1, "Zero-size realloc pointer is tracked");
        TEST_ASSERT(mg_get_allocation_size(zero_res) == 0, "Zero-size realloc pointer size is 0");
        TEST_ASSERT(mg_unregister_external(zero_res) == 1, "Unregistered zero-size pointer");
        free(zero_res);
        TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry restored to initial");
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Final registry count matches initial count");
}

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
static int check_access_aborts(const void *ptr, size_t access_size) {
    pid_t pid = fork();
    if (pid < 0) {
        return 0;
    }
    if (pid == 0) {
        /* In child process: silence stderr so expected failure messages don't clutter test output */
        if (freopen("/dev/null", "w", stderr) == NULL) {
            /* ignore if freopen fails */
        }
        mg_check_access(ptr, access_size);
        exit(0); /* If mg_check_access did not abort, exit 0 */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    /* Should terminate via SIGABRT */
    return WIFSIGNALED(status) && (WTERMSIG(status) == SIGABRT);
}
#endif

/**
 * @brief Tests mg_check_access validation and abort behavior.
 */
static void test_check_access(void) {
    size_t alloc_size = 64;
    void *buf = mg_malloc(alloc_size);
    TEST_ASSERT(buf != NULL, "Allocated buffer for mg_check_access test");

    /* 1. Valid accesses must succeed without aborting */
    mg_check_access(buf, 1);
    mg_check_access(buf, alloc_size);
    mg_check_access((char *)buf + 10, 20);
    mg_check_access((char *)buf + alloc_size - 1, 1);
    TEST_ASSERT(1, "Valid accesses succeeded through mg_check_access without aborting");

    /* 2. External allocation valid accesses */
    void *ext_buf = malloc(128);
    TEST_ASSERT(ext_buf != NULL, "Allocated external buffer");
    TEST_ASSERT(mg_register_external(ext_buf, 128) == 1, "Registered external buffer");
    mg_check_access(ext_buf, 128);
    mg_check_access((char *)ext_buf + 50, 50);
    TEST_ASSERT(1, "Valid accesses to external buffer succeeded through mg_check_access");
    TEST_ASSERT(mg_unregister_external(ext_buf) == 1, "Unregistered external buffer");
    free(ext_buf);

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
    /* 3. Out-of-bounds access must abort */
    TEST_ASSERT(check_access_aborts(buf, alloc_size + 1),
                "mg_check_access aborts on access exceeding allocation size");
    TEST_ASSERT(check_access_aborts((char *)buf + 50, 20),
                "mg_check_access aborts on interior access extending past end");
    TEST_ASSERT(check_access_aborts((char *)buf + alloc_size, 1),
                "mg_check_access aborts on access starting at boundary");

    /* 4. NULL pointer access must abort */
    TEST_ASSERT(check_access_aborts(NULL, 1),
                "mg_check_access aborts on NULL pointer");

    /* 5. Untracked stack variable must abort */
    int stack_var = 42;
    TEST_ASSERT(check_access_aborts(&stack_var, sizeof(stack_var)),
                "mg_check_access aborts on untracked stack variable");

    /* 6. Arithmetic overflow must abort */
    TEST_ASSERT(check_access_aborts(buf, SIZE_MAX),
                "mg_check_access aborts on SIZE_MAX access overflow");
#endif

    /* 7. Clean up and test access after free */
    mg_free(buf);

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
    /* Buffer is now freed and no longer tracked; access must abort */
    TEST_ASSERT(check_access_aborts(buf, 1),
                "mg_check_access aborts on access to deallocated buffer");
#endif
}

/**
 * @brief Tests the allocator interceptor APIs (mg_intercept_malloc,
 *        mg_intercept_calloc, mg_intercept_realloc, mg_intercept_free).
 */
static void test_interceptor_apis(void) {
    size_t initial_count = mg_get_tracked_count();

    /* =========================================================================
     * 1. mg_intercept_malloc
     * ========================================================================= */
    size_t m_size = 128;
    void *m_ptr = mg_intercept_malloc(m_size);
    TEST_ASSERT(m_ptr != NULL, "mg_intercept_malloc returned non-NULL");
    TEST_ASSERT(mg_is_tracked(m_ptr) == 1, "mg_intercept_malloc buffer is tracked in registry");
    TEST_ASSERT(mg_get_allocation_size(m_ptr) == m_size, "mg_intercept_malloc recorded size matches requested size");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Registry count incremented after intercept_malloc");

    /* Validate memory readability, writability, and boundary checks */
    memset(m_ptr, 0x33, m_size);
    TEST_ASSERT(((unsigned char *)m_ptr)[0] == 0x33, "Buffer is writable");
    TEST_ASSERT(mg_is_valid_access(m_ptr, 1) == 1, "Access 1 byte is valid");
    TEST_ASSERT(mg_is_valid_access(m_ptr, m_size) == 1, "Access full size is valid");
    TEST_ASSERT(mg_is_valid_access((char *)m_ptr + 20, 50) == 1, "Interior access is valid");
    TEST_ASSERT(mg_is_valid_access(m_ptr, m_size + 1) == 0, "Access beyond boundary rejected");

    /* Deallocate via mg_intercept_free */
    mg_intercept_free(m_ptr);
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry count restored after mg_intercept_free");
    TEST_ASSERT(mg_is_tracked(m_ptr) == 0, "Buffer no longer tracked after mg_intercept_free");
    TEST_ASSERT(mg_is_valid_access(m_ptr, 1) == 0, "Access to freed buffer rejected");

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
    TEST_ASSERT(check_access_aborts(m_ptr, 1),
                "mg_check_access aborts on use-after-free following mg_intercept_free");
#endif

    /* Zero-size intercept_malloc */
    void *m_zero = mg_intercept_malloc(0);
    if (m_zero != NULL) {
        TEST_ASSERT(mg_is_tracked(m_zero) == 1, "Zero-sized malloc tracked if non-NULL");
        TEST_ASSERT(mg_get_allocation_size(m_zero) == 0, "Zero-sized malloc recorded size is 0");
        mg_intercept_free(m_zero);
        TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry restored after zero-sized free");
    }

    /* Extreme size failure test */
    void *m_fail = mg_intercept_malloc(SIZE_MAX);
    TEST_ASSERT(m_fail == NULL, "mg_intercept_malloc(SIZE_MAX) returns NULL");
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry count unchanged on malloc failure");

    /* =========================================================================
     * 2. mg_intercept_calloc
     * ========================================================================= */
    size_t c_nmemb = 16;
    size_t c_elem_size = 4;
    size_t c_total = c_nmemb * c_elem_size; /* 64 bytes */
    void *c_ptr = mg_intercept_calloc(c_nmemb, c_elem_size);
    TEST_ASSERT(c_ptr != NULL, "mg_intercept_calloc returned non-NULL");
    TEST_ASSERT(mg_is_tracked(c_ptr) == 1, "calloc buffer is tracked");
    TEST_ASSERT(mg_get_allocation_size(c_ptr) == c_total, "calloc recorded size matches nmemb * size");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Registry count incremented by 1");

    /* Verify zero-initialization */
    int all_zeros = 1;
    unsigned char *c_bytes = (unsigned char *)c_ptr;
    for (size_t i = 0; i < c_total; i++) {
        if (c_bytes[i] != 0) {
            all_zeros = 0;
            break;
        }
    }
    TEST_ASSERT(all_zeros == 1, "mg_intercept_calloc memory is zero-initialized");
    TEST_ASSERT(mg_is_valid_access(c_ptr, c_total) == 1, "Full calloc buffer range is valid access");
    TEST_ASSERT(mg_is_valid_access(c_ptr, c_total + 1) == 0, "Access beyond calloc boundary is rejected");

    mg_intercept_free(c_ptr);
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry count restored after freeing calloc buffer");

    /* Calloc zero element count or size */
    void *c_zero1 = mg_intercept_calloc(0, 10);
    if (c_zero1 != NULL) {
        TEST_ASSERT(mg_is_tracked(c_zero1) == 1, "calloc(0, 10) tracked if non-NULL");
        mg_intercept_free(c_zero1);
    }
    void *c_zero2 = mg_intercept_calloc(10, 0);
    if (c_zero2 != NULL) {
        TEST_ASSERT(mg_is_tracked(c_zero2) == 1, "calloc(10, 0) tracked if non-NULL");
        mg_intercept_free(c_zero2);
    }
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry clean after zero calloc tests");

    /* Verify that registry remains unchanged across all calloc failure paths.
     * Establish an active sentinel allocation to ensure existing entries are unaffected. */
    void *sentinel = mg_intercept_malloc(64);
    TEST_ASSERT(sentinel != NULL, "Sentinel allocation succeeded before calloc failure tests");
    size_t count_with_sentinel = mg_get_tracked_count();
    TEST_ASSERT(count_with_sentinel == initial_count + 1, "Registry count has exactly sentinel allocation");

    /* 1. Multiplication overflow rejection with SIZE_MAX */
    void *c_ov1 = mg_intercept_calloc(SIZE_MAX, 2);
    TEST_ASSERT(c_ov1 == NULL, "mg_intercept_calloc(SIZE_MAX, 2) rejects overflow and returns NULL");
    TEST_ASSERT(mg_get_tracked_count() == count_with_sentinel, "Registry unchanged after overflow rejection (SIZE_MAX, 2)");
    TEST_ASSERT(mg_is_tracked(sentinel) == 1, "Sentinel allocation remains tracked after calloc overflow");
    TEST_ASSERT(mg_is_valid_access(sentinel, 64) == 1, "Sentinel access remains valid after calloc overflow");

    /* 2. Multiplication overflow with non-trivial operands */
    void *c_ov2 = mg_intercept_calloc(SIZE_MAX / 2 + 1, 3);
    TEST_ASSERT(c_ov2 == NULL, "mg_intercept_calloc rejects multiplication overflow");
    TEST_ASSERT(mg_get_tracked_count() == count_with_sentinel, "Registry unchanged after overflow rejection");

    /* 3. Extreme overflow with both operands large */
    void *c_ov3 = mg_intercept_calloc(SIZE_MAX, SIZE_MAX);
    TEST_ASSERT(c_ov3 == NULL, "mg_intercept_calloc(SIZE_MAX, SIZE_MAX) rejects overflow");
    TEST_ASSERT(mg_get_tracked_count() == count_with_sentinel, "Registry unchanged after overflow rejection");

    /* 4. Single extreme element size failure (allocation failure path where calloc returns NULL) */
    void *c_fail = mg_intercept_calloc(1, SIZE_MAX);
    TEST_ASSERT(c_fail == NULL, "mg_intercept_calloc(1, SIZE_MAX) returns NULL on allocation failure");
    TEST_ASSERT(mg_get_tracked_count() == count_with_sentinel, "Registry unchanged on calloc failure");
    TEST_ASSERT(mg_is_tracked(sentinel) == 1, "Sentinel remains tracked after allocation failure");
    TEST_ASSERT(mg_get_allocation_size(sentinel) == 64, "Sentinel size unchanged after allocation failure");

    /* Clean up sentinel */
    mg_intercept_free(sentinel);
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry restored to initial after freeing sentinel");

    /* =========================================================================
     * 3. mg_intercept_realloc
     * ========================================================================= */
    /* 3a. NULL pointer behaves as malloc: allocates and registers */
    size_t r_init_size = 64;
    void *r_ptr = mg_intercept_realloc(NULL, r_init_size);
    TEST_ASSERT(r_ptr != NULL, "mg_intercept_realloc(NULL, size) behaves like malloc");
    TEST_ASSERT(mg_is_tracked(r_ptr) == 1, "Buffer from NULL realloc is registered");
    TEST_ASSERT(mg_get_allocation_size(r_ptr) == r_init_size, "Registered size matches requested size");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Registry count incremented by 1");

    memset(r_ptr, 0x77, r_init_size);

    /* 3b. Successful expansion / relocation */
    size_t r_expanded_size = 256;
    void *r_expanded = mg_intercept_realloc(r_ptr, r_expanded_size);
    TEST_ASSERT(r_expanded != NULL, "mg_intercept_realloc expansion succeeded");
    TEST_ASSERT(mg_is_tracked(r_expanded) == 1, "Expanded buffer is tracked");
    TEST_ASSERT(mg_get_allocation_size(r_expanded) == r_expanded_size, "Expanded size updated in registry");
    TEST_ASSERT(mg_is_valid_access(r_expanded, r_expanded_size) == 1, "Full expanded range is valid");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Registry maintains single allocation entry");

    /* Verify original data preserved in expanded buffer without reusing r_ptr */
    unsigned char *exp_bytes = (unsigned char *)r_expanded;
    int data_preserved = 1;
    for (size_t i = 0; i < r_init_size; i++) {
        if (exp_bytes[i] != 0x77) {
            data_preserved = 0;
            break;
        }
    }
    TEST_ASSERT(data_preserved == 1, "Original data preserved across realloc expansion");

    /* 3c. Successful shrinking */
    size_t r_shrunk_size = 32;
    void *r_shrunk = mg_intercept_realloc(r_expanded, r_shrunk_size);
    TEST_ASSERT(r_shrunk != NULL, "mg_intercept_realloc shrinking succeeded");
    TEST_ASSERT(mg_is_tracked(r_shrunk) == 1, "Shrunk buffer is tracked");
    TEST_ASSERT(mg_get_allocation_size(r_shrunk) == r_shrunk_size, "Shrunk size updated in registry");
    TEST_ASSERT(mg_is_valid_access(r_shrunk, r_shrunk_size) == 1, "Shrunk size is valid access");
    TEST_ASSERT(mg_is_valid_access(r_shrunk, r_shrunk_size + 1) == 0, "Access beyond shrunk size is rejected");

    /* 3d. Reallocation failure: original buffer and registry preserved untouched */
    void *r_fail = mg_intercept_realloc(r_shrunk, SIZE_MAX);
    TEST_ASSERT(r_fail == NULL, "mg_intercept_realloc(..., SIZE_MAX) returns NULL");
    TEST_ASSERT(mg_is_tracked(r_shrunk) == 1, "Original pointer is STILL tracked after realloc failure");
    TEST_ASSERT(mg_get_allocation_size(r_shrunk) == r_shrunk_size, "Original size preserved after realloc failure");
    TEST_ASSERT(mg_is_valid_access(r_shrunk, r_shrunk_size) == 1, "Original range remains valid after realloc failure");
    TEST_ASSERT(((unsigned char *)r_shrunk)[0] == 0x77, "Original buffer content intact after realloc failure");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Registry count unchanged on realloc failure");

    /* Clean up shrunk buffer */
    mg_intercept_free(r_shrunk);
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry count restored after freeing shrunk buffer");

    /* 3e. Reallocating untracked memory */
    void *untracked_buf = malloc(48);
    TEST_ASSERT(untracked_buf != NULL, "Allocated untracked buffer for realloc test");
    TEST_ASSERT(mg_is_tracked(untracked_buf) == 0, "Buffer is initially untracked");

    void *untracked_realloc = mg_intercept_realloc(untracked_buf, 96);
    TEST_ASSERT(untracked_realloc != NULL, "Reallocating untracked buffer succeeded");
    TEST_ASSERT(mg_is_tracked(untracked_realloc) == 0, "Untracked buffer remains untracked");
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry count unchanged");
    mg_intercept_free(untracked_realloc);

    /* 3f. Reallocation with size == 0 */
    void *zero_target = mg_intercept_malloc(50);
    TEST_ASSERT(zero_target != NULL, "Allocated buffer for realloc(ptr, 0)");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Count incremented");

    void *zero_realloc_res = mg_intercept_realloc(zero_target, 0);
    if (zero_realloc_res == NULL) {
        /* Standard glibc: buffer freed and NULL returned */
        TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry entry cleaned up on realloc(ptr, 0) NULL return");
        TEST_ASSERT(mg_is_tracked(zero_target) == 0, "Pointer no longer tracked");
    } else {
        /* Non-null zero-sized buffer */
        TEST_ASSERT(mg_is_tracked(zero_realloc_res) == 1, "Non-null zero realloc tracked");
        TEST_ASSERT(mg_get_allocation_size(zero_realloc_res) == 0, "Recorded size is 0");
        mg_intercept_free(zero_realloc_res);
        TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry restored");
    }

    /* =========================================================================
     * 4. mg_intercept_free
     * ========================================================================= */
    /* 4a. NULL pointer free is safe */
    size_t count_before_null = mg_get_tracked_count();
    mg_intercept_free(NULL);
    TEST_ASSERT(mg_get_tracked_count() == count_before_null, "mg_intercept_free(NULL) did not alter registry");

    /* 4b. Freeing an untracked pointer calls libc free without error */
    void *untracked_to_free = malloc(64);
    TEST_ASSERT(untracked_to_free != NULL, "Allocated untracked buffer to test free");
    TEST_ASSERT(mg_is_tracked(untracked_to_free) == 0, "Buffer is untracked");
    mg_intercept_free(untracked_to_free);
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry count unchanged after freeing untracked pointer");

    /* Final assertion count check */
    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry count perfectly restored after all interceptor tests");
}

/* =========================================================================
 * Multithreaded Registry Tests
 * ========================================================================= */

typedef struct {
    int thread_id;
    int assertions;
    int failures;
} ThreadTestArg;

#define THREAD_ASSERT(arg, cond, msg) do { \
    (arg)->assertions++; \
    if (!(cond)) { \
        fprintf(stderr, "  [FAIL Thread %d] Line %d: %s (%s)\n", (arg)->thread_id, __LINE__, msg, #cond); \
        (arg)->failures++; \
    } \
} while (0)

/* --- Test 11: Concurrent mg_malloc and mg_free across multiple threads --- */
#define NUM_MALLOC_THREADS 8
#define ALLOCS_PER_THREAD 50

static void *thread_worker_malloc_free(void *arg_ptr) {
    ThreadTestArg *arg = (ThreadTestArg *)arg_ptr;
    void *ptrs[ALLOCS_PER_THREAD];
    size_t sizes[ALLOCS_PER_THREAD];

    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        sizes[i] = 16 + ((size_t)i * 17) % 256;
        ptrs[i] = mg_malloc(sizes[i]);
        THREAD_ASSERT(arg, ptrs[i] != NULL, "Thread mg_malloc returned non-NULL");
        if (ptrs[i] != NULL) {
            THREAD_ASSERT(arg, mg_is_tracked(ptrs[i]) == 1, "Allocated ptr is tracked");
            THREAD_ASSERT(arg, mg_get_allocation_size(ptrs[i]) == sizes[i], "Recorded size matches");
            THREAD_ASSERT(arg, mg_is_valid_access(ptrs[i], sizes[i]) == 1, "Full access is valid");
            THREAD_ASSERT(arg, mg_is_valid_access(ptrs[i], sizes[i] + 1) == 0, "OOB access is invalid");

            /* Write and verify test pattern */
            memset(ptrs[i], 0x5A + arg->thread_id, sizes[i]);
            unsigned char *byte_ptr = (unsigned char *)ptrs[i];
            THREAD_ASSERT(arg, byte_ptr[0] == (unsigned char)(0x5A + arg->thread_id), "First byte pattern matches");
            THREAD_ASSERT(arg, byte_ptr[sizes[i] - 1] == (unsigned char)(0x5A + arg->thread_id), "Last byte pattern matches");
        }
    }

    /* Free all allocations in reverse order */
    for (int i = ALLOCS_PER_THREAD - 1; i >= 0; i--) {
        if (ptrs[i] != NULL) {
            mg_free(ptrs[i]);
            THREAD_ASSERT(arg, mg_is_tracked(ptrs[i]) == 0, "Freed pointer is no longer tracked");
            THREAD_ASSERT(arg, mg_is_valid_access(ptrs[i], 1) == 0, "Freed pointer access is invalid");
        }
    }

    return NULL;
}

static void test_multithreaded_malloc_free(void) {
    size_t initial_count = mg_get_tracked_count();
    pthread_t threads[NUM_MALLOC_THREADS];
    ThreadTestArg thread_args[NUM_MALLOC_THREADS];

    for (int i = 0; i < NUM_MALLOC_THREADS; i++) {
        thread_args[i].thread_id = i;
        thread_args[i].assertions = 0;
        thread_args[i].failures = 0;
        int rc = pthread_create(&threads[i], NULL, thread_worker_malloc_free, &thread_args[i]);
        TEST_ASSERT(rc == 0, "pthread_create succeeded for malloc/free worker");
    }

    for (int i = 0; i < NUM_MALLOC_THREADS; i++) {
        pthread_join(threads[i], NULL);
        total_assertions += thread_args[i].assertions;
        failed_assertions += thread_args[i].failures;
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count,
                "Tracked count perfectly restored after multithreaded malloc/free");
}

/* --- Test 12: Concurrent external registration, reallocation, and duplicate races --- */
#define NUM_EXT_THREADS 8
#define EXT_ALLOCS_PER_THREAD 20

static void *thread_worker_external_realloc(void *arg_ptr) {
    ThreadTestArg *arg = (ThreadTestArg *)arg_ptr;
    void *ptrs[EXT_ALLOCS_PER_THREAD];

    for (int i = 0; i < EXT_ALLOCS_PER_THREAD; i++) {
        size_t orig_size = 32 + (size_t)i * 8;
        ptrs[i] = malloc(orig_size);
        THREAD_ASSERT(arg, ptrs[i] != NULL, "Standard malloc succeeded in worker");

        int reg_ok = mg_register_external(ptrs[i], orig_size);
        THREAD_ASSERT(arg, reg_ok == 1, "External registration succeeded");
        THREAD_ASSERT(arg, mg_is_tracked(ptrs[i]) == 1, "External pointer is tracked");
        THREAD_ASSERT(arg, mg_get_allocation_size(ptrs[i]) == orig_size, "Initial size recorded");

        /* Reallocate to 3x size */
        size_t new_size = orig_size * 3;
        void *realloc_ptr = mg_realloc_external(ptrs[i], new_size);
        THREAD_ASSERT(arg, realloc_ptr != NULL, "Reallocation succeeded");
        if (realloc_ptr != NULL) {
            ptrs[i] = realloc_ptr;
            THREAD_ASSERT(arg, mg_is_tracked(ptrs[i]) == 1, "Reallocated pointer tracked");
            THREAD_ASSERT(arg, mg_get_allocation_size(ptrs[i]) == new_size, "Reallocated size updated");
            THREAD_ASSERT(arg, mg_is_valid_access(ptrs[i], new_size) == 1, "Reallocated range is valid");
        }

        /* Unregister and free */
        int unreg_ok = mg_unregister_external(ptrs[i]);
        THREAD_ASSERT(arg, unreg_ok == 1, "Unregistration succeeded");
        THREAD_ASSERT(arg, mg_is_tracked(ptrs[i]) == 0, "Unregistered pointer no longer tracked");
        free(ptrs[i]);
    }

    return NULL;
}

typedef struct {
    int thread_id;
    void *shared_buffer;
    size_t buffer_size;
    int reg_result;
} DuplicateRaceArg;

static void *thread_worker_duplicate_race(void *arg_ptr) {
    DuplicateRaceArg *arg = (DuplicateRaceArg *)arg_ptr;
    arg->reg_result = mg_register_external(arg->shared_buffer, arg->buffer_size);
    return NULL;
}

static void test_multithreaded_external_and_realloc(void) {
    size_t initial_count = mg_get_tracked_count();

    /* 1. Concurrent registration and reallocation across independent pointers */
    pthread_t threads[NUM_EXT_THREADS];
    ThreadTestArg thread_args[NUM_EXT_THREADS];

    for (int i = 0; i < NUM_EXT_THREADS; i++) {
        thread_args[i].thread_id = i;
        thread_args[i].assertions = 0;
        thread_args[i].failures = 0;
        int rc = pthread_create(&threads[i], NULL, thread_worker_external_realloc, &thread_args[i]);
        TEST_ASSERT(rc == 0, "pthread_create succeeded for external worker");
    }

    for (int i = 0; i < NUM_EXT_THREADS; i++) {
        pthread_join(threads[i], NULL);
        total_assertions += thread_args[i].assertions;
        failed_assertions += thread_args[i].failures;
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count,
                "Registry count restored after concurrent external registration/realloc");

    /* 2. Concurrent race on duplicate registration of the exact same pointer */
    void *shared_buf = malloc(128);
    TEST_ASSERT(shared_buf != NULL, "Allocated shared buffer for duplicate registration test");

    DuplicateRaceArg race_args[NUM_EXT_THREADS];
    for (int i = 0; i < NUM_EXT_THREADS; i++) {
        race_args[i].thread_id = i;
        race_args[i].shared_buffer = shared_buf;
        race_args[i].buffer_size = 128;
        race_args[i].reg_result = -1;
        pthread_create(&threads[i], NULL, thread_worker_duplicate_race, &race_args[i]);
    }

    int success_count = 0;
    int fail_count = 0;
    for (int i = 0; i < NUM_EXT_THREADS; i++) {
        pthread_join(threads[i], NULL);
        if (race_args[i].reg_result == 1) {
            success_count++;
        } else if (race_args[i].reg_result == 0) {
            fail_count++;
        }
    }

    TEST_ASSERT(success_count == 1, "Exactly one thread succeeded in registering the shared pointer");
    TEST_ASSERT(fail_count == NUM_EXT_THREADS - 1, "All other threads failed with 0 on duplicate registration");
    TEST_ASSERT(mg_is_tracked(shared_buf) == 1, "Shared buffer is registered");
    TEST_ASSERT(mg_get_tracked_count() == initial_count + 1, "Tracked count increased by exactly 1");

    int unreg = mg_unregister_external(shared_buf);
    TEST_ASSERT(unreg == 1, "Successfully unregistered shared buffer");
    free(shared_buf);

    TEST_ASSERT(mg_get_tracked_count() == initial_count,
                "Registry count restored after duplicate registration race test");
}

/* --- Test 13: Concurrent Readers (mg_is_valid_access) and Writers (malloc/free/realloc) --- */
#define NUM_RW_READERS 4
#define NUM_RW_WRITERS 4
#define NUM_SHARED_BUFFERS 10
#define RW_ITERATIONS 100

static void *shared_rw_ptrs[NUM_SHARED_BUFFERS];
static size_t shared_rw_sizes[NUM_SHARED_BUFFERS];

static void *thread_worker_reader(void *arg_ptr) {
    ThreadTestArg *arg = (ThreadTestArg *)arg_ptr;

    for (int iter = 0; iter < RW_ITERATIONS; iter++) {
        for (int k = 0; k < NUM_SHARED_BUFFERS; k++) {
            void *p = shared_rw_ptrs[k];
            size_t sz = shared_rw_sizes[k];

            int tracked = mg_is_tracked(p);
            THREAD_ASSERT(arg, tracked == 1, "Shared buffer is tracked");

            size_t rec_sz = mg_get_allocation_size(p);
            THREAD_ASSERT(arg, rec_sz == sz, "Shared buffer size matches");

            int valid_base = mg_is_valid_access(p, sz);
            THREAD_ASSERT(arg, valid_base == 1, "Valid full access returns 1");

            int valid_half = mg_is_valid_access(p, sz / 2);
            THREAD_ASSERT(arg, valid_half == 1, "Valid interior access returns 1");

            int invalid_oob = mg_is_valid_access(p, sz + 1);
            THREAD_ASSERT(arg, invalid_oob == 0, "OOB access returns 0");

            int invalid_cross = mg_is_valid_access((char *)p + 1, sz);
            THREAD_ASSERT(arg, invalid_cross == 0, "Cross-boundary access returns 0");
        }
    }

    return NULL;
}

static void *thread_worker_writer(void *arg_ptr) {
    ThreadTestArg *arg = (ThreadTestArg *)arg_ptr;

    for (int iter = 0; iter < RW_ITERATIONS / 2; iter++) {
        /* Malloc and free private memory */
        size_t sz = 32 + ((size_t)iter * 8) % 128;
        void *p = mg_malloc(sz);
        THREAD_ASSERT(arg, p != NULL, "Writer mg_malloc succeeded");
        if (p) {
            THREAD_ASSERT(arg, mg_is_valid_access(p, sz) == 1, "Writer access valid");
            mg_free(p);
            THREAD_ASSERT(arg, mg_is_valid_access(p, sz) == 0, "Writer access after free invalid");
        }

        /* External registration and realloc private memory */
        void *ext = malloc(sz);
        THREAD_ASSERT(arg, ext != NULL, "Writer malloc succeeded");
        if (ext) {
            THREAD_ASSERT(arg, mg_register_external(ext, sz) == 1, "Writer register succeeded");
            void *realloced = mg_realloc_external(ext, sz * 2);
            THREAD_ASSERT(arg, realloced != NULL, "Writer realloc succeeded");
            if (realloced) {
                ext = realloced;
            }
            THREAD_ASSERT(arg, mg_unregister_external(ext) == 1, "Writer unregister succeeded");
            free(ext);
        }
    }

    return NULL;
}

static void test_multithreaded_concurrent_readers_writers(void) {
    size_t initial_count = mg_get_tracked_count();

    /* Pre-register shared buffers for readers */
    for (int i = 0; i < NUM_SHARED_BUFFERS; i++) {
        shared_rw_sizes[i] = (size_t)(i + 1) * 64;
        shared_rw_ptrs[i] = mg_malloc(shared_rw_sizes[i]);
        TEST_ASSERT(shared_rw_ptrs[i] != NULL, "Pre-allocated shared reader buffer");
        memset(shared_rw_ptrs[i], 0x3C, shared_rw_sizes[i]);
    }

    pthread_t reader_threads[NUM_RW_READERS];
    pthread_t writer_threads[NUM_RW_WRITERS];
    ThreadTestArg reader_args[NUM_RW_READERS];
    ThreadTestArg writer_args[NUM_RW_WRITERS];

    /* Launch readers and writers concurrently */
    for (int i = 0; i < NUM_RW_READERS; i++) {
        reader_args[i].thread_id = i;
        reader_args[i].assertions = 0;
        reader_args[i].failures = 0;
        pthread_create(&reader_threads[i], NULL, thread_worker_reader, &reader_args[i]);
    }
    for (int i = 0; i < NUM_RW_WRITERS; i++) {
        writer_args[i].thread_id = 100 + i;
        writer_args[i].assertions = 0;
        writer_args[i].failures = 0;
        pthread_create(&writer_threads[i], NULL, thread_worker_writer, &writer_args[i]);
    }

    for (int i = 0; i < NUM_RW_READERS; i++) {
        pthread_join(reader_threads[i], NULL);
        total_assertions += reader_args[i].assertions;
        failed_assertions += reader_args[i].failures;
    }
    for (int i = 0; i < NUM_RW_WRITERS; i++) {
        pthread_join(writer_threads[i], NULL);
        total_assertions += writer_args[i].assertions;
        failed_assertions += writer_args[i].failures;
    }

    /* Clean up shared reader buffers */
    for (int i = 0; i < NUM_SHARED_BUFFERS; i++) {
        mg_free(shared_rw_ptrs[i]);
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count,
                "Tracked count perfectly restored after reader-writer concurrency test");
}

/* --- Test 14: Concurrent allocator interceptor APIs --- */
#define NUM_INTERCEPTOR_THREADS 8
#define INTERCEPTOR_ITERS 25

static void *thread_worker_interceptors(void *arg_ptr) {
    ThreadTestArg *arg = (ThreadTestArg *)arg_ptr;

    for (int iter = 0; iter < INTERCEPTOR_ITERS; iter++) {
        /* 1. mg_intercept_malloc */
        size_t m_sz = 48 + ((size_t)iter * 7) % 64;
        int *m_ptr = (int *)mg_intercept_malloc(m_sz);
        THREAD_ASSERT(arg, m_ptr != NULL, "mg_intercept_malloc returned non-NULL");
        if (m_ptr) {
            THREAD_ASSERT(arg, mg_is_tracked(m_ptr) == 1, "Intercepted malloc is tracked");
            THREAD_ASSERT(arg, mg_is_valid_access(m_ptr, m_sz) == 1, "Access is valid");
            m_ptr[0] = 777;
            mg_intercept_free(m_ptr);
            THREAD_ASSERT(arg, mg_is_tracked(m_ptr) == 0, "Intercepted malloc freed");
        }

        /* 2. mg_intercept_calloc */
        size_t nmemb = 16;
        int *c_ptr = (int *)mg_intercept_calloc(nmemb, sizeof(int));
        THREAD_ASSERT(arg, c_ptr != NULL, "mg_intercept_calloc returned non-NULL");
        if (c_ptr) {
            THREAD_ASSERT(arg, mg_is_tracked(c_ptr) == 1, "Intercepted calloc is tracked");
            THREAD_ASSERT(arg, c_ptr[0] == 0 && c_ptr[nmemb - 1] == 0, "Calloc zero-initialized");
            c_ptr[0] = 888;
            mg_intercept_free(c_ptr);
            THREAD_ASSERT(arg, mg_is_tracked(c_ptr) == 0, "Intercepted calloc freed");
        }

        /* 3. mg_intercept_realloc */
        void *r_ptr = mg_intercept_malloc(32);
        THREAD_ASSERT(arg, r_ptr != NULL, "Initial alloc for realloc non-NULL");
        if (r_ptr) {
            memset(r_ptr, 0xAA, 32);
            void *r_exp = mg_intercept_realloc(r_ptr, 128);
            THREAD_ASSERT(arg, r_exp != NULL, "mg_intercept_realloc expanded buffer");
            if (r_exp) {
                unsigned char *b = (unsigned char *)r_exp;
                THREAD_ASSERT(arg, b[0] == 0xAA && b[31] == 0xAA, "Original bytes preserved in realloc");
                THREAD_ASSERT(arg, mg_is_tracked(r_exp) == 1, "Reallocated buffer tracked");
                THREAD_ASSERT(arg, mg_get_allocation_size(r_exp) == 128, "Reallocated size is 128");
                mg_intercept_free(r_exp);
            } else {
                mg_intercept_free(r_ptr);
            }
        }
    }

    return NULL;
}

static void test_multithreaded_interceptors(void) {
    size_t initial_count = mg_get_tracked_count();
    pthread_t threads[NUM_INTERCEPTOR_THREADS];
    ThreadTestArg thread_args[NUM_INTERCEPTOR_THREADS];

    for (int i = 0; i < NUM_INTERCEPTOR_THREADS; i++) {
        thread_args[i].thread_id = i;
        thread_args[i].assertions = 0;
        thread_args[i].failures = 0;
        pthread_create(&threads[i], NULL, thread_worker_interceptors, &thread_args[i]);
    }

    for (int i = 0; i < NUM_INTERCEPTOR_THREADS; i++) {
        pthread_join(threads[i], NULL);
        total_assertions += thread_args[i].assertions;
        failed_assertions += thread_args[i].failures;
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count,
                "Registry count perfectly restored after multithreaded interceptor test");
}

/* --- Test 15: AVL tree invariant verification and randomized churn --- */
#define CHURN_ALLOC_COUNT 1000

static void test_avl_tree_invariants_and_random_churn(void) {
    size_t initial_count = mg_get_tracked_count();
    void *ptrs[CHURN_ALLOC_COUNT];
    size_t sizes[CHURN_ALLOC_COUNT];

    /* Initial tree invariant on whatever is currently tracked */
    TEST_ASSERT(mg_verify_tree_invariants() == 1, "Initial AVL tree invariants hold");

    /* 1. Allocate CHURN_ALLOC_COUNT blocks with pseudo-random sizes */
    uint32_t seed = 123456789;
    for (int i = 0; i < CHURN_ALLOC_COUNT; i++) {
        seed = seed * 1664525u + 1013904223u;
        sizes[i] = 16 + (seed % 1024);
        ptrs[i] = mg_malloc(sizes[i]);
        TEST_ASSERT(ptrs[i] != NULL, "Churn malloc succeeded");
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count + CHURN_ALLOC_COUNT, "All churn blocks tracked");
    TEST_ASSERT(mg_verify_tree_invariants() == 1, "AVL tree invariants hold after batch insertions");

    /* 2. Validate interior pointers for all blocks */
    for (int i = 0; i < CHURN_ALLOC_COUNT; i++) {
        seed = seed * 1664525u + 1013904223u;
        size_t offset = (seed % sizes[i]);
        size_t acc_len = 1 + (seed % (sizes[i] - offset));
        char *interior = (char *)ptrs[i] + offset;
        TEST_ASSERT(mg_is_valid_access(interior, acc_len) == 1, "Interior pointer valid");

        /* Access extending beyond the allocation boundary must be rejected */
        TEST_ASSERT(mg_is_valid_access(ptrs[i], sizes[i] + 1) == 0, "Access extending beyond boundary rejected");
        TEST_ASSERT(mg_is_valid_access((char *)ptrs[i] + sizes[i] - 1, 2) == 0, "Access crossing end boundary rejected");
    }

    /* 3. Reallocate every second block (expand or shrink) */
    for (int i = 0; i < CHURN_ALLOC_COUNT; i += 2) {
        seed = seed * 1664525u + 1013904223u;
        size_t new_sz = 32 + (seed % 2048);
        void *new_p = mg_realloc_external(ptrs[i], new_sz);
        TEST_ASSERT(new_p != NULL, "Reallocation succeeded");
        ptrs[i] = new_p;
        sizes[i] = new_sz;
    }

    TEST_ASSERT(mg_verify_tree_invariants() == 1, "AVL tree invariants hold after reallocations");

    /* 4. Free half of the allocations in pseudo-random order */
    int order[CHURN_ALLOC_COUNT];
    for (int i = 0; i < CHURN_ALLOC_COUNT; i++) {
        order[i] = i;
    }
    for (int i = CHURN_ALLOC_COUNT - 1; i > 0; i--) {
        seed = seed * 1664525u + 1013904223u;
        int j = (int)(seed % (unsigned int)(i + 1));
        int temp = order[i];
        order[i] = order[j];
        order[j] = temp;
    }

    for (int i = 0; i < CHURN_ALLOC_COUNT / 2; i++) {
        int idx = order[i];
        mg_free(ptrs[idx]);
        ptrs[idx] = NULL;
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count + (CHURN_ALLOC_COUNT / 2), "Half blocks freed");
    TEST_ASSERT(mg_verify_tree_invariants() == 1, "AVL tree invariants hold after randomized partial deletion");

    /* 5. Free remaining blocks in order */
    for (int i = CHURN_ALLOC_COUNT / 2; i < CHURN_ALLOC_COUNT; i++) {
        int idx = order[i];
        mg_free(ptrs[idx]);
        ptrs[idx] = NULL;
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count, "All churn blocks freed");
    TEST_ASSERT(mg_verify_tree_invariants() == 1, "AVL tree invariants hold when fully emptied");
}

/* --- Test 16: Large-scale allocations and random deallocation order --- */
#define LARGE_SCALE_COUNT 5000

static void test_large_scale_allocations(void) {
    size_t initial_count = mg_get_tracked_count();
    void **ptrs = (void **)malloc(LARGE_SCALE_COUNT * sizeof(void *));
    TEST_ASSERT(ptrs != NULL, "Host memory allocation for pointers succeeded");
    if (!ptrs) return;

    /* 1. Allocate LARGE_SCALE_COUNT blocks */
    for (int i = 0; i < LARGE_SCALE_COUNT; i++) {
        size_t sz = 16 + (size_t)(i % 256);
        ptrs[i] = mg_malloc(sz);
        TEST_ASSERT(ptrs[i] != NULL, "Large scale mg_malloc succeeded");
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count + LARGE_SCALE_COUNT, "Tracked count matches 5,000");
    TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold for 5,000 nodes");

    /* 2. Spot check 500 interior and 500 invalid lookups */
    for (int i = 0; i < 500; i++) {
        int idx = (i * 97) % LARGE_SCALE_COUNT;
        size_t sz = 16 + (size_t)(idx % 256);
        char *mid = (char *)ptrs[idx] + (sz / 2);
        TEST_ASSERT(mg_is_valid_access(mid, 1) == 1, "Spot-check interior access valid");

        /* Stack pointer is never tracked in heap */
        int stack_probe = 0x55AA;
        TEST_ASSERT(mg_is_valid_access(&stack_probe, sizeof(int)) == 0, "Stack variable access invalid");
    }

    /* 3. Shuffle and deallocate all 5,000 blocks */
    uint32_t seed = 987654321;
    for (int i = LARGE_SCALE_COUNT - 1; i > 0; i--) {
        seed = seed * 1664525u + 1013904223u;
        int j = (int)(seed % (unsigned int)(i + 1));
        void *tmp = ptrs[i];
        ptrs[i] = ptrs[j];
        ptrs[j] = tmp;
    }

    for (int i = 0; i < LARGE_SCALE_COUNT; i++) {
        mg_free(ptrs[i]);
        if (i % 1000 == 999) {
            TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold during progressive deletion");
        }
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored after large scale test");
    TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after all large-scale nodes freed");

    free(ptrs);
}

/* --- Test 17: Overlapping external allocation ranges --- */
static void test_overlapping_external_ranges(void) {
    size_t initial_count = mg_get_tracked_count();
    char *big_buffer = (char *)malloc(2048);
    TEST_ASSERT(big_buffer != NULL, "big_buffer allocated");
    if (!big_buffer) return;

    /*
     * Register overlapping ranges:
     * Range A: [0, 800)
     * Range B: [300, 1200) -> overlaps Range A on [300, 800)
     * Range C: [1000, 1600) -> overlaps Range B on [1000, 1200)
     * Range D: [1500, 2000) -> overlaps Range C on [1500, 1600)
     */
    void *ptr_a = big_buffer;
    void *ptr_b = big_buffer + 300;
    void *ptr_c = big_buffer + 1000;
    void *ptr_d = big_buffer + 1500;

    TEST_ASSERT(mg_register_external(ptr_a, 800) == 1, "Registered Range A [0, 800)");
    TEST_ASSERT(mg_register_external(ptr_b, 900) == 1, "Registered Range B [300, 1200)");
    TEST_ASSERT(mg_register_external(ptr_c, 600) == 1, "Registered Range C [1000, 1600)");
    TEST_ASSERT(mg_register_external(ptr_d, 500) == 1, "Registered Range D [1500, 2000)");

    TEST_ASSERT(mg_get_tracked_count() == initial_count + 4, "4 overlapping ranges registered");
    TEST_ASSERT(mg_verify_tree_invariants() == 1, "AVL interval tree invariants hold with overlapping ranges");

    /* 1. Interior validations */
    TEST_ASSERT(mg_is_valid_access(big_buffer + 100, 50) == 1, "Access inside Range A valid");
    TEST_ASSERT(mg_is_valid_access(big_buffer + 400, 100) == 1, "Access inside Range A and B valid");
    TEST_ASSERT(mg_is_valid_access(big_buffer + 900, 50) == 1, "Access inside Range B valid");
    TEST_ASSERT(mg_is_valid_access(big_buffer + 1100, 50) == 1, "Access inside Range B and C valid");
    TEST_ASSERT(mg_is_valid_access(big_buffer + 1550, 40) == 1, "Access inside Range C and D valid");
    TEST_ASSERT(mg_is_valid_access(big_buffer + 1800, 50) == 1, "Access inside Range D valid");

    /* 2. Access outside all ranges */
    TEST_ASSERT(mg_is_valid_access(big_buffer + 2000, 10) == 0, "Access beyond Range D rejected");

    /* 3. Cross-boundary access spanning beyond any single allocation must be rejected */
    /* [100, 950) spans 850 bytes. Range A only extends to 800. Range B starts at 300. */
    TEST_ASSERT(mg_is_valid_access(big_buffer + 100, 850) == 0, "Access spanning across ranges rejected");

    /* 4. Unregister Range B: [300, 1200) */
    TEST_ASSERT(mg_unregister_external(ptr_b) == 1, "Range B unregistered");
    TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after unregistering Range B");

    /* [400, 500) is still valid because Range A [0, 800) covers it */
    TEST_ASSERT(mg_is_valid_access(big_buffer + 400, 100) == 1, "Access still covered by Range A is valid");

    /* [900, 950) was ONLY covered by Range B, so it must now be INVALID */
    TEST_ASSERT(mg_is_valid_access(big_buffer + 900, 50) == 0, "Access previously in Range B is now invalid");

    /* 5. Clean up remaining ranges */
    TEST_ASSERT(mg_unregister_external(ptr_a) == 1, "Range A unregistered");
    TEST_ASSERT(mg_unregister_external(ptr_c) == 1, "Range C unregistered");
    TEST_ASSERT(mg_unregister_external(ptr_d) == 1, "Range D unregistered");

    TEST_ASSERT(mg_get_tracked_count() == initial_count, "All external ranges cleaned up");
    TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after all ranges unregistered");

    free(big_buffer);
}

/* --- Test 18: Reallocation address relocation stress --- */
#define RELOC_COUNT 200

static void test_realloc_address_relocation_stress(void) {
    size_t initial_count = mg_get_tracked_count();
    void *ptrs[RELOC_COUNT];
    size_t original_sizes[RELOC_COUNT];

    /* 1. Allocate initial small blocks and fill with distinctive patterns */
    for (int i = 0; i < RELOC_COUNT; i++) {
        original_sizes[i] = 32 + (size_t)(i % 64);
        ptrs[i] = mg_malloc(original_sizes[i]);
        TEST_ASSERT(ptrs[i] != NULL, "Initial alloc succeeded");
        memset(ptrs[i], (unsigned char)(i & 0xFF), original_sizes[i]);
    }

    TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold before realloc relocations");

    /* 2. Reallocate to significantly larger sizes to force memory relocation */
    int relocations_observed = 0;
    for (int i = 0; i < RELOC_COUNT; i++) {
        uintptr_t old_addr = (uintptr_t)ptrs[i];
        size_t new_sz = 8192 + (size_t)(i * 32);
        void *new_p = mg_realloc_external(ptrs[i], new_sz);
        TEST_ASSERT(new_p != NULL, "Realloc relocation succeeded");

        if ((uintptr_t)new_p != old_addr) {
            relocations_observed++;
            /* The old address must no longer be tracked */
            TEST_ASSERT(mg_is_tracked((const void *)old_addr) == 0, "Old address no longer tracked after relocation");
        }

        /* Verify data preservation */
        unsigned char *bytes = (unsigned char *)new_p;
        int preserved = 1;
        for (size_t b = 0; b < original_sizes[i]; b++) {
            if (bytes[b] != (unsigned char)(i & 0xFF)) {
                preserved = 0;
                break;
            }
        }
        TEST_ASSERT(preserved == 1, "Payload data intact after relocation");

        /* Verify new bounds and interior access */
        TEST_ASSERT(mg_is_tracked(new_p) == 1, "New pointer is tracked");
        TEST_ASSERT(mg_get_allocation_size(new_p) == new_sz, "New size recorded");
        TEST_ASSERT(mg_is_valid_access((char *)new_p + new_sz - 1, 1) == 1, "Access at new boundary valid");
        TEST_ASSERT(mg_is_valid_access((char *)new_p + new_sz, 1) == 0, "Access past new boundary rejected");

        ptrs[i] = new_p;
    }

    TEST_ASSERT(relocations_observed > 0, "Observed at least one actual pointer relocation");
    TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after all relocations");

    /* 3. Reallocate back down to small sizes */
    for (int i = 0; i < RELOC_COUNT; i++) {
        size_t shrink_sz = 16 + (size_t)(i % 16);
        void *shrunk = mg_realloc_external(ptrs[i], shrink_sz);
        TEST_ASSERT(shrunk != NULL, "Realloc shrink succeeded");
        TEST_ASSERT(mg_get_allocation_size(shrunk) == shrink_sz, "Shrunk size recorded");
        ptrs[i] = shrunk;
    }

    TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after shrink reallocations");

    /* 4. Clean up all blocks */
    for (int i = 0; i < RELOC_COUNT; i++) {
        mg_free(ptrs[i]);
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored after relocation test");
    TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold when emptied");
}

/* --- Test 19: Multithreaded concurrent tree stress --- */
#define STRESS_THREADS 16
#define STRESS_ITERS 50

static void *thread_worker_tree_stress(void *arg_ptr) {
    ThreadTestArg *arg = (ThreadTestArg *)arg_ptr;
    uint32_t seed = 54321 + (uint32_t)arg->thread_id * 7919;

    for (int iter = 0; iter < STRESS_ITERS; iter++) {
        seed = seed * 1664525u + 1013904223u;
        size_t sz = 24 + (seed % 128);

        /* 1. Malloc and verify */
        void *p = mg_malloc(sz);
        THREAD_ASSERT(arg, p != NULL, "Stress malloc returned non-NULL");
        if (p) {
            THREAD_ASSERT(arg, mg_is_tracked(p) == 1, "Allocated buffer is tracked");
            THREAD_ASSERT(arg, mg_is_valid_access(p, sz) == 1, "Full range access valid");
            THREAD_ASSERT(arg, mg_is_valid_access((char *)p + (sz / 2), 1) == 1, "Midpoint access valid");

            /* 2. Realloc */
            size_t new_sz = sz * 2;
            void *rp = mg_realloc_external(p, new_sz);
            THREAD_ASSERT(arg, rp != NULL, "Stress realloc returned non-NULL");
            if (rp) {
                THREAD_ASSERT(arg, mg_is_tracked(rp) == 1, "Reallocated buffer tracked");
                THREAD_ASSERT(arg, mg_is_valid_access(rp, new_sz) == 1, "New full range valid");
                mg_free(rp);
                THREAD_ASSERT(arg, mg_is_tracked(rp) == 0, "Buffer untracked after free");
            } else {
                mg_free(p);
            }
        }

        /* 3. Register external buffer */
        char ext_buf[64];
        if (mg_register_external(ext_buf, sizeof(ext_buf))) {
            THREAD_ASSERT(arg, mg_is_tracked(ext_buf) == 1, "External buffer tracked");
            THREAD_ASSERT(arg, mg_is_valid_access(ext_buf + 10, 20) == 1, "External interior valid");
            mg_unregister_external(ext_buf);
            THREAD_ASSERT(arg, mg_is_tracked(ext_buf) == 0, "External buffer unregistered");
        }

        /* 4. Invariant check */
        if (iter % 10 == 0) {
            THREAD_ASSERT(arg, mg_verify_tree_invariants() == 1, "Tree invariants hold concurrently");
        }
    }

    return NULL;
}

static void test_multithreaded_concurrent_tree_stress(void) {
    size_t initial_count = mg_get_tracked_count();
    pthread_t threads[STRESS_THREADS];
    ThreadTestArg thread_args[STRESS_THREADS];

    for (int i = 0; i < STRESS_THREADS; i++) {
        thread_args[i].thread_id = i;
        thread_args[i].assertions = 0;
        thread_args[i].failures = 0;
        pthread_create(&threads[i], NULL, thread_worker_tree_stress, &thread_args[i]);
    }

    for (int i = 0; i < STRESS_THREADS; i++) {
        pthread_join(threads[i], NULL);
        total_assertions += thread_args[i].assertions;
        failed_assertions += thread_args[i].failures;
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count,
                "Registry count restored after multithreaded tree stress");
    TEST_ASSERT(mg_verify_tree_invariants() == 1,
                "Tree invariants hold after multithreaded stress test completes");
}

/* --- Step 25: Deterministic Realloc Pointer Invalidation & Edge Cases --- */

static void *mock_inplace_realloc(void *ptr, size_t size) {
    (void)size;
    return ptr; /* Guarantees in-place reallocation (same address) */
}

static void *mock_relocating_realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    void *new_buf = malloc(size);
    if (!new_buf) return NULL;
    memcpy(new_buf, ptr, size < 32 ? size : 32);
    free(ptr);
    return new_buf; /* Guarantees memory relocation to a distinct address */
}

static void *mock_failing_realloc(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
    return NULL; /* Guarantees allocation failure without modifying original */
}

static char mock_zero_target_anchor;
static void *mock_zero_nonnull_realloc(void *ptr, size_t size) {
    if (size == 0) {
        free(ptr);
        return &mock_zero_target_anchor;
    }
    return realloc(ptr, size);
}

static void *mock_zero_null_realloc(void *ptr, size_t size) {
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

static void test_realloc_pointer_invalidation_and_edge_cases(void) {
    size_t initial_count = mg_get_tracked_count();

    /* -------------------------------------------------------------------------
     * 1. Successful realloc that remains at the same address (in-place)
     * ------------------------------------------------------------------------- */
    {
        void *p = malloc(64);
        TEST_ASSERT(p != NULL, "Allocated buffer for in-place realloc test");
        TEST_ASSERT(mg_register_external(p, 64) == 1, "Registered buffer");

        uintptr_t orig_addr = (uintptr_t)p;
        mg_test_set_realloc_fn(mock_inplace_realloc);

        void *new_p = mg_realloc_external(p, 128);
        mg_test_reset_realloc_fn();

        TEST_ASSERT(new_p != NULL, "In-place realloc returned non-NULL");
        TEST_ASSERT((uintptr_t)new_p == orig_addr, "Address preserved for in-place realloc");
        TEST_ASSERT(mg_is_tracked(new_p) == 1, "In-place realloc pointer is tracked");
        TEST_ASSERT(mg_get_allocation_size(new_p) == 128, "Size updated to 128");
        TEST_ASSERT(mg_is_valid_access(new_p, 128) == 1, "Full 128-byte range valid");
        TEST_ASSERT(mg_is_valid_access((char *)new_p + 128, 1) == 0, "Access past new size rejected");
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after in-place realloc");

        TEST_ASSERT(mg_unregister_external(new_p) == 1, "Unregistered in-place realloc");
        free(new_p);
        TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored");
    }

    /* -------------------------------------------------------------------------
     * 2. Successful realloc that relocates to a different address
     *    and verifies old allocation address is no longer registered
     * ------------------------------------------------------------------------- */
    {
        void *p = malloc(48);
        TEST_ASSERT(p != NULL, "Allocated buffer for relocating realloc test");
        memset(p, 0x5A, 48);
        TEST_ASSERT(mg_register_external(p, 48) == 1, "Registered buffer");

        /* Preserve old address as an integer BEFORE calling realloc */
        uintptr_t old_addr = (uintptr_t)p;
        mg_test_set_realloc_fn(mock_relocating_realloc);

        void *new_p = mg_realloc_external(p, 256);
        mg_test_reset_realloc_fn();

        TEST_ASSERT(new_p != NULL, "Relocating realloc returned non-NULL");
        TEST_ASSERT((uintptr_t)new_p != old_addr, "Memory was genuinely relocated to new address");

        /* Old allocation address MUST NOT be tracked or accessible */
        TEST_ASSERT(mg_is_tracked((const void *)old_addr) == 0, "Old address is no longer tracked");
        TEST_ASSERT(mg_is_valid_access((const void *)old_addr, 1) == 0, "Access at old address rejected");

        /* New allocation address MUST be tracked with updated size */
        TEST_ASSERT(mg_is_tracked(new_p) == 1, "New address is tracked in registry");
        TEST_ASSERT(mg_get_allocation_size(new_p) == 256, "Size updated to 256 bytes");
        TEST_ASSERT(mg_is_valid_access(new_p, 256) == 1, "Full 256-byte range valid");

        /* Interior pointer validation */
        TEST_ASSERT(mg_is_valid_access((char *)new_p + 100, 1) == 1, "Interior access at offset 100 valid");
        TEST_ASSERT(mg_is_valid_access((char *)new_p + 255, 1) == 1, "Interior access at last byte valid");
        TEST_ASSERT(mg_is_valid_access((char *)new_p + 256, 1) == 0, "Access past boundary rejected");

        /* Payload data preserved */
        TEST_ASSERT(((unsigned char *)new_p)[0] == 0x5A && ((unsigned char *)new_p)[31] == 0x5A,
                    "Payload intact in relocated block");
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after relocating realloc");

        TEST_ASSERT(mg_unregister_external(new_p) == 1, "Unregistered relocated buffer");
        free(new_p);
        TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored");
    }

    /* -------------------------------------------------------------------------
     * 3. Failed realloc preserving original allocation and registry entry
     * ------------------------------------------------------------------------- */
    {
        void *p = malloc(80);
        TEST_ASSERT(p != NULL, "Allocated buffer for failed realloc test");
        memset(p, 0xC3, 80);
        TEST_ASSERT(mg_register_external(p, 80) == 1, "Registered buffer");

        mg_test_set_realloc_fn(mock_failing_realloc);
        void *fail_res = mg_realloc_external(p, 512);
        mg_test_reset_realloc_fn();

        TEST_ASSERT(fail_res == NULL, "Realloc failed as expected");
        TEST_ASSERT(mg_is_tracked(p) == 1, "Original pointer is STILL tracked after failure");
        TEST_ASSERT(mg_get_allocation_size(p) == 80, "Original size preserved after failure");
        TEST_ASSERT(mg_is_valid_access(p, 80) == 1, "Original bounds valid after failure");
        TEST_ASSERT(mg_is_valid_access((char *)p + 80, 1) == 0, "Access past original bounds rejected");
        TEST_ASSERT(((unsigned char *)p)[0] == 0xC3, "Original buffer content intact");
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after failed realloc");

        TEST_ASSERT(mg_unregister_external(p) == 1, "Cleaned up failed test buffer");
        free(p);
        TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored");
    }

    /* -------------------------------------------------------------------------
     * 4. External vs Native allocation ownership behavior (owns_payload)
     * ------------------------------------------------------------------------- */
    {
        /* External: owns_payload == 0 */
        void *ext = malloc(50);
        TEST_ASSERT(ext != NULL, "Allocated external block");
        TEST_ASSERT(mg_register_external(ext, 50) == 1, "Registered external block");

        mg_test_set_realloc_fn(mock_relocating_realloc);
        void *realloc_ext = mg_realloc_external(ext, 100);
        mg_test_reset_realloc_fn();

        TEST_ASSERT(realloc_ext != NULL, "External realloc succeeded");
        /* owns_payload must remain 0: mg_unregister_external detaches without freeing payload */
        TEST_ASSERT(mg_unregister_external(realloc_ext) == 1, "Unregistered external block");
        free(realloc_ext);
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold");

        /* Native: owns_payload == 1 */
        void *nat = mg_malloc(50);
        TEST_ASSERT(nat != NULL, "Allocated native block");

        mg_test_set_realloc_fn(mock_relocating_realloc);
        void *realloc_nat = mg_realloc_external(nat, 100);
        mg_test_reset_realloc_fn();

        TEST_ASSERT(realloc_nat != NULL, "Native realloc succeeded");
        /* owns_payload must remain 1: mg_free frees payload and registry entry */
        mg_free(realloc_nat);
        TEST_ASSERT(mg_is_tracked(realloc_nat) == 0, "Native block untracked after mg_free");
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after mg_free");
    }

    /* -------------------------------------------------------------------------
     * 5. realloc(NULL, size) handling
     * ------------------------------------------------------------------------- */
    {
        /* mg_realloc_external(NULL, size) -> untracked allocation (standard realloc) */
        void *p1 = mg_realloc_external(NULL, 64);
        TEST_ASSERT(p1 != NULL, "mg_realloc_external(NULL, 64) returned non-NULL");
        TEST_ASSERT(mg_is_tracked(p1) == 0, "mg_realloc_external(NULL, size) produces untracked pointer");
        free(p1);

        /* mg_intercept_realloc(NULL, size) -> tracked allocation (like mg_intercept_malloc) */
        void *p2 = mg_intercept_realloc(NULL, 64);
        TEST_ASSERT(p2 != NULL, "mg_intercept_realloc(NULL, 64) returned non-NULL");
        TEST_ASSERT(mg_is_tracked(p2) == 1, "mg_intercept_realloc(NULL, size) produces tracked pointer");
        TEST_ASSERT(mg_get_allocation_size(p2) == 64, "Size is 64");
        mg_intercept_free(p2);
        TEST_ASSERT(mg_is_tracked(p2) == 0, "Untracked after mg_intercept_free");
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold");
    }

    /* -------------------------------------------------------------------------
     * 6. Zero-size realloc platform contracts
     * ------------------------------------------------------------------------- */
    {
        /* Contract A: platform frees buffer and returns NULL */
        void *p_null = mg_intercept_malloc(64);
        TEST_ASSERT(p_null != NULL, "Allocated buffer for zero-size NULL realloc");
        uintptr_t addr_null = (uintptr_t)p_null;
        size_t count_before_a = mg_get_tracked_count();

        mg_test_set_realloc_fn(mock_zero_null_realloc);
        void *res_null = mg_realloc_external(p_null, 0);
        mg_test_reset_realloc_fn();

        TEST_ASSERT(res_null == NULL, "Mock zero-size realloc returned NULL");
        TEST_ASSERT(mg_is_tracked((const void *)addr_null) == 0, "Old address unlinked after zero-size free");
        TEST_ASSERT(mg_get_tracked_count() == count_before_a - 1, "Tracked count decremented");
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after zero-size NULL");

        /* Contract B: platform returns non-NULL zero-sized pointer */
        void *p_nonnull = mg_intercept_malloc(64);
        TEST_ASSERT(p_nonnull != NULL, "Allocated buffer for zero-size non-NULL realloc");
        uintptr_t addr_nonnull = (uintptr_t)p_nonnull;

        mg_test_set_realloc_fn(mock_zero_nonnull_realloc);
        void *res_nonnull = mg_realloc_external(p_nonnull, 0);
        mg_test_reset_realloc_fn();

        TEST_ASSERT(res_nonnull != NULL, "Mock zero-size realloc returned non-NULL");
        TEST_ASSERT(mg_is_tracked((const void *)addr_nonnull) == 0, "Old address unlinked");
        TEST_ASSERT(mg_is_tracked(res_nonnull) == 1, "New zero-size address tracked");
        TEST_ASSERT(mg_get_allocation_size(res_nonnull) == 0, "Allocation size is 0");
        TEST_ASSERT(mg_is_valid_access(res_nonnull, 1) == 0, "Non-zero access rejected for 0-size buffer");
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after zero-size non-NULL");

        TEST_ASSERT(mg_unregister_external(res_nonnull) == 1, "Unregistered zero-size buffer");
        TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored");
    }

    /* -------------------------------------------------------------------------
     * 7. Untracked pointer pass-through
     * ------------------------------------------------------------------------- */
    {
        void *raw = malloc(32);
        TEST_ASSERT(raw != NULL, "Allocated raw buffer");
        void *realloced = mg_realloc_external(raw, 96);
        TEST_ASSERT(realloced != NULL, "Untracked realloc succeeded");
        TEST_ASSERT(mg_is_tracked(realloced) == 0, "Untracked pointer remains untracked");
        free(realloced);
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold");
    }

    /* -------------------------------------------------------------------------
     * 8. Registry node allocation failure before realloc (nonzero size)
     * ------------------------------------------------------------------------- */
    {
        void *p = malloc(64);
        TEST_ASSERT(p != NULL, "Allocated buffer for node alloc failure test");
        memset(p, 0x77, 64);
        TEST_ASSERT(mg_register_external(p, 64) == 1, "Registered buffer");
        size_t count_before = mg_get_tracked_count();

        /* Simulate registry node allocation failure */
        mg_test_set_node_alloc_fail(1);
        void *res = mg_realloc_external(p, 128);
        mg_test_set_node_alloc_fail(0);

        TEST_ASSERT(res == NULL, "mg_realloc_external returned NULL when node allocation fails");
        TEST_ASSERT(mg_is_tracked(p) == 1, "Original allocation is STILL tracked after node alloc failure");
        TEST_ASSERT(mg_get_allocation_size(p) == 64, "Original size preserved (64 bytes)");
        TEST_ASSERT(mg_is_valid_access(p, 64) == 1, "Original buffer memory access valid");
        TEST_ASSERT(mg_is_valid_access((char *)p + 64, 1) == 0, "Access past original bounds rejected");
        TEST_ASSERT(((unsigned char *)p)[0] == 0x77, "Original buffer content unmodified");
        TEST_ASSERT(mg_get_tracked_count() == count_before, "Tracked count unchanged after node alloc failure");
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after node alloc failure");

        /* Clean up */
        TEST_ASSERT(mg_unregister_external(p) == 1, "Unregistered buffer");
        free(p);
        TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored");
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after cleanup");
    }

    /* -------------------------------------------------------------------------
     * 9. Registry node allocation failure before realloc (zero size)
     * ------------------------------------------------------------------------- */
    {
        void *p0 = malloc(48);
        TEST_ASSERT(p0 != NULL, "Allocated buffer for zero-size node alloc failure test");
        memset(p0, 0x88, 48);
        TEST_ASSERT(mg_register_external(p0, 48) == 1, "Registered buffer");
        size_t count_before = mg_get_tracked_count();

        /* Simulate node allocation failure for size == 0 */
        mg_test_set_node_alloc_fail(1);
        void *res0 = mg_realloc_external(p0, 0);
        mg_test_set_node_alloc_fail(0);

        TEST_ASSERT(res0 == NULL, "Zero-size realloc returned NULL on node alloc failure");
        TEST_ASSERT(mg_is_tracked(p0) == 1, "Original allocation still tracked after zero-size node alloc failure");
        TEST_ASSERT(mg_get_allocation_size(p0) == 48, "Original size preserved");
        TEST_ASSERT(mg_is_valid_access(p0, 48) == 1, "Original bounds valid");
        TEST_ASSERT(((unsigned char *)p0)[0] == 0x88, "Buffer content unmodified");
        TEST_ASSERT(mg_get_tracked_count() == count_before, "Tracked count unchanged");
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold");

        /* Clean up */
        TEST_ASSERT(mg_unregister_external(p0) == 1, "Unregistered buffer");
        free(p0);
        TEST_ASSERT(mg_get_tracked_count() == initial_count, "Tracked count restored");
        TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold after cleanup");
    }

    TEST_ASSERT(mg_get_tracked_count() == initial_count, "Overall tracked count restored");
    TEST_ASSERT(mg_verify_tree_invariants() == 1, "Tree invariants hold at test end");
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
    RUN_TEST(test_boundary_validation);
    RUN_TEST(test_external_allocations);
    RUN_TEST(test_check_access);
    RUN_TEST(test_interceptor_apis);
    RUN_TEST(test_multithreaded_malloc_free);
    RUN_TEST(test_multithreaded_external_and_realloc);
    RUN_TEST(test_multithreaded_concurrent_readers_writers);
    RUN_TEST(test_multithreaded_interceptors);
    RUN_TEST(test_avl_tree_invariants_and_random_churn);
    RUN_TEST(test_large_scale_allocations);
    RUN_TEST(test_overlapping_external_ranges);
    RUN_TEST(test_realloc_address_relocation_stress);
    RUN_TEST(test_realloc_pointer_invalidation_and_edge_cases);
    RUN_TEST(test_multithreaded_concurrent_tree_stress);

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

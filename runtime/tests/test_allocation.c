#include "memoryguard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

    void *zero_res = mg_realloc_external(zero_target_p, 0);
    if (zero_res == NULL) {
        /* Standard platform behavior (e.g. glibc): buffer was freed and NULL returned */
        TEST_ASSERT(mg_get_tracked_count() == initial_count, "Registry entry removed when realloc(ptr, 0) returns NULL");
        TEST_ASSERT(mg_is_tracked(zero_target_p) == 0, "Original pointer no longer tracked");
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

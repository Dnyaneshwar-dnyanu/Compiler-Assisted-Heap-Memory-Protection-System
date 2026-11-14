#ifndef MEMORYGUARD_H
#define MEMORYGUARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file memoryguard.h
 * @brief MemoryGuard C Runtime Interface.
 *
 * Provides thread-safe heap allocation tracking, boundary validation, and
 * allocator interception APIs. All operations on the allocation registry are
 * synchronized and safe for concurrent multi-threaded execution.
 */

/**
 * @brief Allocates heap memory and records it in the allocation registry.
 *
 * Allocates `size` bytes of memory using standard malloc, creates an Allocation
 * metadata entry tracking the base address and size, and registers it in the
 * internal linked list registry.
 *
 * If either the payload allocation or the metadata allocation fails, memory is
 * safely cleaned up and NULL is returned.
 *
 * @param size Number of bytes to allocate.
 * @return void* Pointer to the allocated memory, or NULL on allocation failure.
 */
void *mg_malloc(size_t size);

/**
 * @brief Deallocates memory and removes its record from the allocation registry.
 *
 * Locates the allocation metadata in the registry by the exact base address.
 * If found, unlinks the metadata node, releases the allocated memory using
 * standard free, and deallocates the metadata entry.
 *
 * If `ptr` is NULL or is not found in the registry (e.g. unknown pointer, or
 * already freed pointer), the function safely returns without calling free,
 * preventing double-free errors.
 *
 * @param ptr Pointer to the memory previously allocated by mg_malloc.
 */
void mg_free(void *ptr);

/**
 * @brief Checks whether an exact base address is currently tracked in the registry.
 *
 * @param ptr Pointer to check.
 * @return int 1 if the exact pointer is registered, 0 otherwise (including NULL).
 */
int mg_is_tracked(const void *ptr);

/**
 * @brief Retrieves the recorded size of an active allocation.
 *
 * @param ptr Base pointer of the allocation.
 * @return size_t The allocated size in bytes if registered, or 0 if not tracked or NULL.
 */
size_t mg_get_allocation_size(const void *ptr);

/**
 * @brief Returns the total number of currently active allocations in the registry.
 *
 * Helper function useful for debugging, test verification, and leak detection.
 *
 * @return size_t Number of active allocations registered.
 */
size_t mg_get_tracked_count(void);

/**
 * @brief Validates whether a memory access range lies entirely within a single tracked allocation.
 *
 * Checks if the requested access range [ptr, ptr + access_size) belongs
 * completely to an active heap allocation tracked by MemoryGuard.
 * Supports interior pointers.
 *
 * Safely rejects:
 *  - NULL pointers
 *  - Pointers outside all tracked allocations
 *  - Accesses extending beyond allocation boundaries
 *  - Accesses that overflow address calculations
 *
 * Zero-size access semantics (access_size == 0):
 *  - Returns 1 if and only if ptr is non-NULL and points to a valid allocated byte
 *    within an active allocation (alloc_start <= ptr < alloc_end).
 *  - Returns 0 if ptr is NULL, points at or beyond the boundary (ptr >= alloc_end),
 *    points before alloc_start, or refers to a zero-sized allocation.
 *
 * @param ptr Starting address of the memory access.
 * @param access_size Size of the memory access in bytes.
 * @return int 1 if the access is valid, 0 otherwise.
 */
int mg_is_valid_access(const void *ptr, size_t access_size);

/**
 * @brief Validates that a heap memory access range is valid, aborting on failure.
 *
 * Checks whether the memory range [ptr, ptr + access_size) lies entirely within
 * an active allocation tracked by MemoryGuard by calling mg_is_valid_access().
 *
 * If the access is valid, this function returns normally.
 * If the access is invalid (e.g. out-of-bounds access, access after deallocation,
 * NULL pointer, untracked address, or arithmetic overflow), this function prints
 * diagnostic details to stderr with the offending pointer and access size, then terminates
 * execution by calling abort().
 *
 * @param ptr Starting address of the memory access.
 * @param access_size Size of the memory access in bytes.
 */
void mg_check_access(const void *ptr, size_t access_size);

/**
 * @brief Registers an external heap allocation with the MemoryGuard registry.
 *
 * Tracks an existing buffer allocated through standard C allocation functions
 * (malloc, calloc, etc.) without transferring ownership of its memory to MemoryGuard.
 * MemoryGuard will track this range for boundary checks and size queries, but will
 * not free the payload memory upon unregistration.
 *
 * Rejects:
 *  - NULL pointers (returns 0)
 *  - Duplicate registrations of an already-tracked base address (returns 0)
 *
 * Supports zero-sized allocations (size == 0) consistently with mg_malloc(0).
 *
 * @param ptr Non-NULL base address of the external allocation.
 * @param size Allocated size in bytes.
 * @return int 1 on success, 0 on failure (e.g. ptr is NULL, duplicate base address,
 *             or metadata allocation failure).
 */
int mg_register_external(void *ptr, size_t size);

/**
 * @brief Unregisters an external heap allocation from the MemoryGuard registry.
 *
 * Removes the metadata node from the registry without freeing the underlying payload.
 * The application retains ownership of the payload and is responsible for freeing it.
 *
 * @param ptr Base address of the external allocation to unregister.
 * @return int 1 on success, 0 if ptr is NULL or not found in the registry.
 */
int mg_unregister_external(void *ptr);

/**
 * @brief Reallocates an external allocation and updates the registry.
 *
 * Invokes standard C realloc internally.
 *
 * Behavior:
 *  - If ptr is registered in MemoryGuard:
 *      * On successful nonzero-size reallocation (new_size > 0), updates the existing
 *        registry entry in-place with the new address and size. The old pointer is never
 *        dereferenced or used again.
 *      * On failure, preserves the original registry entry untouched because realloc
 *        leaves the original allocation intact.
 *      * If new_size == 0, follows platform-specific realloc(ptr, 0) behavior:
 *        if non-null is returned, registry size updates to 0;
 *        if NULL is returned (platform freed the buffer), the registry entry is removed.
 *  - If ptr is not registered, or if ptr is NULL, performs standard realloc without
 *    altering or registering the allocation in the registry.
 *
 * @param ptr Pointer previously allocated (may be NULL or unregistered).
 * @param new_size Requested new size in bytes.
 * @return void* Pointer to reallocated memory, or NULL on failure / deallocation.
 */
void *mg_realloc_external(void *ptr, size_t new_size);

/**
 * @brief Allocator interceptor: allocates memory and automatically registers it.
 *
 * Allocates `size` bytes using standard libc malloc. Upon successful allocation,
 * registers the resulting memory buffer in the MemoryGuard registry via
 * mg_register_external().
 *
 * If either the payload allocation or the metadata registration fails, any allocated
 * memory is safely freed and NULL is returned.
 *
 * @param size Number of bytes to allocate.
 * @return void* Pointer to the allocated and tracked buffer, or NULL on failure.
 */
void *mg_intercept_malloc(size_t size);

/**
 * @brief Allocator interceptor: allocates zero-initialized array and registers it.
 *
 * Performs safe multiplication overflow checking on `nmemb * size`. If multiplication
 * would overflow size_t, returns NULL immediately without allocating.
 *
 * Allocates memory using standard libc calloc and registers the total allocated
 * byte count in the MemoryGuard registry via mg_register_external().
 *
 * If either allocation or registration fails, memory is freed and NULL is returned.
 *
 * @param nmemb Number of elements to allocate.
 * @param size Size in bytes of each element.
 * @return void* Pointer to the allocated, zero-initialized, and tracked buffer, or NULL on failure.
 */
void *mg_intercept_calloc(size_t nmemb, size_t size);

/**
 * @brief Allocator interceptor: reallocates memory and updates registry tracking.
 *
 * Intercepts standard realloc calls:
 *  - If ptr is NULL, behaves like mg_intercept_malloc(size), allocating and registering.
 *  - If ptr is non-NULL, delegates to mg_realloc_external(ptr, size), which handles:
 *      * Successful in-place expansion/relocation (updates registry address and size).
 *      * Reallocation failure (preserves original registry entry untouched).
 *      * Zero-size deallocation (unregisters and frees entry if NULL is returned).
 *      * Untracked pointers (passes through to standard realloc safely).
 *
 * @param ptr Existing pointer to reallocate, or NULL.
 * @param size Requested new size in bytes.
 * @return void* Pointer to reallocated and tracked memory, or NULL on failure / deallocation.
 */
void *mg_intercept_realloc(void *ptr, size_t size);

/**
 * @brief Allocator interceptor: unregisters tracked metadata and frees memory.
 *
 * Unregisters `ptr` from the MemoryGuard registry (if tracked) and always calls
 * standard libc free(ptr) for non-NULL pointers.
 *
 * Safe for NULL pointers (no-op) and untracked pointers (calls libc free without
 * registry errors, preventing memory leaks for uninstrumented external allocations).
 *
 * @param ptr Pointer to the memory buffer to free.
 */
void mg_intercept_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* MEMORYGUARD_H */

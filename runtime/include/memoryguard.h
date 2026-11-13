#ifndef MEMORYGUARD_H
#define MEMORYGUARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Metadata structure for an active heap memory allocation.
 *
 * Each active allocation managed by MemoryGuard is represented by an
 * instance of this structure in the allocation registry's linked list.
 */
typedef struct Allocation {
    void *address;              /**< Starting base address of the allocated memory */
    size_t size;                /**< Size of the allocated memory in bytes */
    struct Allocation *next;    /**< Pointer to the next metadata entry in the registry */
} Allocation;

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

#ifdef __cplusplus
}
#endif

#endif /* MEMORYGUARD_H */

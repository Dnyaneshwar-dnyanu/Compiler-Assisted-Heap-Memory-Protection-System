#include "memoryguard.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/**
 * @brief Private metadata structure for an active heap memory allocation.
 *
 * Internal to the runtime implementation. Each active allocation managed by
 * MemoryGuard is represented by an instance of this structure in the allocation
 * registry's linked list.
 */
typedef struct Allocation {
    void *address;              /**< Starting base address of the allocated memory */
    size_t size;                /**< Size of the allocated memory in bytes */
    int owns_payload;           /**< 1 if allocated by mg_malloc, 0 if registered externally */
    struct Allocation *next;    /**< Pointer to the next metadata entry in the registry */
} Allocation;

/**
 * @brief Head of the singly linked list storing active allocation metadata.
 *
 * Internal module state representing the MemoryGuard allocation registry.
 * Each node tracks an active allocation's base address and size.
 */
static Allocation *allocation_registry = NULL;

void *mg_malloc(size_t size) {
    /* Step 1: Allocate requested memory payload using standard malloc */
    void *payload = malloc(size);
    if (payload == NULL) {
        return NULL;
    }

    /* Step 2: Allocate metadata node for registry tracking */
    Allocation *node = (Allocation *)malloc(sizeof(Allocation));
    if (node == NULL) {
        /* Metadata allocation failed: release payload to prevent memory leak */
        free(payload);
        return NULL;
    }

    /* Step 3: Populate metadata node fields */
    node->address = payload;
    node->size = size;
    node->owns_payload = 1;

    /* Step 4: Register node by prepending it to the linked list */
    node->next = allocation_registry;
    allocation_registry = node;

    return payload;
}

void mg_free(void *ptr) {
    /* Handle NULL pointer safely */
    if (ptr == NULL) {
        return;
    }

    Allocation *curr = allocation_registry;
    Allocation *prev = NULL;

    /* Search for the allocation entry matching the exact base pointer */
    while (curr != NULL) {
        if (curr->address == ptr) {
            /* Unlink the metadata node from the registry */
            if (prev == NULL) {
                allocation_registry = curr->next;
            } else {
                prev->next = curr->next;
            }

            /* Release the underlying allocated memory if owned by MemoryGuard */
            if (curr->owns_payload) {
                free(curr->address);
            }

            /* Release the metadata node itself */
            free(curr);

            return;
        }
        prev = curr;
        curr = curr->next;
    }

    /* If ptr is not found in the registry (unknown pointer or already freed),
     * safely do nothing. This prevents double-freeing and invalid free operations. */
}

int mg_is_tracked(const void *ptr) {
    if (ptr == NULL) {
        return 0;
    }

    const Allocation *curr = allocation_registry;
    while (curr != NULL) {
        if (curr->address == ptr) {
            return 1;
        }
        curr = curr->next;
    }

    return 0;
}

size_t mg_get_allocation_size(const void *ptr) {
    if (ptr == NULL) {
        return 0;
    }

    const Allocation *curr = allocation_registry;
    while (curr != NULL) {
        if (curr->address == ptr) {
            return curr->size;
        }
        curr = curr->next;
    }

    return 0;
}

size_t mg_get_tracked_count(void) {
    size_t count = 0;
    const Allocation *curr = allocation_registry;
    while (curr != NULL) {
        count++;
        curr = curr->next;
    }
    return count;
}

int mg_is_valid_access(const void *ptr, size_t access_size) {
    /* Reject NULL pointers */
    if (ptr == NULL) {
        return 0;
    }

    uintptr_t access_start = (uintptr_t)ptr;

    /* Reject accesses where address arithmetic overflows */
    if (UINTPTR_MAX - access_start < access_size) {
        return 0;
    }

    uintptr_t access_end = access_start + access_size;

    const Allocation *curr = allocation_registry;
    while (curr != NULL) {
        uintptr_t alloc_start = (uintptr_t)curr->address;
        size_t alloc_size = curr->size;

        /* Defensive check: ensure allocation range arithmetic does not overflow */
        if (UINTPTR_MAX - alloc_start < alloc_size) {
            curr = curr->next;
            continue;
        }

        uintptr_t alloc_end = alloc_start + alloc_size;

        if (access_size > 0) {
            /* Access range [access_start, access_end) must be entirely within [alloc_start, alloc_end) */
            if (access_start >= alloc_start && access_end <= alloc_end) {
                return 1;
            }
        } else {
            /* Zero-size access: valid iff ptr points to an allocated byte (alloc_start <= ptr < alloc_end) */
            if (access_start >= alloc_start && access_start < alloc_end) {
                return 1;
            }
        }

        curr = curr->next;
    }

    /* Pointer/range is outside all tracked allocations */
    return 0;
}

void mg_check_access(const void *ptr, size_t access_size) {
    if (!mg_is_valid_access(ptr, access_size)) {
        fprintf(stderr,
                "MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=%p, access_size=%zu bytes\n",
                ptr, access_size);
        fflush(stderr);
        abort();
    }
}

int mg_register_external(void *ptr, size_t size) {
    if (ptr == NULL) {
        return 0;
    }

    /* Reject duplicate registration of the same base pointer */
    if (mg_is_tracked(ptr)) {
        return 0;
    }

    Allocation *node = (Allocation *)malloc(sizeof(Allocation));
    if (node == NULL) {
        return 0;
    }

    node->address = ptr;
    node->size = size;
    node->owns_payload = 0;

    node->next = allocation_registry;
    allocation_registry = node;

    return 1;
}

int mg_unregister_external(void *ptr) {
    if (ptr == NULL) {
        return 0;
    }

    Allocation *curr = allocation_registry;
    Allocation *prev = NULL;

    while (curr != NULL) {
        if (curr->address == ptr) {
            /* Unlink node from the registry */
            if (prev == NULL) {
                allocation_registry = curr->next;
            } else {
                prev->next = curr->next;
            }

            /* Release only the metadata node; caller retains payload ownership */
            free(curr);
            return 1;
        }
        prev = curr;
        curr = curr->next;
    }

    return 0; /* Pointer was not registered */
}

void *mg_realloc_external(void *ptr, size_t new_size) {
    if (ptr == NULL) {
        /* Standard realloc(NULL, size) allocates memory without registering it */
        return realloc(NULL, new_size);
    }

    /* Find the registry entry for ptr BEFORE calling realloc */
    Allocation *curr = allocation_registry;
    Allocation *prev = NULL;
    while (curr != NULL && curr->address != ptr) {
        prev = curr;
        curr = curr->next;
    }

    /* If ptr is not tracked by MemoryGuard, delegate directly to standard realloc */
    if (curr == NULL) {
        return realloc(ptr, new_size);
    }

    /* ptr is tracked in MemoryGuard */
    if (new_size == 0) {
        void *new_ptr = realloc(ptr, 0);
        if (new_ptr != NULL) {
            /* Platform returned a non-null zero-sized buffer */
            curr->address = new_ptr;
            curr->size = 0;
            return new_ptr;
        } else {
            /* Platform deallocated the buffer and returned NULL */
            if (prev == NULL) {
                allocation_registry = curr->next;
            } else {
                prev->next = curr->next;
            }
            free(curr);
            return NULL;
        }
    }

    /* new_size > 0: call standard realloc */
    void *new_ptr = realloc(ptr, new_size);
    if (new_ptr == NULL) {
        /* Reallocation failed. Standard realloc guarantees the original memory
         * block at ptr remains allocated and intact.
         * The original registry entry is preserved untouched without any modification. */
        return NULL;
    }

    /* Reallocation succeeded.
     * Update the existing registry node in-place with the new address and size.
     * Note: 'ptr' is now invalidated if moved; we do not read or pass 'ptr'
     * again, avoiding stale or dangling pointer bugs. */
    curr->address = new_ptr;
    curr->size = new_size;
    return new_ptr;
}

void *mg_intercept_malloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        return NULL;
    }

    if (!mg_register_external(ptr, size)) {
        free(ptr);
        return NULL;
    }

    return ptr;
}

void *mg_intercept_calloc(size_t nmemb, size_t size) {
    /* Prevent integer multiplication overflow */
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return NULL;
    }

    size_t total_size = nmemb * size;
    void *ptr = calloc(nmemb, size);
    if (ptr == NULL) {
        return NULL;
    }

    if (!mg_register_external(ptr, total_size)) {
        free(ptr);
        return NULL;
    }

    return ptr;
}

void *mg_intercept_realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return mg_intercept_malloc(size);
    }

    return mg_realloc_external(ptr, size);
}

void mg_intercept_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    mg_unregister_external(ptr);
    free(ptr);
}


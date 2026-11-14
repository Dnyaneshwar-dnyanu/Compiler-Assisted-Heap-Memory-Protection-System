#include "memoryguard.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

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

/**
 * @brief Mutex protecting the allocation registry and its metadata nodes.
 *
 * Ensures thread-safe access to allocation_registry across concurrent allocations,
 * deallocations, queries, and boundary checks.
 */
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Finds an allocation metadata entry by exact base address.
 *
 * MUST be called with registry_mutex held.
 *
 * @param ptr Base address to find.
 * @param prev_out If non-NULL, receives the pointer to the previous node in the list.
 * @return Allocation* Pointer to matching metadata node, or NULL if not found.
 */
static Allocation *find_allocation_locked(const void *ptr, Allocation **prev_out) {
    Allocation *curr = allocation_registry;
    Allocation *prev = NULL;

    while (curr != NULL) {
        if (curr->address == ptr) {
            if (prev_out != NULL) {
                *prev_out = prev;
            }
            return curr;
        }
        prev = curr;
        curr = curr->next;
    }

    if (prev_out != NULL) {
        *prev_out = NULL;
    }
    return NULL;
}

/**
 * @brief Checks if a base address is currently registered.
 *
 * MUST be called with registry_mutex held.
 *
 * @param ptr Base address to check.
 * @return int 1 if tracked, 0 otherwise.
 */
static int is_tracked_locked(const void *ptr) {
    if (ptr == NULL) {
        return 0;
    }
    return find_allocation_locked(ptr, NULL) != NULL;
}

/**
 * @brief Validates whether a memory access range lies entirely within a single tracked allocation.
 *
 * MUST be called with registry_mutex held.
 *
 * @param ptr Starting address of the memory access.
 * @param access_size Size of the memory access in bytes.
 * @return int 1 if valid, 0 otherwise.
 */
static int is_valid_access_locked(const void *ptr, size_t access_size) {
    if (ptr == NULL) {
        return 0;
    }

    uintptr_t access_start = (uintptr_t)ptr;
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

    return 0;
}

void *mg_malloc(size_t size) {
    /* Step 1: Allocate requested memory payload using standard malloc outside the lock */
    void *payload = malloc(size);
    if (payload == NULL) {
        return NULL;
    }

    /* Step 2: Allocate metadata node for registry tracking outside the lock */
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

    /* Step 4: Register node by prepending it to the linked list under mutex */
    pthread_mutex_lock(&registry_mutex);
    node->next = allocation_registry;
    allocation_registry = node;
    pthread_mutex_unlock(&registry_mutex);

    return payload;
}

void mg_free(void *ptr) {
    /* Handle NULL pointer safely */
    if (ptr == NULL) {
        return;
    }

    pthread_mutex_lock(&registry_mutex);
    Allocation *prev = NULL;
    Allocation *curr = find_allocation_locked(ptr, &prev);

    if (curr != NULL) {
        /* Unlink the metadata node from the registry */
        if (prev == NULL) {
            allocation_registry = curr->next;
        } else {
            prev->next = curr->next;
        }

        int owns = curr->owns_payload;
        void *addr = curr->address;

        pthread_mutex_unlock(&registry_mutex);

        /* Release the underlying allocated memory if owned by MemoryGuard outside lock */
        if (owns) {
            free(addr);
        }

        /* Release the metadata node itself outside lock */
        free(curr);
        return;
    }

    pthread_mutex_unlock(&registry_mutex);

    /* If ptr is not found in the registry (unknown pointer or already freed),
     * safely do nothing. This prevents double-freeing and invalid free operations. */
}

int mg_is_tracked(const void *ptr) {
    if (ptr == NULL) {
        return 0;
    }

    pthread_mutex_lock(&registry_mutex);
    int res = is_tracked_locked(ptr);
    pthread_mutex_unlock(&registry_mutex);

    return res;
}

size_t mg_get_allocation_size(const void *ptr) {
    if (ptr == NULL) {
        return 0;
    }

    pthread_mutex_lock(&registry_mutex);
    Allocation *prev = NULL;
    Allocation *curr = find_allocation_locked(ptr, &prev);
    size_t size = (curr != NULL) ? curr->size : 0;
    pthread_mutex_unlock(&registry_mutex);

    return size;
}

size_t mg_get_tracked_count(void) {
    pthread_mutex_lock(&registry_mutex);
    size_t count = 0;
    const Allocation *curr = allocation_registry;
    while (curr != NULL) {
        count++;
        curr = curr->next;
    }
    pthread_mutex_unlock(&registry_mutex);
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

    pthread_mutex_lock(&registry_mutex);
    int res = is_valid_access_locked(ptr, access_size);
    pthread_mutex_unlock(&registry_mutex);

    return res;
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

    /* Allocate metadata node outside the lock */
    Allocation *node = (Allocation *)malloc(sizeof(Allocation));
    if (node == NULL) {
        return 0;
    }

    node->address = ptr;
    node->size = size;
    node->owns_payload = 0;

    pthread_mutex_lock(&registry_mutex);
    /* Reject duplicate registration of the same base pointer */
    if (is_tracked_locked(ptr)) {
        pthread_mutex_unlock(&registry_mutex);
        free(node);
        return 0;
    }

    node->next = allocation_registry;
    allocation_registry = node;
    pthread_mutex_unlock(&registry_mutex);

    return 1;
}

int mg_unregister_external(void *ptr) {
    if (ptr == NULL) {
        return 0;
    }

    pthread_mutex_lock(&registry_mutex);
    Allocation *prev = NULL;
    Allocation *curr = find_allocation_locked(ptr, &prev);

    if (curr != NULL) {
        /* Unlink node from the registry */
        if (prev == NULL) {
            allocation_registry = curr->next;
        } else {
            prev->next = curr->next;
        }
        pthread_mutex_unlock(&registry_mutex);

        /* Release only the metadata node; caller retains payload ownership */
        free(curr);
        return 1;
    }

    pthread_mutex_unlock(&registry_mutex);
    return 0; /* Pointer was not registered */
}

void *mg_realloc_external(void *ptr, size_t new_size) {
    if (ptr == NULL) {
        /* Standard realloc(NULL, size) allocates memory without registering it */
        return realloc(NULL, new_size);
    }

    pthread_mutex_lock(&registry_mutex);
    Allocation *prev = NULL;
    Allocation *curr = find_allocation_locked(ptr, &prev);

    /* If ptr is not tracked by MemoryGuard, delegate directly to standard realloc outside lock */
    if (curr == NULL) {
        pthread_mutex_unlock(&registry_mutex);
        return realloc(ptr, new_size);
    }

    /* ptr is tracked in MemoryGuard */
    if (new_size == 0) {
        void *new_ptr = realloc(ptr, 0);
        if (new_ptr != NULL) {
            /* Platform returned a non-null zero-sized buffer */
            curr->address = new_ptr;
            curr->size = 0;
            pthread_mutex_unlock(&registry_mutex);
            return new_ptr;
        } else {
            /* Platform deallocated the buffer and returned NULL */
            if (prev == NULL) {
                allocation_registry = curr->next;
            } else {
                prev->next = curr->next;
            }
            pthread_mutex_unlock(&registry_mutex);
            free(curr);
            return NULL;
        }
    }

    /* new_size > 0: call standard realloc while preserving atomic registry consistency */
    void *new_ptr = realloc(ptr, new_size);
    if (new_ptr == NULL) {
        /* Reallocation failed. Standard realloc guarantees the original memory
         * block at ptr remains allocated and intact.
         * The original registry entry is preserved untouched without any modification. */
        pthread_mutex_unlock(&registry_mutex);
        return NULL;
    }

    /* Reallocation succeeded.
     * Update the existing registry node in-place with the new address and size.
     * Note: 'ptr' is now invalidated if moved; we do not read or pass 'ptr'
     * again, avoiding stale or dangling pointer bugs. */
    curr->address = new_ptr;
    curr->size = new_size;
    pthread_mutex_unlock(&registry_mutex);
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

    /* If registration fails, release payload memory to prevent leak and return NULL */
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

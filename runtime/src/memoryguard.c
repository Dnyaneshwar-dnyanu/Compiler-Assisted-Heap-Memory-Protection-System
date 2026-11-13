#include "memoryguard.h"
#include <stdlib.h>

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

            /* Release the underlying allocated memory */
            free(curr->address);

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

#include "memoryguard.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

/**
 * @file memoryguard.c
 * @brief MemoryGuard Runtime Implementation with AVL Interval Tree Registry.
 *
 * Replaces the legacy singly linked list with a balanced AVL interval tree to
 * achieve O(log N) allocation lookup, interior-pointer boundary validation,
 * insertion, and deallocation.
 */

/**
 * @brief Metadata structure for an active heap memory allocation in the AVL Interval Tree.
 *
 * Each active allocation managed or tracked by MemoryGuard is represented by an
 * instance of this structure. The tree is ordered strictly by base address (`start`).
 * Each node maintains subtree `max_end` and `height` to facilitate logarithmic
 * interval searches and maintain AVL tree balance.
 */
typedef struct Allocation {
    void *address;              /**< Starting base address of the allocated memory */
    size_t size;                /**< Size of the allocated memory in bytes */
    uintptr_t start;            /**< (uintptr_t)address for fast address comparisons */
    uintptr_t end;              /**< start + size, safely clamped on overflow */
    uintptr_t max_end;          /**< Maximum end boundary in the subtree rooted at this node */
    int height;                 /**< Height of this node in the AVL tree */
    int owns_payload;           /**< 1 if allocated by mg_malloc, 0 if registered externally */
    struct Allocation *left;    /**< Left child (allocations with smaller base addresses) */
    struct Allocation *right;   /**< Right child (allocations with larger base addresses) */
} Allocation;

/**
 * @brief Root of the AVL interval tree storing active allocation metadata.
 */
static Allocation *allocation_registry_root = NULL;

/**
 * @brief Total count of currently active tracked allocations.
 */
static size_t total_allocations = 0;

/**
 * @brief Mutex protecting the allocation registry and all metadata operations.
 */
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Safely computes start + size, clamping to UINTPTR_MAX on integer overflow.
 */
static inline uintptr_t calculate_end(uintptr_t start, size_t size) {
    if (UINTPTR_MAX - start < size) {
        return UINTPTR_MAX;
    }
    return start + size;
}

/**
 * @brief Returns the height of an AVL node, or 0 if NULL.
 */
static inline int get_height(const Allocation *node) {
    return node ? node->height : 0;
}

/**
 * @brief Recomputes height and subtree max_end for a node based on its children.
 */
static inline void update_node(Allocation *node) {
    if (!node) {
        return;
    }

    int lh = get_height(node->left);
    int rh = get_height(node->right);
    node->height = 1 + (lh > rh ? lh : rh);

    uintptr_t m = node->end;
    if (node->left && node->left->max_end > m) {
        m = node->left->max_end;
    }
    if (node->right && node->right->max_end > m) {
        m = node->right->max_end;
    }
    node->max_end = m;
}

/**
 * @brief Returns the balance factor of an AVL node (left height - right height).
 */
static inline int get_balance(const Allocation *node) {
    return node ? (get_height(node->left) - get_height(node->right)) : 0;
}

/**
 * @brief Performs a standard right rotation around node y.
 */
static Allocation *rotate_right(Allocation *y) {
    Allocation *x = y->left;
    Allocation *T2 = x->right;

    x->right = y;
    y->left = T2;

    update_node(y);
    update_node(x);

    return x;
}

/**
 * @brief Performs a standard left rotation around node x.
 */
static Allocation *rotate_left(Allocation *x) {
    Allocation *y = x->right;
    Allocation *T2 = y->left;

    y->left = x;
    x->right = T2;

    update_node(x);
    update_node(y);

    return y;
}

/**
 * @brief Rebalances an AVL node if its balance factor exceeds +/-1.
 */
static Allocation *balance_node(Allocation *root) {
    if (!root) {
        return NULL;
    }

    update_node(root);
    int balance = get_balance(root);

    /* Left Heavy */
    if (balance > 1) {
        if (get_balance(root->left) < 0) {
            root->left = rotate_left(root->left);
        }
        return rotate_right(root);
    }

    /* Right Heavy */
    if (balance < -1) {
        if (get_balance(root->right) > 0) {
            root->right = rotate_right(root->right);
        }
        return rotate_left(root);
    }

    return root;
}

/**
 * @brief Inserts a new node into the AVL tree ordered by start address.
 *
 * @param root Current subtree root.
 * @param node Node to insert.
 * @param inserted Output flag set to 1 if inserted, 0 if duplicate base address.
 * @return Allocation* New subtree root after insertion and rebalancing.
 */
static Allocation *insert_avl(Allocation *root, Allocation *node, int *inserted) {
    if (root == NULL) {
        node->left = NULL;
        node->right = NULL;
        node->height = 1;
        node->max_end = node->end;
        *inserted = 1;
        return node;
    }

    if (node->start < root->start) {
        root->left = insert_avl(root->left, node, inserted);
    } else if (node->start > root->start) {
        root->right = insert_avl(root->right, node, inserted);
    } else {
        /* Duplicate base address */
        *inserted = 0;
        return root;
    }

    if (!(*inserted)) {
        return root;
    }

    return balance_node(root);
}

/**
 * @brief Finds the node with the minimum start address in a subtree.
 */
static Allocation *find_min_node(Allocation *node) {
    Allocation *curr = node;
    while (curr && curr->left != NULL) {
        curr = curr->left;
    }
    return curr;
}

/**
 * @brief Deletes the node with matching start address from the AVL tree.
 *
 * Detaches the exact node and returns it in *deleted_node so the caller can
 * inspect or free it. Rebalances all affected ancestors.
 *
 * @param root Current subtree root.
 * @param start Start address of the allocation to delete.
 * @param deleted_node Output pointer receiving the detached node.
 * @return Allocation* New subtree root after deletion and rebalancing.
 */
static Allocation *delete_avl(Allocation *root, uintptr_t start, Allocation **deleted_node) {
    if (root == NULL) {
        return NULL;
    }

    if (start < root->start) {
        root->left = delete_avl(root->left, start, deleted_node);
    } else if (start > root->start) {
        root->right = delete_avl(root->right, start, deleted_node);
    } else {
        *deleted_node = root;

        if (root->left == NULL) {
            Allocation *r = root->right;
            root->left = root->right = NULL;
            return r;
        } else if (root->right == NULL) {
            Allocation *l = root->left;
            root->left = root->right = NULL;
            return l;
        } else {
            /* Node with two children: replace with in-order successor */
            Allocation *successor = find_min_node(root->right);
            Allocation *detached_succ = NULL;
            root->right = delete_avl(root->right, successor->start, &detached_succ);

            successor->left = root->left;
            successor->right = root->right;
            root->left = root->right = NULL;

            root = successor;
        }
    }

    return balance_node(root);
}

/**
 * @brief Finds an allocation metadata entry by exact base address.
 *
 * MUST be called with registry_mutex held.
 *
 * @param ptr Base address to find.
 * @return Allocation* Pointer to matching metadata node, or NULL if not found.
 */
static Allocation *find_allocation_locked(const void *ptr) {
    if (ptr == NULL) {
        return NULL;
    }

    uintptr_t target = (uintptr_t)ptr;
    Allocation *curr = allocation_registry_root;

    while (curr != NULL) {
        if (target == curr->start) {
            return curr;
        } else if (target < curr->start) {
            curr = curr->left;
        } else {
            curr = curr->right;
        }
    }

    return NULL;
}

/**
 * @brief Recursively validates an access range against the AVL interval tree.
 *
 * Uses subtree max_end and BST start-address ordering to prune unpromising subtrees:
 *  1. If node->max_end < min_needed_end, no interval in this subtree can reach the access.
 *  2. If node contains the access, returns 1 immediately.
 *  3. Search left subtree if left->max_end >= min_needed_end.
 *  4. Search right subtree only if node->start < access_start and right->max_end >= min_needed_end.
 */
static int is_valid_access_node(const Allocation *node, uintptr_t access_start, uintptr_t access_end, size_t access_size) {
    if (!node) {
        return 0;
    }

    uintptr_t min_needed_end = (access_size > 0) ? access_end
        : ((access_start < UINTPTR_MAX) ? access_start + 1 : UINTPTR_MAX);

    /* Pruning 1: if max_end of this subtree is less than needed, no interval can contain it */
    if (node->max_end < min_needed_end) {
        return 0;
    }

    /* Check current node */
    uintptr_t alloc_start = node->start;
    uintptr_t alloc_end = node->end;

    if (access_size > 0) {
        if (access_start >= alloc_start && access_end <= alloc_end) {
            return 1;
        }
    } else {
        if (access_start >= alloc_start && access_start < alloc_end) {
            return 1;
        }
    }

    /* Check left subtree (allocations with start < node->start) */
    if (node->left && node->left->max_end >= min_needed_end) {
        if (is_valid_access_node(node->left, access_start, access_end, access_size)) {
            return 1;
        }
    }

    /* Check right subtree (allocations with start > node->start).
     * For a node in the right subtree to contain access_start, it MUST have start <= access_start.
     * Pruning 2: only search right subtree if node->start < access_start. */
    if (node->start < access_start && node->right && node->right->max_end >= min_needed_end) {
        if (is_valid_access_node(node->right, access_start, access_end, access_size)) {
            return 1;
        }
    }

    return 0;
}

void *mg_malloc(size_t size) {
    /* Step 1: Allocate memory payload using standard malloc outside the lock */
    void *payload = malloc(size);
    if (payload == NULL) {
        return NULL;
    }

    /* Step 2: Allocate metadata node outside the lock */
    Allocation *node = (Allocation *)malloc(sizeof(Allocation));
    if (node == NULL) {
        free(payload);
        return NULL;
    }

    uintptr_t start = (uintptr_t)payload;
    node->address = payload;
    node->size = size;
    node->start = start;
    node->end = calculate_end(start, size);
    node->max_end = node->end;
    node->height = 1;
    node->owns_payload = 1;
    node->left = NULL;
    node->right = NULL;

    /* Step 3: Insert node into AVL interval tree under mutex */
    pthread_mutex_lock(&registry_mutex);
    int inserted = 0;
    allocation_registry_root = insert_avl(allocation_registry_root, node, &inserted);
    if (!inserted) {
        pthread_mutex_unlock(&registry_mutex);
        free(node);
        free(payload);
        return NULL;
    }
    total_allocations++;
    pthread_mutex_unlock(&registry_mutex);

    return payload;
}

void mg_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    pthread_mutex_lock(&registry_mutex);
    Allocation *deleted = NULL;
    allocation_registry_root = delete_avl(allocation_registry_root, (uintptr_t)ptr, &deleted);

    if (deleted != NULL) {
        total_allocations--;
        int owns = deleted->owns_payload;
        void *addr = deleted->address;
        pthread_mutex_unlock(&registry_mutex);

        if (owns) {
            free(addr);
        }
        free(deleted);
        return;
    }

    pthread_mutex_unlock(&registry_mutex);
}

int mg_is_tracked(const void *ptr) {
    if (ptr == NULL) {
        return 0;
    }

    pthread_mutex_lock(&registry_mutex);
    int res = (find_allocation_locked(ptr) != NULL);
    pthread_mutex_unlock(&registry_mutex);

    return res;
}

size_t mg_get_allocation_size(const void *ptr) {
    if (ptr == NULL) {
        return 0;
    }

    pthread_mutex_lock(&registry_mutex);
    const Allocation *curr = find_allocation_locked(ptr);
    size_t size = (curr != NULL) ? curr->size : 0;
    pthread_mutex_unlock(&registry_mutex);

    return size;
}

size_t mg_get_tracked_count(void) {
    pthread_mutex_lock(&registry_mutex);
    size_t count = total_allocations;
    pthread_mutex_unlock(&registry_mutex);
    return count;
}

int mg_is_valid_access(const void *ptr, size_t access_size) {
    if (ptr == NULL) {
        return 0;
    }

    uintptr_t access_start = (uintptr_t)ptr;
    if (UINTPTR_MAX - access_start < access_size) {
        return 0;
    }
    uintptr_t access_end = access_start + access_size;

    pthread_mutex_lock(&registry_mutex);
    int res = is_valid_access_node(allocation_registry_root, access_start, access_end, access_size);
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

    Allocation *node = (Allocation *)malloc(sizeof(Allocation));
    if (node == NULL) {
        return 0;
    }

    uintptr_t start = (uintptr_t)ptr;
    node->address = ptr;
    node->size = size;
    node->start = start;
    node->end = calculate_end(start, size);
    node->max_end = node->end;
    node->height = 1;
    node->owns_payload = 0;
    node->left = NULL;
    node->right = NULL;

    pthread_mutex_lock(&registry_mutex);
    int inserted = 0;
    allocation_registry_root = insert_avl(allocation_registry_root, node, &inserted);
    if (!inserted) {
        pthread_mutex_unlock(&registry_mutex);
        free(node);
        return 0;
    }
    total_allocations++;
    pthread_mutex_unlock(&registry_mutex);

    return 1;
}

int mg_unregister_external(void *ptr) {
    if (ptr == NULL) {
        return 0;
    }

    pthread_mutex_lock(&registry_mutex);
    Allocation *deleted = NULL;
    allocation_registry_root = delete_avl(allocation_registry_root, (uintptr_t)ptr, &deleted);

    if (deleted != NULL) {
        total_allocations--;
        pthread_mutex_unlock(&registry_mutex);
        free(deleted);
        return 1;
    }

    pthread_mutex_unlock(&registry_mutex);
    return 0;
}

void *mg_realloc_external(void *ptr, size_t new_size) {
    if (ptr == NULL) {
        return realloc(NULL, new_size);
    }

    pthread_mutex_lock(&registry_mutex);
    Allocation *curr = find_allocation_locked(ptr);

    if (curr == NULL) {
        pthread_mutex_unlock(&registry_mutex);
        return realloc(ptr, new_size);
    }

    int owns = curr->owns_payload;

    if (new_size == 0) {
        void *new_ptr = realloc(ptr, 0);
        if (new_ptr != NULL) {
            /* Platform returned a non-null zero-sized buffer */
            Allocation *deleted = NULL;
            allocation_registry_root = delete_avl(allocation_registry_root, curr->start, &deleted);

            curr->address = new_ptr;
            curr->start = (uintptr_t)new_ptr;
            curr->size = 0;
            curr->end = curr->start;
            curr->max_end = curr->end;
            curr->owns_payload = owns;
            curr->height = 1;
            curr->left = NULL;
            curr->right = NULL;

            int inserted = 0;
            allocation_registry_root = insert_avl(allocation_registry_root, curr, &inserted);
            if (!inserted) {
                total_allocations--;
                free(curr);
            }
            pthread_mutex_unlock(&registry_mutex);
            return new_ptr;
        } else {
            /* Platform freed the buffer and returned NULL */
            Allocation *deleted = NULL;
            allocation_registry_root = delete_avl(allocation_registry_root, (uintptr_t)ptr, &deleted);
            if (deleted) {
                total_allocations--;
                free(deleted);
            }
            pthread_mutex_unlock(&registry_mutex);
            return NULL;
        }
    }

    /* new_size > 0 */
    void *new_ptr = realloc(ptr, new_size);
    if (new_ptr == NULL) {
        /* Realloc failed: standard realloc preserves original allocation untouched */
        pthread_mutex_unlock(&registry_mutex);
        return NULL;
    }

    /* Reallocation succeeded: re-insert into AVL tree to guarantee correct balance and max_end */
    Allocation *deleted = NULL;
    allocation_registry_root = delete_avl(allocation_registry_root, (uintptr_t)ptr, &deleted);

    curr->address = new_ptr;
    curr->start = (uintptr_t)new_ptr;
    curr->size = new_size;
    curr->end = calculate_end(curr->start, new_size);
    curr->max_end = curr->end;
    curr->owns_payload = owns;
    curr->height = 1;
    curr->left = NULL;
    curr->right = NULL;

    int inserted = 0;
    allocation_registry_root = insert_avl(allocation_registry_root, curr, &inserted);
    if (!inserted) {
        total_allocations--;
        free(curr);
    }

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

/**
 * @brief Recursive helper to verify all AVL interval tree invariants.
 */
static int verify_node_invariants(const Allocation *node, int has_min, uintptr_t min_addr, int has_max, uintptr_t max_addr, size_t *count) {
    if (!node) {
        return 1;
    }

    (*count)++;

    /* 1. BST ordering check */
    if (has_min && node->start <= min_addr) {
        return 0;
    }
    if (has_max && node->start >= max_addr) {
        return 0;
    }

    /* 2. End calculation check */
    uintptr_t expected_end = calculate_end(node->start, node->size);
    if (node->end != expected_end) {
        return 0;
    }

    /* 3. Subtree recursion */
    if (node->left && !verify_node_invariants(node->left, has_min, min_addr, 1, node->start, count)) {
        return 0;
    }
    if (node->right && !verify_node_invariants(node->right, 1, node->start, has_max, max_addr, count)) {
        return 0;
    }

    /* 4. Height check */
    int lh = get_height(node->left);
    int rh = get_height(node->right);
    if (node->height != 1 + (lh > rh ? lh : rh)) {
        return 0;
    }

    /* 5. AVL balance factor check */
    int balance = lh - rh;
    if (balance < -1 || balance > 1) {
        return 0;
    }

    /* 6. Subtree max_end check */
    uintptr_t expected_max_end = node->end;
    if (node->left && node->left->max_end > expected_max_end) {
        expected_max_end = node->left->max_end;
    }
    if (node->right && node->right->max_end > expected_max_end) {
        expected_max_end = node->right->max_end;
    }
    if (node->max_end != expected_max_end) {
        return 0;
    }

    return 1;
}

int mg_verify_tree_invariants(void) {
    pthread_mutex_lock(&registry_mutex);
    size_t count = 0;
    int ok = verify_node_invariants(allocation_registry_root, 0, 0, 0, 0, &count);
    if (ok && count != total_allocations) {
        ok = 0;
    }
    pthread_mutex_unlock(&registry_mutex);
    return ok;
}

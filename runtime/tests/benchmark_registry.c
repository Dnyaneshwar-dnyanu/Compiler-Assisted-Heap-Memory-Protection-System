#define _POSIX_C_SOURCE 199309L
#include "memoryguard.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

/**
 * @file benchmark_registry.c
 * @brief Benchmark comparing the legacy singly-linked list registry with the
 *        new balanced AVL interval tree registry at various scales.
 */

/* ========================================================================= */
/* Reference Singly-Linked List Registry Implementation                     */
/* ========================================================================= */

typedef struct OldAllocation {
    void *address;
    size_t size;
    uintptr_t start;
    uintptr_t end;
    struct OldAllocation *next;
} OldAllocation;

static OldAllocation *old_registry_head = NULL;

static void old_list_insert(void *ptr, size_t size) {
    OldAllocation *node = (OldAllocation *)malloc(sizeof(OldAllocation));
    if (!node) return;
    node->address = ptr;
    node->size = size;
    node->start = (uintptr_t)ptr;
    node->end = (UINTPTR_MAX - node->start < size) ? UINTPTR_MAX : (node->start + size);
    node->next = old_registry_head;
    old_registry_head = node;
}

static int old_list_is_valid_access(const void *ptr, size_t access_size) {
    if (!ptr) return 0;
    uintptr_t access_start = (uintptr_t)ptr;
    if (UINTPTR_MAX - access_start < access_size) return 0;
    uintptr_t access_end = access_start + access_size;

    OldAllocation *curr = old_registry_head;
    while (curr) {
        if (access_size > 0) {
            if (access_start >= curr->start && access_end <= curr->end) {
                return 1;
            }
        } else {
            if (access_start >= curr->start && access_start < curr->end) {
                return 1;
            }
        }
        curr = curr->next;
    }
    return 0;
}

static void old_list_delete(void *ptr) {
    if (!ptr) return;
    OldAllocation *curr = old_registry_head;
    OldAllocation *prev = NULL;
    while (curr) {
        if (curr->address == ptr) {
            if (prev) {
                prev->next = curr->next;
            } else {
                old_registry_head = curr->next;
            }
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

static void old_list_clear(void) {
    OldAllocation *curr = old_registry_head;
    while (curr) {
        OldAllocation *next = curr->next;
        free(curr);
        curr = next;
    }
    old_registry_head = NULL;
}

/* ========================================================================= */
/* Timing Utilities                                                         */
/* ========================================================================= */

static double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ========================================================================= */
/* Benchmark Runner                                                          */
/* ========================================================================= */

static void run_benchmark_for_scale(size_t N) {
    printf("\n--------------------------------------------------------------------------------\n");
    printf(" Scale: N = %zu allocations\n", N);
    printf("--------------------------------------------------------------------------------\n");

    /* Allocate dummy address buffers for consistent tracking tests */
    void **ptrs = (void **)malloc(N * sizeof(void *));
    size_t *sizes = (size_t *)malloc(N * sizeof(size_t));
    if (!ptrs || !sizes) {
        fprintf(stderr, "Host memory allocation failed for N = %zu\n", N);
        return;
    }

    /* Fabricate non-overlapping simulated base addresses and sizes */
    uintptr_t base = 0x10000000;
    for (size_t i = 0; i < N; i++) {
        sizes[i] = 32 + (i % 256);
        ptrs[i] = (void *)base;
        base += sizes[i] + 32; /* Guarantee disjoint base intervals */
    }

    /* --------------------------------------------------------------------- */
    /* 1. Insertion Benchmark                                                */
    /* --------------------------------------------------------------------- */
    double t0, t1;

    /* Legacy linked list insertion */
    t0 = get_time_sec();
    for (size_t i = 0; i < N; i++) {
        old_list_insert(ptrs[i], sizes[i]);
    }
    t1 = get_time_sec();
    double old_insert_time_ms = (t1 - t0) * 1000.0;

    /* AVL Interval Tree insertion */
    t0 = get_time_sec();
    for (size_t i = 0; i < N; i++) {
        mg_register_external(ptrs[i], sizes[i]);
    }
    t1 = get_time_sec();
    double avl_insert_time_ms = (t1 - t0) * 1000.0;

    printf("  [Insertion %zu items]\n", N);
    printf("    Linked List: %8.3f ms\n", old_insert_time_ms);
    printf("    AVL Tree:    %8.3f ms\n", avl_insert_time_ms);

    /* --------------------------------------------------------------------- */
    /* 2. Interior Pointer Lookup Benchmark                                  */
    /* --------------------------------------------------------------------- */
    /* Number of lookup queries */
    size_t num_lookups = (N >= 100000) ? 2000 : 5000;

    /* Pre-generate pseudo-random interior query pointers */
    void **query_ptrs = (void **)malloc(num_lookups * sizeof(void *));
    size_t *query_lens = (size_t *)malloc(num_lookups * sizeof(size_t));
    uint32_t seed = 424242;
    for (size_t i = 0; i < num_lookups; i++) {
        seed = seed * 1664525u + 1013904223u;
        size_t idx = seed % N;
        size_t offset = (seed >> 8) % sizes[idx];
        query_ptrs[i] = (char *)ptrs[idx] + offset;
        query_lens[i] = 1;
    }

    /* Legacy linked list lookups */
    t0 = get_time_sec();
    int old_valid_count = 0;
    for (size_t i = 0; i < num_lookups; i++) {
        old_valid_count += old_list_is_valid_access(query_ptrs[i], query_lens[i]);
    }
    t1 = get_time_sec();
    double old_lookup_time_ms = (t1 - t0) * 1000.0;

    /* AVL Interval Tree lookups */
    t0 = get_time_sec();
    int avl_valid_count = 0;
    for (size_t i = 0; i < num_lookups; i++) {
        avl_valid_count += mg_is_valid_access(query_ptrs[i], query_lens[i]);
    }
    t1 = get_time_sec();
    double avl_lookup_time_ms = (t1 - t0) * 1000.0;

    double lookup_speedup = (avl_lookup_time_ms > 0.0001) ? (old_lookup_time_ms / avl_lookup_time_ms) : 9999.0;
    printf("  [Interior Lookup %zu queries (valid: %d vs %d)]\n", num_lookups, old_valid_count, avl_valid_count);
    printf("    Linked List: %8.3f ms  (%6.2f ns/op)\n",
           old_lookup_time_ms, (old_lookup_time_ms * 1e6) / num_lookups);
    printf("    AVL Tree:    %8.3f ms  (%6.2f ns/op)\n",
           avl_lookup_time_ms, (avl_lookup_time_ms * 1e6) / num_lookups);
    printf("    => Speedup:  %8.1fx FASTER with AVL Interval Tree\n", lookup_speedup);

    /* --------------------------------------------------------------------- */
    /* 3. Deletion Benchmark                                                 */
    /* --------------------------------------------------------------------- */
    size_t num_deletes = (N >= 100000) ? 2000 : ((N >= 10000) ? 5000 : N);

    /* Select pointers to delete in shuffled order */
    void **del_ptrs = (void **)malloc(num_deletes * sizeof(void *));
    for (size_t i = 0; i < num_deletes; i++) {
        del_ptrs[i] = ptrs[i];
    }
    for (size_t i = num_deletes - 1; i > 0; i--) {
        seed = seed * 1664525u + 1013904223u;
        size_t j = seed % (i + 1);
        void *tmp = del_ptrs[i];
        del_ptrs[i] = del_ptrs[j];
        del_ptrs[j] = tmp;
    }

    /* Legacy linked list deletions */
    t0 = get_time_sec();
    for (size_t i = 0; i < num_deletes; i++) {
        old_list_delete(del_ptrs[i]);
    }
    t1 = get_time_sec();
    double old_delete_time_ms = (t1 - t0) * 1000.0;

    /* AVL Interval Tree deletions */
    t0 = get_time_sec();
    for (size_t i = 0; i < num_deletes; i++) {
        mg_unregister_external(del_ptrs[i]);
    }
    t1 = get_time_sec();
    double avl_delete_time_ms = (t1 - t0) * 1000.0;

    double delete_speedup = (avl_delete_time_ms > 0.0001) ? (old_delete_time_ms / avl_delete_time_ms) : 9999.0;
    printf("  [Deletion %zu items]\n", num_deletes);
    printf("    Linked List: %8.3f ms  (%6.2f ns/op)\n",
           old_delete_time_ms, (old_delete_time_ms * 1e6) / num_deletes);
    printf("    AVL Tree:    %8.3f ms  (%6.2f ns/op)\n",
           avl_delete_time_ms, (avl_delete_time_ms * 1e6) / num_deletes);
    printf("    => Speedup:  %8.1fx FASTER with AVL Interval Tree\n", delete_speedup);

    /* Cleanup remaining items */
    old_list_clear();
    for (size_t i = num_deletes; i < N; i++) {
        mg_unregister_external(ptrs[i]);
    }

    free(query_ptrs);
    free(query_lens);
    free(del_ptrs);
    free(ptrs);
    free(sizes);
}

int main(void) {
    printf("================================================================================\n");
    printf("    MemoryGuard Allocation Registry Benchmark: Linked List vs AVL Interval Tree\n");
    printf("================================================================================\n");
    printf("Theoretical Complexities:\n");
    printf("  Operation              Linked List     AVL Interval Tree\n");
    printf("  --------------------------------------------------------\n");
    printf("  Base Address Lookup    O(N)            O(log N)\n");
    printf("  Interior Range Query   O(N)            O(log N)\n");
    printf("  Insertion              O(1) / O(N)     O(log N)\n");
    printf("  Deallocation           O(N)            O(log N)\n");

    size_t scales[] = {100, 1000, 10000, 100000};
    size_t num_scales = sizeof(scales) / sizeof(scales[0]);

    for (size_t i = 0; i < num_scales; i++) {
        run_benchmark_for_scale(scales[i]);
    }

    printf("\n================================================================================\n");
    printf(" Benchmark Completed Successfully!\n");
    printf("================================================================================\n");

    return 0;
}

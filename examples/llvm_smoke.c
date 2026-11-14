#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* Global variable for non-heap static memory origin test */
static int global_counter = 0;

/* Function 1: Local variable write and read (Stack variables) */
int test_local_variable(int x) {
    int local_var = x + 5;   /* Write to local variable (NON_HEAP) */
    return local_var * 2;    /* Read from local variable (NON_HEAP) */
}

/* Function 2: Array element write and read (Stack array indexing) */
int test_array_access(int index, int value) {
    int arr[10];
    arr[index] = value;      /* Write to array element (NON_HEAP) */
    return arr[index];       /* Read from array element (NON_HEAP) */
}

/* Function 3: Pointer-based reads and writes (Pointer parameters) */
void test_pointer_access(int *dest, const int *src) {
    int temp = *src;         /* Pointer-based read from parameter (UNKNOWN) */
    *dest = temp + 42;       /* Pointer-based write to parameter (UNKNOWN) */
}

/* Function 4: Heap allocation using malloc (constant and dynamic) */
void test_malloc_allocation(size_t dynamic_size) {
    int *fixed_buf = (int *)malloc(64);
    if (fixed_buf) {
        fixed_buf[0] = 100;  /* Heap candidate write */
        free(fixed_buf);
    }

    char *dyn_buf = (char *)malloc(dynamic_size);
    if (dyn_buf) {
        dyn_buf[0] = 'M';   /* Heap candidate write */
        free(dyn_buf);
    }
}

/* Function 5: Heap allocation using calloc (constant and dynamic) */
void test_calloc_allocation(size_t dynamic_count) {
    int *fixed_arr = (int *)calloc(10, sizeof(int));
    if (fixed_arr) {
        fixed_arr[0] = 200;  /* Heap candidate write */
        free(fixed_arr);
    }

    double *dyn_arr = (double *)calloc(dynamic_count, sizeof(double));
    if (dyn_arr) {
        dyn_arr[0] = 3.14;   /* Heap candidate write */
        free(dyn_arr);
    }
}

/* Function 6: Heap reallocation using realloc (safe pattern without UAF) */
void test_realloc_allocation(size_t new_size) {
    int *ptr = (int *)malloc(32);
    if (!ptr) {
        return;
    }

    ptr[0] = 42;             /* Heap candidate write */

    /* Reallocate: assign to new_ptr; ptr is not used after realloc */
    int *new_ptr = (int *)realloc(ptr, 64);
    if (new_ptr) {
        new_ptr[1] = 84;     /* Heap candidate write */

        int *dyn_ptr = (int *)realloc(new_ptr, new_size);
        if (dyn_ptr) {
            dyn_ptr[0] = 99; /* Heap candidate write */
            free(dyn_ptr);
        } else {
            free(new_ptr);
        }
    } else {
        free(ptr);
    }
}

/* Function 7: Pointer copy tracing */
int test_pointer_copy(void) {
    int *p = (int *)malloc(32);
    if (!p) {
        return 0;
    }
    int *copy = p;           /* Pointer copy */
    copy[0] = 77;            /* Write through copied pointer (HEAP_CANDIDATE) */
    int val = copy[0];       /* Read through copied pointer (HEAP_CANDIDATE) */
    free(p);
    return val;
}

/* Function 8: Heap array indexing */
int test_heap_array_indexing(int index, int val) {
    int *arr = (int *)malloc(10 * sizeof(int));
    if (!arr) {
        return 0;
    }
    arr[index] = val;        /* Write to heap array element via GEP (HEAP_CANDIDATE) */
    int result = arr[index]; /* Read from heap array element via GEP (HEAP_CANDIDATE) */
    free(arr);
    return result;
}

/* Function 9: Conditional pointer selection (PHI / Select) */
int test_conditional_pointer(int cond) {
    int *p1 = (int *)malloc(16);
    int *p2 = (int *)malloc(16);
    if (!p1 || !p2) {
        free(p1);
        free(p2);
        return 0;
    }
    int *chosen = cond ? p1 : p2;  /* Conditional pointer selection */
    chosen[0] = 55;                /* Write through selected pointer (HEAP_CANDIDATE) */
    int res = chosen[0];           /* Read through selected pointer (HEAP_CANDIDATE) */
    free(p1);
    free(p2);
    return res;
}

/* Function 10: Global variable access */
int test_global_access(int val) {
    global_counter = val;    /* Write to global variable (NON_HEAP) */
    return global_counter;   /* Read from global variable (NON_HEAP) */
}

/* Regression Test 1: Select expression with same heap pointer in both branches */
int test_regression_select_same_heap_pointer(int cond) {
    int *p = (int *)malloc(32);
    if (!p) {
        return 0;
    }
    /* Both branches evaluate to the exact same heap pointer 'p' */
    int *chosen = cond ? p : p;
    chosen[0] = 11;          /* Proven HEAP_CANDIDATE via path-based visited set */
    int val = chosen[0];     /* Proven HEAP_CANDIDATE via path-based visited set */
    free(p);
    return val;
}

/* Regression Test 2: PHI node with repeated incoming heap pointer values */
int test_regression_phi_repeated_heap_pointer(int count, int val) {
    int *p = (int *)malloc(32);
    if (!p) {
        return 0;
    }
    int *cur = p;
    for (int i = 0; i < count; i++) {
        cur[0] = val;
        cur = (i % 2 == 0) ? p : p; /* PHI in loop header with repeated 'p' */
    }
    int res = cur[0];
    free(p);
    return res;
}

/* Regression Test 3: Pointer variable initialized on only one conditional branch */
int test_regression_uninitialized_branch(int cond) {
    int *p;
    if (cond) {
        p = (int *)malloc(32);
    }
    /* At this merge point, the store inside the 'then' block does NOT dominate.
     * The analysis must conservatively classify this as UNKNOWN. */
    if (cond) {
        p[0] = 33;           /* Store to p does not dominate -> UNKNOWN */
        int val = p[0];      /* Load from p does not dominate -> UNKNOWN */
        free(p);
        return val;
    }
    return 0;
}

/* Regression Test 4: Pointer variable with multiple stores of the same heap origin */
int test_regression_multiple_stores_same_origin(int cond) {
    int *p = (int *)malloc(32);
    p[0] = 1;                /* Dominating store in same block -> HEAP_CANDIDATE */

    if (cond) {
        free(p);
        p = (int *)malloc(64);
        if (!p) {
            return 0;
        }
    }
    /* After the if-statement, multiple stores exist and neither store strictly
     * dominates the load. The analysis must NOT fall back to "all stores agree"
     * and must return UNKNOWN conservatively. */
    p[0] = 44;               /* Access without proven dominating store -> UNKNOWN */
    int val = p[0];          /* Access without proven dominating store -> UNKNOWN */
    free(p);
    return val;
}

/* Regression Test 5: Integer-to-pointer and pointer-to-integer conversions */
int test_regression_int_to_ptr(uintptr_t addr) {
    int *p = (int *)addr;    /* IntToPtr cast */
    *p = 88;                 /* IntToPtr cast destination -> UNKNOWN */
    int val = *p;            /* IntToPtr cast source -> UNKNOWN */

    int *h = (int *)malloc(16);
    if (h) {
        uintptr_t raw = (uintptr_t)h;    /* PtrToInt cast */
        int *reconstructed = (int *)raw; /* IntToPtr cast */
        *reconstructed = 99;             /* Round-trip cast -> UNKNOWN */
        free(h);
    }
    return val;
}

/* Regression Test 6: Pointer parameters */
int test_regression_pointer_parameters(int *param_dest, const int *param_src) {
    int val = *param_src;    /* Pointer parameter dereference -> UNKNOWN */
    *param_dest = val + 1;   /* Pointer parameter dereference -> UNKNOWN */
    return val;
}

/* Main driver invoking all test cases */
int main(void) {
    int val = test_local_variable(10);
    int arr_val = test_array_access(3, 99);

    int output = 0;
    test_pointer_access(&output, &val);

    test_malloc_allocation(128);
    test_calloc_allocation(5);
    test_realloc_allocation(256);

    int copy_val = test_pointer_copy();
    int heap_arr_val = test_heap_array_indexing(2, 50);
    int cond_val = test_conditional_pointer(1);
    int glob_val = test_global_access(123);

    int reg_sel = test_regression_select_same_heap_pointer(1);
    int reg_phi = test_regression_phi_repeated_heap_pointer(1, 42);
    int reg_uninit = test_regression_uninitialized_branch(0);
    int reg_multi = test_regression_multiple_stores_same_origin(0);
    int reg_param = test_regression_pointer_parameters(&output, &val);

    printf("Summary: %d %d %d %d %d %d %d %d %d %d %d %d\n",
           val, arr_val, output, copy_val, heap_arr_val, cond_val, glob_val,
           reg_sel, reg_phi, reg_uninit, reg_multi, reg_param);
    return 0;
}

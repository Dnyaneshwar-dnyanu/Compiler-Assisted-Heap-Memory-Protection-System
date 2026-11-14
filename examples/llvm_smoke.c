#include <stdio.h>
#include <stdlib.h>

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

/* Function 11: Main driver invoking all test cases */
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

    printf("Local: %d, Array: %d, Pointer: %d, Copy: %d, HeapArr: %d, Cond: %d, Glob: %d\n",
           val, arr_val, output, copy_val, heap_arr_val, cond_val, glob_val);
    return 0;
}

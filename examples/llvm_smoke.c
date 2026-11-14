#include <stdio.h>
#include <stdlib.h>

/* Function 1: Local variable write and read */
int test_local_variable(int x) {
    int local_var = x + 5;   /* Write to local variable */
    return local_var * 2;    /* Read from local variable */
}

/* Function 2: Array element write and read */
int test_array_access(int index, int value) {
    int arr[10];
    arr[index] = value;      /* Write to array element */
    return arr[index];       /* Read from array element */
}

/* Function 3: Pointer-based reads and writes */
void test_pointer_access(int *dest, const int *src) {
    int temp = *src;         /* Pointer-based read */
    *dest = temp + 42;       /* Pointer-based write */
}

/* Function 4: Heap allocation using malloc (constant and dynamic) */
void test_malloc_allocation(size_t dynamic_size) {
    int *fixed_buf = (int *)malloc(64);
    if (fixed_buf) {
        fixed_buf[0] = 100;
        free(fixed_buf);
    }

    char *dyn_buf = (char *)malloc(dynamic_size);
    if (dyn_buf) {
        dyn_buf[0] = 'M';
        free(dyn_buf);
    }
}

/* Function 5: Heap allocation using calloc (constant and dynamic) */
void test_calloc_allocation(size_t dynamic_count) {
    int *fixed_arr = (int *)calloc(10, sizeof(int));
    if (fixed_arr) {
        fixed_arr[0] = 200;
        free(fixed_arr);
    }

    double *dyn_arr = (double *)calloc(dynamic_count, sizeof(double));
    if (dyn_arr) {
        dyn_arr[0] = 3.14;
        free(dyn_arr);
    }
}

/* Function 6: Heap reallocation using realloc (safe pattern without UAF) */
void test_realloc_allocation(size_t new_size) {
    int *ptr = (int *)malloc(32);
    if (!ptr) {
        return;
    }

    ptr[0] = 42;

    /* Reallocate: assign to new_ptr; ptr is not used after realloc */
    int *new_ptr = (int *)realloc(ptr, 64);
    if (new_ptr) {
        new_ptr[1] = 84;

        int *dyn_ptr = (int *)realloc(new_ptr, new_size);
        if (dyn_ptr) {
            dyn_ptr[0] = 99;
            free(dyn_ptr);
        } else {
            free(new_ptr);
        }
    } else {
        free(ptr);
    }
}

/* Function 7: Main driver invoking all test cases */
int main(void) {
    int val = test_local_variable(10);
    int arr_val = test_array_access(3, 99);

    int output = 0;
    test_pointer_access(&output, &val);

    test_malloc_allocation(128);
    test_calloc_allocation(5);
    test_realloc_allocation(256);

    printf("Local: %d, Array: %d, Pointer: %d\n", val, arr_val, output);
    return 0;
}

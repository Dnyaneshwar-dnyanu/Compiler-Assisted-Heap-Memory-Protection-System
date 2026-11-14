#include <stdio.h>

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

/* Function 4: Main driver invoking all test cases */
int main(void) {
    int val = test_local_variable(10);
    int arr_val = test_array_access(3, 99);

    int output = 0;
    test_pointer_access(&output, &val);

    printf("Local: %d, Array: %d, Pointer: %d\n", val, arr_val, output);
    return 0;
}

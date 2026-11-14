#include <stdio.h>

void helper_function(void) {
    printf("MemoryGuard LLVM smoke test: helper_function called.\n");
}

int calculate_value(int a, int b) {
    return a + b;
}

int main(void) {
    helper_function();
    int result = calculate_value(10, 20);
    printf("Calculated result: %d\n", result);
    return 0;
}

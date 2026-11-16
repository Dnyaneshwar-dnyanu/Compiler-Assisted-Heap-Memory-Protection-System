export const SAMPLE_CODES = {
  c: [
    {
      id: 'c-clean',
      title: 'Valid Heap Access (Clean)',
      description: 'Allocates a heap buffer, performs in-bounds writes/reads, and safely frees memory.',
      code: `#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("[Program] Allocating heap buffer for 5 integers...\\n");
    int *array = (int *)malloc(5 * sizeof(int));
    if (!array) {
        return 1;
    }

    // Valid accesses: indices 0 through 4
    for (int i = 0; i < 5; i++) {
        array[i] = (i + 1) * 10;
        printf("  array[%d] = %d\\n", i, array[i]);
    }

    printf("[Program] Freeing heap allocation...\\n");
    free(array);

    printf("[Program] Execution finished cleanly.\\n");
    return 0;
}
`
    },
    {
      id: 'c-oob',
      title: 'Heap Out-of-Bounds Write (Violation)',
      description: 'Attempts to write past the allocated boundary, triggering a MemoryGuard violation.',
      code: `#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("[Program] Allocating heap buffer for 4 integers (16 bytes)...\\n");
    int *buffer = (int *)malloc(4 * sizeof(int));
    if (!buffer) {
        return 1;
    }

    // Valid write within bounds
    buffer[0] = 100;
    buffer[1] = 200;
    printf("[Program] In-bounds writes succeeded.\\n");

    // Out-of-bounds write: index 5 is 24 bytes from base (exceeds 16-byte boundary)
    printf("[Program] Attempting out-of-bounds write to buffer[5]...\\n");
    buffer[5] = 999; // <--- MemoryGuard will intercept this and abort!

    printf("[Program] This line should never execute!\\n");
    free(buffer);
    return 0;
}
`
    },
    {
      id: 'c-uaf',
      title: 'Heap Use-After-Free (Violation)',
      description: 'Frees an allocation and then attempts to store through the dangling pointer.',
      code: `#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("[Program] Allocating heap buffer...\\n");
    int *ptr = (int *)malloc(sizeof(int));
    if (!ptr) {
        return 1;
    }

    *ptr = 42;
    printf("[Program] Value stored: %d\\n", *ptr);

    printf("[Program] Freeing buffer at %p...\\n", (void *)ptr);
    free(ptr);

    // Use-After-Free: ptr is no longer tracked in the active registry
    printf("[Program] Attempting use-after-free write to dangling pointer...\\n");
    *ptr = 1337; // <--- MemoryGuard will detect unallocated/freed pointer and abort!

    printf("[Program] Unreachable line!\\n");
    return 0;
}
`
    },
    {
      id: 'c-interior',
      title: 'Interior Pointer Arithmetic (Clean)',
      description: 'Performs valid pointer offsets and interior pointer dereferencing within bounds.',
      code: `#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("[Program] Allocating 100-byte buffer...\\n");
    char *buf = (char *)malloc(100);
    if (!buf) {
        return 1;
    }

    // Valid interior pointer access at offset 50
    char *interior = buf + 50;
    *interior = 'M';
    *(interior + 1) = 'G';

    printf("[Program] Interior characters: %c%c\\n", *interior, *(interior + 1));
    free(buf);
    return 0;
}
`
    }
  ],
  cpp: [
    {
      id: 'cpp-clean',
      title: 'C++ Valid Heap Allocation (Clean)',
      description: 'Allocates dynamic objects using C++ new/delete with safe boundary checks.',
      code: `#include <iostream>

struct SensorData {
    int id;
    double reading;
};

int main() {
    std::cout << "[Program] Allocating SensorData object on heap...\\n";
    SensorData *sensor = new SensorData();
    sensor->id = 101;
    sensor->reading = 98.6;

    std::cout << "  Sensor ID: " << sensor->id << ", Reading: " << sensor->reading << "\\n";

    delete sensor;
    std::cout << "[Program] Deleted SensorData cleanly.\\n";
    return 0;
}
`
    },
    {
      id: 'cpp-oob',
      title: 'C++ Heap Array Out-of-Bounds (Violation)',
      description: 'Allocates dynamic array with new[] and attempts an out-of-bounds write.',
      code: `#include <iostream>

int main() {
    std::cout << "[Program] Allocating dynamic int array of size 5...\\n";
    int *nums = new int[5];

    for (int i = 0; i < 5; ++i) {
        nums[i] = (i + 1) * 11;
    }

    // Violation: index 7 is beyond the 5-element allocation boundary
    std::cout << "[Program] Attempting out-of-bounds write at nums[7]...\\n";
    nums[7] = 777; // <--- MemoryGuard will intercept and abort!

    delete[] nums;
    return 0;
}
`
    },
    {
      id: 'cpp-uaf',
      title: 'C++ Use-After-Free (Violation)',
      description: 'Deallocates heap object and dereferences the dangling pointer.',
      code: `#include <iostream>

int main() {
    std::cout << "[Program] Allocating double on heap...\\n";
    double *val = new double(3.14159);
    std::cout << "  Value: " << *val << "\\n";

    delete val;
    std::cout << "[Program] Object deallocated.\\n";

    // Use-after-free
    std::cout << "[Program] Writing to deallocated double pointer...\\n";
    *val = 2.71828; // <--- MemoryGuard intercepts invalid access!

    return 0;
}
`
    }
  ]
};

export function getDefaultSample(language = 'c') {
  const samples = SAMPLE_CODES[language] || SAMPLE_CODES.c;
  return samples[0];
}

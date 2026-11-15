#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/**
 * @file benchmark_suite.c
 * @brief MemoryGuard Performance Benchmark Workloads.
 *
 * Implements 6 representative workloads to evaluate:
 *   1. Repeated malloc/free operations.
 *   2. Sequential heap reads and writes.
 *   3. Random heap accesses within valid bounds.
 *   4. Multiple co-existing allocations of variable sizes.
 *   5. Interprocedural pointer passing across function boundaries.
 *   6. Multithreaded concurrent allocation and access under lock contention.
 *
 * Prevents dead-code elimination by accumulating a deterministic 64-bit checksum
 * and passing results through a volatile optimization barrier.
 */

/* Optimization barrier to prevent compiler dead-code elimination */
static volatile uint64_t g_sink = 0;

static inline void do_not_optimize(uint64_t val) {
    g_sink = val;
    __asm__ __volatile__("" : "+r"(val) : : "memory");
}

/* High-resolution monotonic timer */
static inline double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ========================================================================= */
/* Workload 1: Repeated Malloc and Free                                      */
/* ========================================================================= */
static uint64_t bench_malloc_free(size_t ops) {
    uint64_t checksum = 0;
    for (size_t i = 0; i < ops; i++) {
        size_t size = 32 + ((i * 17) % 512);
        char *p = (char *)malloc(size);
        if (p) {
            p[0] = (char)(i & 0xFF);
            p[size - 1] = (char)((i >> 8) & 0xFF);
            checksum += (uint8_t)p[0] + (uint8_t)p[size - 1];
            free(p);
        }
    }
    return checksum;
}

/* ========================================================================= */
/* Workload 2: Sequential Heap Reads and Writes                              */
/* ========================================================================= */
static uint64_t bench_seq_read_write(size_t ops) {
    size_t num_elements = 16384; /* 128 KB buffer */
    uint64_t *buf = (uint64_t *)malloc(num_elements * sizeof(uint64_t));
    if (!buf) return 0;

    for (size_t i = 0; i < num_elements; i++) {
        buf[i] = i ^ 0xA5A5A5A5A5A5A5A5ULL;
    }

    uint64_t checksum = 0;
    size_t idx = 0;
    for (size_t i = 0; i < ops; i++) {
        buf[idx] = (buf[idx] * 3ULL) + 1ULL;
        checksum += buf[idx];
        idx++;
        if (idx >= num_elements) {
            idx = 0;
        }
    }

    free(buf);
    return checksum;
}

/* ========================================================================= */
/* Workload 3: Random Heap Accesses Within Valid Bounds                      */
/* ========================================================================= */
static uint64_t bench_random_access(size_t ops) {
    size_t num_elements = 32768; /* 256 KB buffer */
    uint64_t *buf = (uint64_t *)malloc(num_elements * sizeof(uint64_t));
    if (!buf) return 0;

    for (size_t i = 0; i < num_elements; i++) {
        buf[i] = i ^ 0x5555555555555555ULL;
    }

    uint64_t checksum = 0;
    uint64_t lcg = 123456789ULL;
    for (size_t i = 0; i < ops; i++) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        size_t idx = (size_t)((lcg >> 32) % num_elements);
        buf[idx] ^= (lcg & 0xFF);
        checksum += buf[idx];
    }

    free(buf);
    return checksum;
}

/* ========================================================================= */
/* Workload 4: Multiple Co-existing Allocations of Variable Sizes             */
/* ========================================================================= */
#define MULTI_SLOTS 64

static uint64_t bench_multi_alloc(size_t ops) {
    void *slots[MULTI_SLOTS] = {0};
    size_t slot_sizes[MULTI_SLOTS] = {0};
    uint64_t checksum = 0;
    uint64_t lcg = 987654321ULL;

    for (size_t i = 0; i < ops; i++) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        size_t slot = (size_t)((lcg >> 32) % MULTI_SLOTS);

        if (slots[slot] != NULL) {
            uint8_t *p = (uint8_t *)slots[slot];
            size_t sz = slot_sizes[slot];
            p[0] = (uint8_t)(p[0] + 1);
            p[sz - 1] = (uint8_t)(p[sz - 1] + 2);
            checksum += p[0] + p[sz - 1];
            free(slots[slot]);
            slots[slot] = NULL;
        } else {
            size_t sz = 16 + ((lcg >> 16) % 2048);
            uint8_t *p = (uint8_t *)malloc(sz);
            if (p) {
                p[0] = (uint8_t)(i & 0xFF);
                p[sz - 1] = (uint8_t)((i >> 8) & 0xFF);
                slots[slot] = p;
                slot_sizes[slot] = sz;
                checksum += p[0] + p[sz - 1];
            }
        }
    }

    for (size_t s = 0; s < MULTI_SLOTS; s++) {
        if (slots[s] != NULL) {
            free(slots[s]);
            slots[s] = NULL;
        }
    }

    return checksum;
}

/* ========================================================================= */
/* Workload 5: Interprocedural Pointer Passing Across Function Calls         */
/* ========================================================================= */
__attribute__((noinline))
static uint64_t helper_write_read(uint32_t *arr, size_t count, uint32_t val) {
    for (size_t i = 0; i < count; i++) {
        arr[i] += val;
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += arr[i];
    }
    return sum;
}

__attribute__((noinline))
static uint64_t helper_forward(uint32_t *arr, size_t count, uint32_t val) {
    return helper_write_read(arr, count, val);
}

static uint64_t bench_interprocedural(size_t ops) {
    size_t count = 64;
    uint32_t *p = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!p) return 0;

    for (size_t i = 0; i < count; i++) {
        p[i] = (uint32_t)i;
    }

    uint64_t checksum = 0;
    for (size_t i = 0; i < ops; i++) {
        checksum += helper_forward(p, count, (uint32_t)(i & 0xFF));
    }

    free(p);
    return checksum;
}

/* ========================================================================= */
/* Workload 6: Multithreaded Concurrent Allocation and Access               */
/* ========================================================================= */
#define BENCH_NUM_THREADS 4

typedef struct {
    size_t ops_per_thread;
    uint64_t seed;
    uint64_t checksum;
} ThreadArg;

static void *worker_thread(void *arg) {
    ThreadArg *targ = (ThreadArg *)arg;
    size_t ops = targ->ops_per_thread;
    uint64_t sum = 0;

    for (size_t i = 0; i < ops; i++) {
        size_t sz = 32 + ((i * 13) % 256);
        char *p = (char *)malloc(sz);
        if (p) {
            p[0] = (char)(i & 0xFF);
            p[sz - 1] = (char)((i >> 8) & 0xFF);
            sum += (uint8_t)p[0] + (uint8_t)p[sz - 1];
            free(p);
        }
    }
    targ->checksum = sum;
    return NULL;
}

static uint64_t bench_multithreaded(size_t ops) {
    pthread_t threads[BENCH_NUM_THREADS];
    ThreadArg args[BENCH_NUM_THREADS];
    size_t ops_per_thread = ops / BENCH_NUM_THREADS;
    if (ops_per_thread == 0) ops_per_thread = 1;

    for (int t = 0; t < BENCH_NUM_THREADS; t++) {
        args[t].ops_per_thread = ops_per_thread;
        args[t].seed = 10007ULL * (t + 1);
        args[t].checksum = 0;
        pthread_create(&threads[t], NULL, worker_thread, &args[t]);
    }

    uint64_t total_checksum = 0;
    for (int t = 0; t < BENCH_NUM_THREADS; t++) {
        pthread_join(threads[t], NULL);
        total_checksum += args[t].checksum;
    }
    return total_checksum;
}

/* ========================================================================= */
/* Benchmark Dispatcher and Runner                                           */
/* ========================================================================= */
typedef uint64_t (*BenchmarkFn)(size_t ops);

typedef struct {
    const char *name;
    BenchmarkFn fn;
} BenchmarkEntry;

static const BenchmarkEntry BENCHMARKS[] = {
    {"malloc_free",       bench_malloc_free},
    {"seq_read_write",    bench_seq_read_write},
    {"random_access",     bench_random_access},
    {"multi_alloc",       bench_multi_alloc},
    {"interprocedural",   bench_interprocedural},
    {"multithreaded",     bench_multithreaded},
    {NULL, NULL}
};

static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static void run_single_benchmark(const BenchmarkEntry *bench, size_t ops, int warmup_runs, int bench_runs) {
    /* Perform warm-up runs */
    for (int w = 0; w < warmup_runs; w++) {
        uint64_t c = bench->fn(ops > 1000 ? 1000 : ops);
        do_not_optimize(c);
    }

    double *times = (double *)malloc(bench_runs * sizeof(double));
    if (!times) {
        fprintf(stderr, "Failed to allocate memory for timing measurements\n");
        return;
    }

    uint64_t last_checksum = 0;
    double total_time = 0.0;

    for (int r = 0; r < bench_runs; r++) {
        double t0 = get_time_sec();
        uint64_t chk = bench->fn(ops);
        double t1 = get_time_sec();

        do_not_optimize(chk);
        last_checksum = chk;
        times[r] = t1 - t0;
        total_time += times[r];
    }

    qsort(times, bench_runs, sizeof(double), compare_doubles);

    double min_sec = times[0];
    double median_sec = times[bench_runs / 2];
    double avg_sec = total_time / bench_runs;

    double ns_per_op = (median_sec * 1e9) / (double)ops;
    double ops_per_sec = (median_sec > 1e-12) ? ((double)ops / median_sec) : 0.0;

    /* Structured parseable output for runner scripts */
    printf("RESULT workload=%s ops=%zu runs=%d checksum=0x%016lx min_sec=%.9f median_sec=%.9f avg_sec=%.9f ns_per_op=%.2f ops_per_sec=%.2f\n",
           bench->name, ops, bench_runs, last_checksum, min_sec, median_sec, avg_sec, ns_per_op, ops_per_sec);

    free(times);
}

int main(int argc, char **argv) {
    const char *target_workload = "all";
    size_t ops = 100000;
    int warmup_runs = 2;
    int bench_runs = 5;

    if (argc >= 2) {
        target_workload = argv[1];
    }
    if (argc >= 3) {
        ops = (size_t)strtoull(argv[2], NULL, 10);
    }
    if (argc >= 4) {
        warmup_runs = atoi(argv[3]);
    }
    if (argc >= 5) {
        bench_runs = atoi(argv[4]);
    }

    for (int i = 0; BENCHMARKS[i].name != NULL; i++) {
        if (strcmp(target_workload, "all") == 0 || strcmp(target_workload, BENCHMARKS[i].name) == 0) {
            run_single_benchmark(&BENCHMARKS[i], ops, warmup_runs, bench_runs);
        }
    }

    return 0;
}

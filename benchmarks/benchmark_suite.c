#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/**
 * @file benchmark_suite.c
 * @brief MemoryGuard Performance Benchmark Workloads (Audited & Corrected).
 *
 * Implements 6 representative workloads to evaluate:
 *   1. Repeated malloc/free operations.
 *   2. Sequential heap reads and writes.
 *   3. Random heap accesses within valid bounds.
 *   4. Multiple co-existing allocations of variable sizes.
 *   5. Interprocedural pointer passing across function boundaries.
 *   6. Multithreaded concurrent allocation and access under lock contention.
 *
 * Methodology corrections applied:
 *   - Uses escape_ptr() assembly memory barriers on all allocations and buffers
 *     to prevent LLVM -O2 from eliminating heap allocations or memory accesses.
 *   - Records elapsed time in exact integer nanoseconds via clock_gettime(CLOCK_MONOTONIC).
 *   - Normalizes operation definitions so 1 op corresponds directly to the primary
 *     workload unit (e.g. 1 allocation cycle or 1 memory read-modify-write).
 *   - Automatically batches fast workloads so each timing sample runs for at least
 *     100 ms where practical, eliminating sub-millisecond measurement noise.
 *   - Detects and flags any sample below the recommended timing resolution.
 */

/* Optimization barriers to prevent dead-code elimination */
static inline void escape_ptr(void *p) {
    __asm__ __volatile__("" : "+r"(p) : : "memory");
}

static volatile uint64_t g_sink = 0;

static inline void do_not_optimize(uint64_t val) {
    g_sink = val;
    __asm__ __volatile__("" : "+r"(val) : : "memory");
}

/* High-resolution monotonic timer returning integer nanoseconds */
static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ========================================================================= */
/* Workload 1: Repeated Malloc and Free                                      */
/* Operation: 1 allocation cycle (1 malloc + 1 write + 1 read + 1 free)       */
/* ========================================================================= */
static uint64_t bench_malloc_free(size_t ops) {
    uint64_t checksum = 0;
    for (size_t i = 0; i < ops; i++) {
        size_t size = 32 + ((i * 17) % 512);
        char *p = (char *)malloc(size);
        if (p) {
            escape_ptr(p);
            p[0] = (char)(i & 0xFF);
            p[size - 1] = (char)((i >> 8) & 0xFF);
            checksum += (uint8_t)p[0] + (uint8_t)p[size - 1];
            escape_ptr(p);
            free(p);
        }
    }
    return checksum;
}

/* ========================================================================= */
/* Workload 2: Sequential Heap Reads and Writes                              */
/* Operation: 1 sequential read-modify-write (1 read + 1 write)              */
/* ========================================================================= */
static uint64_t bench_seq_read_write(size_t ops) {
    size_t num_elements = 16384; /* 128 KB buffer */
    uint64_t *buf = (uint64_t *)malloc(num_elements * sizeof(uint64_t));
    if (!buf) return 0;
    escape_ptr(buf);

    for (size_t i = 0; i < num_elements; i++) {
        buf[i] = i ^ 0xA5A5A5A5A5A5A5A5ULL;
    }
    escape_ptr(buf);

    uint64_t checksum = 0;
    size_t idx = 0;
    for (size_t i = 0; i < ops; i++) {
        uint64_t v = buf[idx];
        v = (v * 3ULL) + 1ULL;
        buf[idx] = v;
        checksum += v;
        idx++;
        if (idx >= num_elements) {
            idx = 0;
        }
    }

    escape_ptr(buf);
    free(buf);
    return checksum;
}

/* ========================================================================= */
/* Workload 3: Random Heap Accesses Within Valid Bounds                      */
/* Operation: 1 random read-modify-write (1 read + 1 write)                  */
/* ========================================================================= */
static uint64_t bench_random_access(size_t ops) {
    size_t num_elements = 32768; /* 256 KB buffer */
    uint64_t *buf = (uint64_t *)malloc(num_elements * sizeof(uint64_t));
    if (!buf) return 0;
    escape_ptr(buf);

    for (size_t i = 0; i < num_elements; i++) {
        buf[i] = i ^ 0x5555555555555555ULL;
    }
    escape_ptr(buf);

    uint64_t checksum = 0;
    uint64_t lcg = 123456789ULL;
    for (size_t i = 0; i < ops; i++) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        size_t idx = (size_t)((lcg >> 32) % num_elements);
        uint64_t v = buf[idx] ^ (lcg & 0xFF);
        buf[idx] = v;
        checksum += v;
    }

    escape_ptr(buf);
    free(buf);
    return checksum;
}

/* ========================================================================= */
/* Workload 4: Multiple Co-existing Allocations of Variable Sizes             */
/* Operation: 1 dynamic heap lifecycle transition (alloc+write or read+free)  */
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
            escape_ptr(p);
            size_t sz = slot_sizes[slot];
            p[0] = (uint8_t)(p[0] + 1);
            p[sz - 1] = (uint8_t)(p[sz - 1] + 2);
            checksum += p[0] + p[sz - 1];
            escape_ptr(p);
            free(slots[slot]);
            slots[slot] = NULL;
        } else {
            size_t sz = 16 + ((lcg >> 16) % 2048);
            uint8_t *p = (uint8_t *)malloc(sz);
            if (p) {
                escape_ptr(p);
                p[0] = (uint8_t)(i & 0xFF);
                p[sz - 1] = (uint8_t)((i >> 8) & 0xFF);
                slots[slot] = p;
                slot_sizes[slot] = sz;
                checksum += p[0] + p[sz - 1];
                escape_ptr(p);
            }
        }
    }

    for (size_t s = 0; s < MULTI_SLOTS; s++) {
        if (slots[s] != NULL) {
            escape_ptr(slots[s]);
            free(slots[s]);
            slots[s] = NULL;
        }
    }

    return checksum;
}

/* ========================================================================= */
/* Workload 5: Interprocedural Pointer Passing Across Function Calls         */
/* Operation: 1 forwarded read-modify-write access (1 read + 1 write)        */
/* ========================================================================= */
__attribute__((noinline))
static uint64_t helper_write_read(uint32_t *arr, size_t idx, uint32_t val) {
    escape_ptr(arr);
    arr[idx] = arr[idx] * 3U + val;
    uint32_t res = arr[idx];
    escape_ptr(arr);
    return (uint64_t)res;
}

__attribute__((noinline))
static uint64_t helper_forward(uint32_t *arr, size_t idx, uint32_t val) {
    return helper_write_read(arr, idx, val);
}

static uint64_t bench_interprocedural(size_t ops) {
    size_t count = 4096;
    uint32_t *p = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!p) return 0;
    escape_ptr(p);

    for (size_t i = 0; i < count; i++) {
        p[i] = (uint32_t)(i ^ 0x3C3C3C3CU);
    }
    escape_ptr(p);

    uint64_t checksum = 0;
    size_t idx = 0;
    for (size_t i = 0; i < ops; i++) {
        checksum += helper_forward(p, idx, (uint32_t)(i & 0xFF));
        idx++;
        if (idx >= count) {
            idx = 0;
        }
    }

    escape_ptr(p);
    free(p);
    return checksum;
}

/* ========================================================================= */
/* Workload 6: Multithreaded Concurrent Allocation and Access               */
/* Operation: 1 concurrent allocation cycle across 4 threads                 */
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
            escape_ptr(p);
            p[0] = (char)(i & 0xFF);
            p[sz - 1] = (char)((i >> 8) & 0xFF);
            sum += (uint8_t)p[0] + (uint8_t)p[sz - 1];
            escape_ptr(p);
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
/* Benchmark Dispatcher and Runner Harness                                   */
/* ========================================================================= */
typedef uint64_t (*BenchmarkFn)(size_t ops);

typedef struct {
    const char *name;
    const char *op_description;
    BenchmarkFn fn;
} BenchmarkEntry;

static const BenchmarkEntry BENCHMARKS[] = {
    {"malloc_free",       "allocation_lifecycle",  bench_malloc_free},
    {"seq_read_write",    "sequential_rmw",        bench_seq_read_write},
    {"random_access",     "random_rmw",            bench_random_access},
    {"multi_alloc",       "dynamic_lifecycle",     bench_multi_alloc},
    {"interprocedural",   "forwarded_rmw",         bench_interprocedural},
    {"multithreaded",     "concurrent_lifecycle",  bench_multithreaded},
    {NULL, NULL, NULL}
};

static int compare_uint64(const void *a, const void *b) {
    uint64_t ua = *(const uint64_t *)a;
    uint64_t ub = *(const uint64_t *)b;
    if (ua < ub) return -1;
    if (ua > ub) return 1;
    return 0;
}

static void run_single_benchmark(const BenchmarkEntry *bench, size_t ops,
                                 int warmup_runs, int bench_runs,
                                 int target_duration_ms, size_t forced_batch) {
    /* 1. Warm-up and batch size determination */
    uint64_t single_run_ns = 1;
    for (int w = 0; w < warmup_runs; w++) {
        uint64_t t0 = get_time_ns();
        uint64_t c = bench->fn(ops);
        uint64_t t1 = get_time_ns();
        do_not_optimize(c);
        if (t1 > t0) {
            single_run_ns = t1 - t0;
        }
    }

    size_t batch_count = 1;
    if (forced_batch > 0) {
        batch_count = forced_batch;
    } else if (target_duration_ms > 0) {
        uint64_t target_ns = (uint64_t)target_duration_ms * 1000000ULL;
        if (single_run_ns < target_ns) {
            batch_count = (size_t)(target_ns / single_run_ns) + 1;
            if (batch_count > 5000) {
                batch_count = 5000;
            }
        }
    }

    size_t effective_ops = ops * batch_count;

    uint64_t *times_ns = (uint64_t *)malloc(bench_runs * sizeof(uint64_t));
    if (!times_ns) {
        fprintf(stderr, "Failed to allocate memory for timing measurements\n");
        return;
    }

    uint64_t last_checksum = 0;
    uint64_t total_ns = 0;

    for (int r = 0; r < bench_runs; r++) {
        uint64_t t0 = get_time_ns();
        uint64_t run_chk = 0;
        for (size_t b = 0; b < batch_count; b++) {
            run_chk += bench->fn(ops);
        }
        uint64_t t1 = get_time_ns();

        do_not_optimize(run_chk);
        last_checksum = run_chk;
        uint64_t elapsed = (t1 > t0) ? (t1 - t0) : 1;
        times_ns[r] = elapsed;
        total_ns += elapsed;
    }

    qsort(times_ns, bench_runs, sizeof(uint64_t), compare_uint64);

    uint64_t min_ns = times_ns[0];
    uint64_t median_ns = times_ns[bench_runs / 2];
    uint64_t avg_ns = total_ns / bench_runs;

    /* Sample duration in milliseconds */
    double sample_duration_ms = (double)median_ns / 1e6;

    /* Accurate per-operation cost and throughput */
    double ns_per_op = (double)median_ns / (double)effective_ops;
    double ops_per_sec = (median_ns > 0) ? ((double)effective_ops * 1e9 / (double)median_ns) : 0.0;

    /* Flag whether sample duration satisfies minimum recommended resolution (>= 50 ms) */
    int is_reliable = (sample_duration_ms >= 50.0) ? 1 : 0;

    /* Structured parseable output */
    printf("RESULT workload=%s op_type=%s ops=%zu batch_count=%zu effective_ops=%zu runs=%d checksum=0x%016lx min_ns=%lu median_ns=%lu avg_ns=%lu sample_duration_ms=%.3f ns_per_op=%.2f ops_per_sec=%.2f reliable=%d\n",
           bench->name, bench->op_description, ops, batch_count, effective_ops, bench_runs,
           last_checksum, min_ns, median_ns, avg_ns, sample_duration_ms, ns_per_op, ops_per_sec, is_reliable);

    free(times_ns);
}

int main(int argc, char **argv) {
    const char *target_workload = "all";
    size_t ops = 100000;
    int warmup_runs = 2;
    int bench_runs = 5;
    int target_duration_ms = 100; /* Default target: at least 100 ms per sample */
    size_t forced_batch = 0;

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
    if (argc >= 6) {
        target_duration_ms = atoi(argv[5]);
    }
    if (argc >= 7) {
        forced_batch = (size_t)strtoull(argv[6], NULL, 10);
    }

    for (int i = 0; BENCHMARKS[i].name != NULL; i++) {
        if (strcmp(target_workload, "all") == 0 || strcmp(target_workload, BENCHMARKS[i].name) == 0) {
            run_single_benchmark(&BENCHMARKS[i], ops, warmup_runs, bench_runs, target_duration_ms, forced_batch);
        }
    }

    return 0;
}

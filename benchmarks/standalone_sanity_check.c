#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

static inline void escape_ptr(void *p) {
    __asm__ __volatile__("" : "+r"(p) : : "memory");
}

static inline void do_not_optimize(uint64_t val) {
    __asm__ __volatile__("" : "+r"(val) : : "memory");
}

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void) {
    printf("====================================================\n");
    printf("   Standalone Hardware & libc Sanity Check\n");
    printf("====================================================\n");

    const size_t N = 1000000;

    /* 1. Raw libc malloc + free */
    {
        uint64_t t0 = get_time_ns();
        uint64_t chk = 0;
        for (size_t i = 0; i < N; i++) {
            size_t sz = 64;
            char *p = (char *)malloc(sz);
            escape_ptr(p);
            p[0] = (char)(i & 0xFF);
            chk += (uint8_t)p[0];
            escape_ptr(p);
            free(p);
        }
        uint64_t t1 = get_time_ns();
        do_not_optimize(chk);
        double elapsed_ms = (double)(t1 - t0) / 1e6;
        double ns_per_op = (double)(t1 - t0) / (double)N;
        printf("1. libc malloc(64)+free (%zu ops): %7.2f ms (%6.2f ns/op)\n", N, elapsed_ms, ns_per_op);
    }

    /* 2. Sequential heap read-modify-write */
    {
        size_t buf_len = 16384;
        uint64_t *buf = (uint64_t *)malloc(buf_len * sizeof(uint64_t));
        for (size_t i = 0; i < buf_len; i++) buf[i] = i;

        uint64_t t0 = get_time_ns();
        uint64_t chk = 0;
        size_t idx = 0;
        for (size_t i = 0; i < N; i++) {
            buf[idx] = buf[idx] * 3ULL + 1ULL;
            chk += buf[idx];
            idx++;
            if (idx >= buf_len) idx = 0;
        }
        uint64_t t1 = get_time_ns();
        do_not_optimize(chk);
        free(buf);
        double elapsed_ms = (double)(t1 - t0) / 1e6;
        double ns_per_op = (double)(t1 - t0) / (double)N;
        printf("2. Sequential heap RMW  (%zu ops): %7.2f ms (%6.2f ns/op)\n", N, elapsed_ms, ns_per_op);
    }

    /* 3. Random heap read-modify-write */
    {
        size_t buf_len = 32768;
        uint64_t *buf = (uint64_t *)malloc(buf_len * sizeof(uint64_t));
        for (size_t i = 0; i < buf_len; i++) buf[i] = i ^ 0x5555555555555555ULL;

        uint64_t t0 = get_time_ns();
        uint64_t chk = 0;
        uint64_t lcg = 123456789ULL;
        for (size_t i = 0; i < N; i++) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            size_t idx = (size_t)((lcg >> 32) % buf_len);
            buf[idx] ^= (lcg & 0xFF);
            chk += buf[idx];
        }
        uint64_t t1 = get_time_ns();
        do_not_optimize(chk);
        free(buf);
        double elapsed_ms = (double)(t1 - t0) / 1e6;
        double ns_per_op = (double)(t1 - t0) / (double)N;
        printf("3. Random heap RMW      (%zu ops): %7.2f ms (%6.2f ns/op)\n", N, elapsed_ms, ns_per_op);
    }

    printf("====================================================\n");
    return 0;
}

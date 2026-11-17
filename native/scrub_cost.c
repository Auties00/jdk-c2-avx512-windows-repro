/*
 * scrub_cost.c: what would it cost to overwrite, after every call, the one quadword that Windows Server 2025
 * can take for the XCOMP_BV of a stale XSAVE header (see xstate_stale_header.c and xstate_detect.c)?
 *
 * A proxy measurement in C, not a JVM benchmark. Three loops call a function through a pointer:
 *   base    the call only
 *   scrub   the call, then: lea rax, [rsp - L]; and rax, -64; mov qword ptr [rax + 8], 0
 *   scrub4  the call, then four 8-byte stores of zero at rsp - L + 8 - {0, 16, 32, 48}
 * with two callees: an empty leaf, and one that does some integer work (eight xorshift rounds). Each variant is timed 21
 * times, interleaved; the output is the mean and the 95% confidence interval of the mean, in ns per call.
 *
 * usage: scrub_cost [L]      (L defaults to 1920, the AVX-512 context; 320 for AVX2-only hosts)
 * Build: clang -O2 -o scrub_cost.exe scrub_cost.c
 * MIT License.
 */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CALLS 50000000
#define REPEATS 21

typedef uint64_t (*callee_t)(uint64_t);

__attribute__((noinline)) static uint64_t leaf(uint64_t x) { return x + 1; }

__attribute__((noinline)) static uint64_t work(uint64_t x) {
    for (int i = 0; i < 8; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
    return x;
}

static int64_t L = 1920;

__attribute__((noinline)) static uint64_t run_base(callee_t f, long n) {
    uint64_t x = 1;
    for (long i = 0; i < n; i++) {
        x = f(x);
        __asm__ volatile("" : : : "memory");
    }
    return x;
}

__attribute__((noinline)) static uint64_t run_scrub(callee_t f, long n) {
    uint64_t x = 1;
    int64_t l = L;
    for (long i = 0; i < n; i++) {
        x = f(x);
        __asm__ volatile("movq %%rsp, %%rax\n\t" "subq %0, %%rax\n\t" "andq $-64, %%rax\n\t" "movq $0, 8(%%rax)"
                         : : "r"(l) : "rax", "memory", "cc");
    }
    return x;
}

__attribute__((noinline)) static uint64_t run_scrub4(callee_t f, long n) {
    uint64_t x = 1;
    int64_t l = L;
    for (long i = 0; i < n; i++) {
        x = f(x);
        __asm__ volatile("movq %%rsp, %%rax\n\t" "subq %0, %%rax\n\t"
                         "movq $0, 8(%%rax)\n\t" "movq $0, -8(%%rax)\n\t" "movq $0, -24(%%rax)\n\t" "movq $0, -40(%%rax)"
                         : : "r"(l) : "rax", "memory", "cc");
    }
    return x;
}

static double now_ns(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (f.QuadPart == 0) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double) t.QuadPart * 1e9 / (double) f.QuadPart;
}

static void stats(const double *v, int n, double *mean, double *ci) {
    double m = 0, s = 0;
    for (int i = 0; i < n; i++) m += v[i];
    m /= n;
    for (int i = 0; i < n; i++) s += (v[i] - m) * (v[i] - m);
    *mean = m;
    *ci = 2.086 * sqrt(s / (n - 1)) / sqrt((double) n);     /* t(0.975, 20) */
}

int main(int argc, char **argv) {
    if (argc > 1) L = atoll(argv[1]);
    volatile unsigned char commit[16384];                     /* the stack the scrub writes to */
    for (int i = 0; i < (int) sizeof(commit); i += 512) commit[i] = 0;
    static const struct { const char *name; callee_t f; long n; } callees[] = {
        { "empty leaf", leaf, CALLS }, { "some integer work", work, CALLS / 10 } };
    uint64_t sink = 0;
    printf("L = %lld\n", (long long) L);
    for (int c = 0; c < 2; c++) {
        double t[3][REPEATS];
        for (int r = -1; r < REPEATS; r++) {                  /* r = -1: warm-up */
            for (int v = 0; v < 3; v++) {
                double a = now_ns();
                sink += v == 0 ? run_base(callees[c].f, callees[c].n)
                      : v == 1 ? run_scrub(callees[c].f, callees[c].n) : run_scrub4(callees[c].f, callees[c].n);
                double b = now_ns();
                if (r >= 0) t[v][r] = (b - a) / (double) callees[c].n;
            }
        }
        double m[3], ci[3];
        for (int v = 0; v < 3; v++) stats(t[v], REPEATS, &m[v], &ci[v]);
        printf("%-14s base %.3f +-%.3f ns/call | scrub %.3f +-%.3f (%+.1f%%, %+.3f ns) | scrub4 %.3f +-%.3f (%+.1f%%, %+.3f ns)\n",
               callees[c].name, m[0], ci[0], m[1], ci[1], (m[1] / m[0] - 1) * 100, m[1] - m[0],
               m[2], ci[2], (m[2] / m[0] - 1) * 100, m[2] - m[0]);
    }
    return (int) (sink & 0);
}

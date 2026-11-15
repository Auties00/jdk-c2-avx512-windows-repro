/*
 * xstate_probe: checks whether the AVX-512 register state of a thread survives a
 * user-mode access-violation trap that a handler resumes, with no JVM involved.
 *
 * Each iteration loads a distinct, non-zero pattern into zmm0-zmm31 and k1-k7,
 * executes `test ecx, [rbx]` against a no-access page (the same kind of trapping
 * load a JVM safepoint poll uses), lets a vectored exception handler (Windows) or
 * a SIGSEGV handler (Linux) skip the 2-byte instruction, then stores every register
 * back and compares. Mismatches are counted per XSAVE component:
 *
 *   xmm0-15 low 128 bits (SSE), ymm0-15 bits 128-255 (AVX), zmm0-15 bits 256-511
 *   (ZMM_Hi256), zmm16-31 (Hi16_ZMM), k1-k7 (opmask).
 *
 * Usage: xstate_probe <fault|nofault> <seconds> <threads>
 *   fault    the load traps every iteration (the case under test)
 *   nofault  the page is readable (control: only interrupts and preemption)
 * Exit code: 0 no corruption, 3 corruption seen, 2 neither AVX-512 nor AVX2.
 * On a host without AVX-512 it falls back to ymm0-15 (SSE and AVX components only).
 *
 * Build: clang -O2 -o xstate_probe xstate_probe.c   (Windows or Linux; gcc works on Linux)
 * The AVX-512 and AVX2 code only runs after a CPUID/XGETBV check.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include <cpuid.h>

#define NREG 32
#define NQ 8
#define NK 7

enum { C_SSE, C_AVX, C_ZMM_HI256, C_HI16_ZMM, C_OPMASK, C_COUNT };
static const char *component_names[C_COUNT] = {
    "xmm0-15[0:127]", "ymm0-15[128:255]", "zmm0-15[256:511]", "zmm16-31", "k1-k7"};

static volatile int fault_mode;
static int self_test; /* XSTATE_PROBE_SELF_TEST=1: the handler flips a bit of xmm0 in the saved context */
static volatile long long deadline_ns;
static unsigned char *trap_page;

typedef struct {
    int id;
    long long iterations;
    long long traps;
    long long bad[C_COUNT];
    long long bad_zero[C_COUNT];
    int reg_hits[NREG];
    char first[256];
} worker_t;

static _Thread_local long long thread_traps;

static long long now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (long long) ((double) c.QuadPart * 1e9 / (double) f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

static int avx2_usable(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 27))) return 0;
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 0x6) != 0x6) return 0;
    __cpuid_count(7, 0, a, b, c, d);
    return (b & (1u << 5)) != 0;
}

static int use_avx512;

static int avx512_usable(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 27))) return 0; /* OSXSAVE */
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 0xE6) != 0xE6) return 0; /* SSE, AVX, opmask, ZMM_Hi256, Hi16_ZMM enabled */
    __cpuid_count(7, 0, a, b, c, d);
    return (b & (1u << 16)) != 0; /* AVX512F */
}

#ifdef _WIN32
static LONG CALLBACK on_exception(PEXCEPTION_POINTERS info) {
    PEXCEPTION_RECORD r = info->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2
            && (unsigned char *) r->ExceptionInformation[1] == trap_page) {
        info->ContextRecord->Rip += 2; /* skip `test ecx, [rbx]` (85 0B) */
        thread_traps++;
        if (self_test) info->ContextRecord->Xmm0.Low ^= 1; /* proves detection works */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void on_segv(int sig, siginfo_t *si, void *uc_) {
    (void) sig;
    ucontext_t *uc = uc_;
    if ((unsigned char *) si->si_addr == trap_page) {
        uc->uc_mcontext.gregs[REG_RIP] += 2;
        thread_traps++;
        if (self_test) uc->uc_mcontext.fpregs->_xmm[0].element[0] ^= 1; /* proves detection works */
        return;
    }
    signal(SIGSEGV, SIG_DFL);
}
#endif

/* Loads zmm0-31 and k1-k7 from `in`, executes the (possibly trapping) load, stores all back to `out`. */
__attribute__((noinline, target("avx512f")))
static void round_trip(const uint64_t *in, uint64_t *out, const uint16_t *kin, uint16_t *kout, unsigned char *page) {
    __asm__ volatile(
        "vmovdqu64 0*64(%%rax), %%zmm0\n\t"  "vmovdqu64 1*64(%%rax), %%zmm1\n\t"
        "vmovdqu64 2*64(%%rax), %%zmm2\n\t"  "vmovdqu64 3*64(%%rax), %%zmm3\n\t"
        "vmovdqu64 4*64(%%rax), %%zmm4\n\t"  "vmovdqu64 5*64(%%rax), %%zmm5\n\t"
        "vmovdqu64 6*64(%%rax), %%zmm6\n\t"  "vmovdqu64 7*64(%%rax), %%zmm7\n\t"
        "vmovdqu64 8*64(%%rax), %%zmm8\n\t"  "vmovdqu64 9*64(%%rax), %%zmm9\n\t"
        "vmovdqu64 10*64(%%rax), %%zmm10\n\t" "vmovdqu64 11*64(%%rax), %%zmm11\n\t"
        "vmovdqu64 12*64(%%rax), %%zmm12\n\t" "vmovdqu64 13*64(%%rax), %%zmm13\n\t"
        "vmovdqu64 14*64(%%rax), %%zmm14\n\t" "vmovdqu64 15*64(%%rax), %%zmm15\n\t"
        "vmovdqu64 16*64(%%rax), %%zmm16\n\t" "vmovdqu64 17*64(%%rax), %%zmm17\n\t"
        "vmovdqu64 18*64(%%rax), %%zmm18\n\t" "vmovdqu64 19*64(%%rax), %%zmm19\n\t"
        "vmovdqu64 20*64(%%rax), %%zmm20\n\t" "vmovdqu64 21*64(%%rax), %%zmm21\n\t"
        "vmovdqu64 22*64(%%rax), %%zmm22\n\t" "vmovdqu64 23*64(%%rax), %%zmm23\n\t"
        "vmovdqu64 24*64(%%rax), %%zmm24\n\t" "vmovdqu64 25*64(%%rax), %%zmm25\n\t"
        "vmovdqu64 26*64(%%rax), %%zmm26\n\t" "vmovdqu64 27*64(%%rax), %%zmm27\n\t"
        "vmovdqu64 28*64(%%rax), %%zmm28\n\t" "vmovdqu64 29*64(%%rax), %%zmm29\n\t"
        "vmovdqu64 30*64(%%rax), %%zmm30\n\t" "vmovdqu64 31*64(%%rax), %%zmm31\n\t"
        "kmovw 0(%%rsi), %%k1\n\t"  "kmovw 2(%%rsi), %%k2\n\t"  "kmovw 4(%%rsi), %%k3\n\t"
        "kmovw 6(%%rsi), %%k4\n\t"  "kmovw 8(%%rsi), %%k5\n\t"  "kmovw 10(%%rsi), %%k6\n\t"
        "kmovw 12(%%rsi), %%k7\n\t"
        ".byte 0x85, 0x0b\n\t" /* test ecx, [rbx]: traps when the page is no-access */
        "vmovdqu64 %%zmm0, 0*64(%%rdx)\n\t"  "vmovdqu64 %%zmm1, 1*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm2, 2*64(%%rdx)\n\t"  "vmovdqu64 %%zmm3, 3*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm4, 4*64(%%rdx)\n\t"  "vmovdqu64 %%zmm5, 5*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm6, 6*64(%%rdx)\n\t"  "vmovdqu64 %%zmm7, 7*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm8, 8*64(%%rdx)\n\t"  "vmovdqu64 %%zmm9, 9*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm10, 10*64(%%rdx)\n\t" "vmovdqu64 %%zmm11, 11*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm12, 12*64(%%rdx)\n\t" "vmovdqu64 %%zmm13, 13*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm14, 14*64(%%rdx)\n\t" "vmovdqu64 %%zmm15, 15*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm16, 16*64(%%rdx)\n\t" "vmovdqu64 %%zmm17, 17*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm18, 18*64(%%rdx)\n\t" "vmovdqu64 %%zmm19, 19*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm20, 20*64(%%rdx)\n\t" "vmovdqu64 %%zmm21, 21*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm22, 22*64(%%rdx)\n\t" "vmovdqu64 %%zmm23, 23*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm24, 24*64(%%rdx)\n\t" "vmovdqu64 %%zmm25, 25*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm26, 26*64(%%rdx)\n\t" "vmovdqu64 %%zmm27, 27*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm28, 28*64(%%rdx)\n\t" "vmovdqu64 %%zmm29, 29*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm30, 30*64(%%rdx)\n\t" "vmovdqu64 %%zmm31, 31*64(%%rdx)\n\t"
        "kmovw %%k1, 0(%%rdi)\n\t"  "kmovw %%k2, 2(%%rdi)\n\t"  "kmovw %%k3, 4(%%rdi)\n\t"
        "kmovw %%k4, 6(%%rdi)\n\t"  "kmovw %%k5, 8(%%rdi)\n\t"  "kmovw %%k6, 10(%%rdi)\n\t"
        "kmovw %%k7, 12(%%rdi)\n\t"
        :
        : "a"(in), "d"(out), "b"(page), "S"(kin), "D"(kout), "c"(0)
        : "memory", "cc",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
          "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21", "xmm22", "xmm23",
          "xmm24", "xmm25", "xmm26", "xmm27", "xmm28", "xmm29", "xmm30", "xmm31",
          "k1", "k2", "k3", "k4", "k5", "k6", "k7");
}


/* AVX2-only hosts: ymm0-15 only. */
__attribute__((noinline, target("avx2")))
static void round_trip_avx2(const uint64_t *in, uint64_t *out, const uint16_t *kin, uint16_t *kout, unsigned char *page) {
    (void) kin; (void) kout;
    __asm__ volatile(
        "vmovdqu 0*64(%%rax), %%ymm0\n\t"
        "vmovdqu 1*64(%%rax), %%ymm1\n\t"
        "vmovdqu 2*64(%%rax), %%ymm2\n\t"
        "vmovdqu 3*64(%%rax), %%ymm3\n\t"
        "vmovdqu 4*64(%%rax), %%ymm4\n\t"
        "vmovdqu 5*64(%%rax), %%ymm5\n\t"
        "vmovdqu 6*64(%%rax), %%ymm6\n\t"
        "vmovdqu 7*64(%%rax), %%ymm7\n\t"
        "vmovdqu 8*64(%%rax), %%ymm8\n\t"
        "vmovdqu 9*64(%%rax), %%ymm9\n\t"
        "vmovdqu 10*64(%%rax), %%ymm10\n\t"
        "vmovdqu 11*64(%%rax), %%ymm11\n\t"
        "vmovdqu 12*64(%%rax), %%ymm12\n\t"
        "vmovdqu 13*64(%%rax), %%ymm13\n\t"
        "vmovdqu 14*64(%%rax), %%ymm14\n\t"
        "vmovdqu 15*64(%%rax), %%ymm15\n\t"
        ".byte 0x85, 0x0b\n\t"
        "vmovdqu %%ymm0, 0*64(%%rdx)\n\t"
        "vmovdqu %%ymm1, 1*64(%%rdx)\n\t"
        "vmovdqu %%ymm2, 2*64(%%rdx)\n\t"
        "vmovdqu %%ymm3, 3*64(%%rdx)\n\t"
        "vmovdqu %%ymm4, 4*64(%%rdx)\n\t"
        "vmovdqu %%ymm5, 5*64(%%rdx)\n\t"
        "vmovdqu %%ymm6, 6*64(%%rdx)\n\t"
        "vmovdqu %%ymm7, 7*64(%%rdx)\n\t"
        "vmovdqu %%ymm8, 8*64(%%rdx)\n\t"
        "vmovdqu %%ymm9, 9*64(%%rdx)\n\t"
        "vmovdqu %%ymm10, 10*64(%%rdx)\n\t"
        "vmovdqu %%ymm11, 11*64(%%rdx)\n\t"
        "vmovdqu %%ymm12, 12*64(%%rdx)\n\t"
        "vmovdqu %%ymm13, 13*64(%%rdx)\n\t"
        "vmovdqu %%ymm14, 14*64(%%rdx)\n\t"
        "vmovdqu %%ymm15, 15*64(%%rdx)\n\t"
        :
        : "a"(in), "d"(out), "b"(page), "c"(0)
        : "memory", "cc",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

static int component_of(int reg, int q) {
    if (reg >= 16) return C_HI16_ZMM;
    if (q < 2) return C_SSE;
    if (q < 4) return C_AVX;
    return C_ZMM_HI256;
}

#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID arg)
#else
static void *worker_main(void *arg)
#endif
{
    worker_t *w = arg;
    uint64_t in[NREG * NQ], out[NREG * NQ];
    uint16_t kin[NK], kout[NK];
    uint64_t seed = 0x9E3779B97F4A7C15ULL * (uint64_t) (w->id + 1);
    while (now_ns() < deadline_ns) {
        for (int chunk = 0; chunk < 1024; chunk++) {
            for (int i = 0; i < NREG * NQ; i++) {
                seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
                in[i] = seed | 1; /* never zero */
            }
            for (int k = 0; k < NK; k++) kin[k] = (uint16_t) ((seed >> (8 * k)) | 0x0101);
            memset(out, 0xEE, sizeof out);
            memset(kout, 0xEE, sizeof kout);
            if (use_avx512) {
                round_trip(in, out, kin, kout, trap_page);
            } else {
                round_trip_avx2(in, out, kin, kout, trap_page);
                for (int r = 0; r < NREG; r++)
                    for (int q = 0; q < NQ; q++)
                        if (r >= 16 || q >= 4) out[r * NQ + q] = in[r * NQ + q];
                memcpy(kout, kin, sizeof kout);
            }
            w->iterations++;
            int seen[C_COUNT] = {0}, seen_zero[C_COUNT] = {0};
            for (int r = 0; r < NREG; r++) {
                for (int q = 0; q < NQ; q++) {
                    uint64_t e = in[r * NQ + q], a = out[r * NQ + q];
                    if (e != a) {
                        int c = component_of(r, q);
                        seen[c] = 1;
                        if (a == 0) seen_zero[c] = 1;
                        w->reg_hits[r]++;
                        if (!w->first[0]) {
                            snprintf(w->first, sizeof w->first,
                                     "iteration %lld: zmm%d qword %d expected %016llx got %016llx",
                                     w->iterations, r, q, (unsigned long long) e, (unsigned long long) a);
                        }
                    }
                }
            }
            for (int k = 0; k < NK; k++) {
                if (kin[k] != kout[k]) {
                    seen[C_OPMASK] = 1;
                    if (kout[k] == 0) seen_zero[C_OPMASK] = 1;
                }
            }
            for (int c = 0; c < C_COUNT; c++) {
                w->bad[c] += seen[c];
                w->bad_zero[c] += seen_zero[c];
            }
        }
    }
    w->traps = thread_traps;
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "fault";
    int seconds = argc > 2 ? atoi(argv[2]) : 10;
    int threads = argc > 3 ? atoi(argv[3]) : 4;
    if (strcmp(mode, "fault") && strcmp(mode, "nofault")) {
        fprintf(stderr, "mode: fault or nofault\n");
        return 1;
    }
    use_avx512 = avx512_usable();
    if (!use_avx512 && !avx2_usable()) {
        printf("xstate_probe: neither AVX-512 nor AVX2 available, skipped\n");
        return 2;
    }
    fault_mode = !strcmp(mode, "fault");
    self_test = getenv("XSTATE_PROBE_SELF_TEST") != NULL;
#ifdef _WIN32
    trap_page = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, fault_mode ? PAGE_NOACCESS : PAGE_READONLY);
    AddVectoredExceptionHandler(1, on_exception);
#else
    trap_page = mmap(NULL, 4096, fault_mode ? PROT_NONE : PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
#endif
    deadline_ns = now_ns() + seconds * 1000000000LL;
    worker_t *ws = calloc(threads, sizeof *ws);
#ifdef _WIN32
    HANDLE *hs = calloc(threads, sizeof *hs);
    for (int t = 0; t < threads; t++) {
        ws[t].id = t;
        hs[t] = CreateThread(NULL, 0, worker_main, &ws[t], 0, NULL);
    }
    WaitForMultipleObjects(threads, hs, TRUE, INFINITE);
#else
    pthread_t *hs = calloc(threads, sizeof *hs);
    for (int t = 0; t < threads; t++) {
        ws[t].id = t;
        pthread_create(&hs[t], NULL, worker_main, &ws[t]);
    }
    for (int t = 0; t < threads; t++) pthread_join(hs[t], NULL);
#endif
    long long iterations = 0, traps = 0, bad[C_COUNT] = {0}, bad_zero[C_COUNT] = {0}, total_bad = 0;
    int reg_hits[NREG] = {0};
    for (int t = 0; t < threads; t++) {
        iterations += ws[t].iterations;
        traps += ws[t].traps;
        for (int c = 0; c < C_COUNT; c++) {
            bad[c] += ws[t].bad[c];
            bad_zero[c] += ws[t].bad_zero[c];
        }
        for (int r = 0; r < NREG; r++) reg_hits[r] += ws[t].reg_hits[r];
    }
    printf("xstate_probe isa=%s mode=%s threads=%d seconds=%d iterations=%lld traps=%lld\n", use_avx512 ? "avx512" : "avx2", mode, threads, seconds, iterations, traps);
    for (int c = 0; c < C_COUNT; c++) {
        printf("  %-18s corrupted iterations %lld (read back as zero: %lld)\n", component_names[c], bad[c], bad_zero[c]);
        total_bad += bad[c];
    }
    printf("  registers hit:");
    for (int r = 0; r < NREG; r++) if (reg_hits[r]) printf(" zmm%d=%d", r, reg_hits[r]);
    printf("\n");
    for (int t = 0; t < threads; t++) if (ws[t].first[0]) printf("  thread %d first: %s\n", t, ws[t].first);
    printf("RESULT %s\n", total_bad ? "CORRUPTED" : "clean");
    return total_bad ? 3 : 0;
}

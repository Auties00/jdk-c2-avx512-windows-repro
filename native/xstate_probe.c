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
 * Usage: xstate_probe <fault|seh|nofault> <seconds> <threads>
 *   fault    the load traps every iteration; a vectored exception handler resumes it
 *   seh      Windows: the trap is in generated code registered with RtlAddFunctionTable and a
 *            language handler resumes it, which is how HotSpot handles a safepoint poll on x64
 *            (Linux: same as fault)
 *   nofault  the page is readable (control: only interrupts and preemption)
 * Environment: XSTATE_PROBE_CLOBBER=1 makes the handler zero the volatile vector registers before
 * resuming; XSTATE_PROBE_HANDLER_YIELD=1 makes it yield the CPU; XSTATE_PROBE_REDIRECT=1 makes it resume
 * at another instruction (a `ret`), as a JVM resumes in its safepoint handler; XSTATE_PROBE_SELF_TEST=1 flips a bit of xmm0 in the saved context (must report corruption).
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
#include <sched.h>
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
/* XSTATE_PROBE_CLOBBER=1: the handler zeroes the vector registers the ABI makes volatile (ymm and zmm upper
 * halves, zmm16-31, k1-k7) before resuming, as handler code using AVX-512 may. The OS must restore them
 * from the saved context on resume; corruption in this mode means it does not. */
static int clobber;
/* XSTATE_PROBE_HANDLER_YIELD=1: the handler gives up its time slice (SwitchToThread / sched_yield), so the
 * thread is usually switched out while the exception is being dispatched, as a JVM thread may be. */
static int handler_yield;
/* XSTATE_PROBE_REDIRECT=1: the handler resumes at a different instruction (a `ret` at thunk+8) instead of
 * the next one, as a JVM resumes a safepoint poll trap in its safepoint handler blob. */
static int redirect;
static int sparse;
/* XSTATE_PROBE_NOISE=1: a thread keeps changing the address space (commit, protect, release: TLB shootdowns)
 * and calls FlushProcessWriteBuffers (IPIs to every processor running a thread of the process), as a JVM does
 * while it compiles, commits heap and metaspace and runs safepoints. */
static int noise;
static long long spin_iterations;
static volatile int noise_stop;
static int sparse_vzeroupper;
static int use_avx512;
static volatile long long first_flags = -1;
static volatile long long first_xstate_mask = -1;

__attribute__((noinline, target("avx512f")))
static void clobber_avx512(void) {
    __asm__ volatile(
        "vzeroall\n\t"
        "vpxord %%zmm16, %%zmm16, %%zmm16\n\t"
        "vpxord %%zmm17, %%zmm17, %%zmm17\n\t"
        "vpxord %%zmm18, %%zmm18, %%zmm18\n\t"
        "vpxord %%zmm19, %%zmm19, %%zmm19\n\t"
        "vpxord %%zmm20, %%zmm20, %%zmm20\n\t"
        "vpxord %%zmm21, %%zmm21, %%zmm21\n\t"
        "vpxord %%zmm22, %%zmm22, %%zmm22\n\t"
        "vpxord %%zmm23, %%zmm23, %%zmm23\n\t"
        "vpxord %%zmm24, %%zmm24, %%zmm24\n\t"
        "vpxord %%zmm25, %%zmm25, %%zmm25\n\t"
        "vpxord %%zmm26, %%zmm26, %%zmm26\n\t"
        "vpxord %%zmm27, %%zmm27, %%zmm27\n\t"
        "vpxord %%zmm28, %%zmm28, %%zmm28\n\t"
        "vpxord %%zmm29, %%zmm29, %%zmm29\n\t"
        "vpxord %%zmm30, %%zmm30, %%zmm30\n\t"
        "vpxord %%zmm31, %%zmm31, %%zmm31\n\t"
        "kxorw %%k1, %%k1, %%k1\n\t"
        "kxorw %%k2, %%k2, %%k2\n\t"
        "kxorw %%k3, %%k3, %%k3\n\t"
        "kxorw %%k4, %%k4, %%k4\n\t"
        "kxorw %%k5, %%k5, %%k5\n\t"
        "kxorw %%k6, %%k6, %%k6\n\t"
        "kxorw %%k7, %%k7, %%k7\n\t"
        ::: "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
            "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
            "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21", "xmm22", "xmm23",
            "xmm24", "xmm25", "xmm26", "xmm27", "xmm28", "xmm29", "xmm30", "xmm31",
            "k1", "k2", "k3", "k4", "k5", "k6", "k7");
}

__attribute__((noinline, target("avx")))
static void clobber_avx(void) {
    __asm__ volatile("vzeroall" ::: "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                     "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

static void yield_now(void) {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

static void clobber_volatile_state(void) {
    if (use_avx512) clobber_avx512(); else clobber_avx();
}
static volatile long long deadline_ns;
static unsigned char *trap_page;
/* Generated code: `test ecx, [rbx]; ret` (85 0B C3). In seh mode it is covered by a function table
 * entry whose language handler resumes it, as a JIT that registers its code heap does. */
static unsigned char *trap_thunk;
static int seh_mode;

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
        if (redirect) info->ContextRecord->Rip = (DWORD64) (uintptr_t) (trap_thunk + 8);
        else info->ContextRecord->Rip += 2; /* skip `test ecx, [rbx]` (85 0B) */
        thread_traps++;
        if (self_test) info->ContextRecord->Xmm0.Low ^= 1; /* proves detection works */
        if (first_flags < 0) {
            DWORD64 mask = 0;
            first_xstate_mask = GetXStateFeaturesMask(info->ContextRecord, &mask) ? (long long) mask : -2;
            first_flags = info->ContextRecord->ContextFlags;
        }
        if (clobber) clobber_volatile_state();
        if (handler_yield) yield_now();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static EXCEPTION_DISPOSITION on_frame_exception(PEXCEPTION_RECORD r, ULONG64 frame, PCONTEXT ctx, PDISPATCHER_CONTEXT dc) {
    (void) frame; (void) dc;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2
            && (unsigned char *) r->ExceptionInformation[1] == trap_page) {
        if (redirect) ctx->Rip = (DWORD64) (uintptr_t) (trap_thunk + 8); else ctx->Rip += 2;
        thread_traps++;
        if (self_test) ctx->Xmm0.Low ^= 1;
        if (first_flags < 0) {
            DWORD64 mask = 0;
            first_xstate_mask = GetXStateFeaturesMask(ctx, &mask) ? (long long) mask : -2;
            first_flags = ctx->ContextFlags;
        }
        if (clobber) clobber_volatile_state();
        if (handler_yield) yield_now();
        return ExceptionContinueExecution;
    }
    return ExceptionContinueSearch;
}

/* Registers the thunk with RtlAddFunctionTable: a leaf entry whose UNWIND_INFO names a language
 * handler (UNW_FLAG_EHANDLER) that jumps to on_frame_exception. */
static void install_function_table(unsigned char *base) {
    unsigned char *stub = base + 16;           /* mov rax, imm64; jmp rax */
    stub[0] = 0x48; stub[1] = 0xB8;
    uint64_t target = (uint64_t) (uintptr_t) on_frame_exception;
    memcpy(stub + 2, &target, 8);
    stub[10] = 0xFF; stub[11] = 0xE0;
    unsigned char *unwind = base + 32;          /* UNWIND_INFO */
    unwind[0] = 1 | (UNW_FLAG_EHANDLER << 3);  /* version 1, exception handler */
    unwind[1] = 0;                              /* prolog size */
    unwind[2] = 0;                              /* unwind codes */
    unwind[3] = 0;                              /* frame register */
    uint32_t handler_rva = 16;
    memcpy(unwind + 4, &handler_rva, 4);
    static RUNTIME_FUNCTION rf;
    rf.BeginAddress = 0;
    rf.EndAddress = 3;
    rf.UnwindData = 32;
    if (!RtlAddFunctionTable(&rf, 1, (DWORD64) (uintptr_t) base)) {
        fprintf(stderr, "RtlAddFunctionTable failed\n");
        exit(1);
    }
}
#else
static void on_segv(int sig, siginfo_t *si, void *uc_) {
    (void) sig;
    ucontext_t *uc = uc_;
    if ((unsigned char *) si->si_addr == trap_page) {
        if (redirect) uc->uc_mcontext.gregs[REG_RIP] = (greg_t) (uintptr_t) (trap_thunk + 8);
        else uc->uc_mcontext.gregs[REG_RIP] += 2;
        thread_traps++;
        if (self_test) uc->uc_mcontext.fpregs->_xmm[0].element[0] ^= 1; /* proves detection works */
        if (clobber) clobber_volatile_state();
        if (handler_yield) yield_now();
        return;
    }
    signal(SIGSEGV, SIG_DFL);
}
#endif

/* Loads zmm0-31 and k1-k7 from `in`, executes the (possibly trapping) load, stores all back to `out`. */
__attribute__((noinline, target("avx512f")))
static void round_trip(const uint64_t *in, uint64_t *out, const uint16_t *kin, uint16_t *kout, unsigned char *page) {
    register void *thunk __asm__("r8") = trap_thunk;
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
        "sub $128, %%rsp\n\t" "call *%%r8\n\t" "add $128, %%rsp\n\t" /* thunk: test ecx, [rbx]; ret (traps when the page is no-access) */
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
        : "a"(in), "d"(out), "b"(page), "S"(kin), "D"(kout), "c"(0), "r"(thunk)
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
    register void *thunk __asm__("r8") = trap_thunk;
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
        "sub $128, %%rsp\n\t" "call *%%r8\n\t" "add $128, %%rsp\n\t" /* thunk: test ecx, [rbx]; ret (traps when the page is no-access) */
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
        : "a"(in), "d"(out), "b"(page), "c"(0), "r"(thunk)
        : "memory", "cc",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

/* Sparse variant (XSTATE_PROBE_SPARSE=1): every vector register is zero except the low 64 bits of
 * xmm16-31, written by EVEX vmovq, which is how C2 spills a reference to an upper-bank register;
 * XSTATE_PROBE_VZEROUPPER=1 also runs vzeroupper before the trap. */
__attribute__((noinline, target("avx512f")))
static void round_trip_sparse(const uint64_t *in, uint64_t *out, uint16_t *kout, unsigned char *page, int vzu) {
    register void *thunk __asm__("r8") = trap_thunk;
    __asm__ volatile(
        "vzeroall\n\t"
        "vpxord %%zmm16, %%zmm16, %%zmm16\n\t"
        "vpxord %%zmm17, %%zmm17, %%zmm17\n\t"
        "vpxord %%zmm18, %%zmm18, %%zmm18\n\t"
        "vpxord %%zmm19, %%zmm19, %%zmm19\n\t"
        "vpxord %%zmm20, %%zmm20, %%zmm20\n\t"
        "vpxord %%zmm21, %%zmm21, %%zmm21\n\t"
        "vpxord %%zmm22, %%zmm22, %%zmm22\n\t"
        "vpxord %%zmm23, %%zmm23, %%zmm23\n\t"
        "vpxord %%zmm24, %%zmm24, %%zmm24\n\t"
        "vpxord %%zmm25, %%zmm25, %%zmm25\n\t"
        "vpxord %%zmm26, %%zmm26, %%zmm26\n\t"
        "vpxord %%zmm27, %%zmm27, %%zmm27\n\t"
        "vpxord %%zmm28, %%zmm28, %%zmm28\n\t"
        "vpxord %%zmm29, %%zmm29, %%zmm29\n\t"
        "vpxord %%zmm30, %%zmm30, %%zmm30\n\t"
        "vpxord %%zmm31, %%zmm31, %%zmm31\n\t"
        "kxorw %%k1, %%k1, %%k1\n\t"
        "kxorw %%k2, %%k2, %%k2\n\t"
        "kxorw %%k3, %%k3, %%k3\n\t"
        "kxorw %%k4, %%k4, %%k4\n\t"
        "kxorw %%k5, %%k5, %%k5\n\t"
        "kxorw %%k6, %%k6, %%k6\n\t"
        "kxorw %%k7, %%k7, %%k7\n\t"
        "vmovq 16*64(%%rax), %%xmm16\n\t"
        "vmovq 17*64(%%rax), %%xmm17\n\t"
        "vmovq 18*64(%%rax), %%xmm18\n\t"
        "vmovq 19*64(%%rax), %%xmm19\n\t"
        "vmovq 20*64(%%rax), %%xmm20\n\t"
        "vmovq 21*64(%%rax), %%xmm21\n\t"
        "vmovq 22*64(%%rax), %%xmm22\n\t"
        "vmovq 23*64(%%rax), %%xmm23\n\t"
        "vmovq 24*64(%%rax), %%xmm24\n\t"
        "vmovq 25*64(%%rax), %%xmm25\n\t"
        "vmovq 26*64(%%rax), %%xmm26\n\t"
        "vmovq 27*64(%%rax), %%xmm27\n\t"
        "vmovq 28*64(%%rax), %%xmm28\n\t"
        "vmovq 29*64(%%rax), %%xmm29\n\t"
        "vmovq 30*64(%%rax), %%xmm30\n\t"
        "vmovq 31*64(%%rax), %%xmm31\n\t"
        "test %%esi, %%esi\n\t"
        "jz 1f\n\t"
        "vzeroupper\n\t"
        "1:\n\t"
        "sub $128, %%rsp\n\t" "call *%%r8\n\t" "add $128, %%rsp\n\t"
        "vmovdqu64 %%zmm0, 0*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm1, 1*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm2, 2*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm3, 3*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm4, 4*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm5, 5*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm6, 6*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm7, 7*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm8, 8*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm9, 9*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm10, 10*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm11, 11*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm12, 12*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm13, 13*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm14, 14*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm15, 15*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm16, 16*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm17, 17*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm18, 18*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm19, 19*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm20, 20*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm21, 21*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm22, 22*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm23, 23*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm24, 24*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm25, 25*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm26, 26*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm27, 27*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm28, 28*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm29, 29*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm30, 30*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm31, 31*64(%%rdx)\n\t"
        "kmovw %%k1, 0(%%rdi)\n\t"
        "kmovw %%k2, 2(%%rdi)\n\t"
        "kmovw %%k3, 4(%%rdi)\n\t"
        "kmovw %%k4, 6(%%rdi)\n\t"
        "kmovw %%k5, 8(%%rdi)\n\t"
        "kmovw %%k6, 10(%%rdi)\n\t"
        "kmovw %%k7, 12(%%rdi)\n\t"
        :
        : "a"(in), "d"(out), "b"(page), "D"(kout), "S"(vzu), "c"(0), "r"(thunk)
        : "memory", "cc",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
          "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21", "xmm22", "xmm23",
          "xmm24", "xmm25", "xmm26", "xmm27", "xmm28", "xmm29", "xmm30", "xmm31",
          "k1", "k2", "k3", "k4", "k5", "k6", "k7");
}

/* Long-lived variant (XSTATE_PROBE_SPIN=<iterations>): zmm16-31 are written once, then the thread spins
 * in scalar code (so timer interrupts and context switches happen while the values sit in the registers),
 * then traps. This is the shape of a C2 loop that keeps a value in the upper bank across many iterations. */
__attribute__((noinline, target("avx512f")))
static void round_trip_spin(const uint64_t *in, uint64_t *out, unsigned char *page, long long spins) {
    register void *thunk __asm__("r8") = trap_thunk;
    __asm__ volatile(
        "vmovdqu64 16*64(%%rax), %%zmm16\n\t"
        "vmovdqu64 17*64(%%rax), %%zmm17\n\t"
        "vmovdqu64 18*64(%%rax), %%zmm18\n\t"
        "vmovdqu64 19*64(%%rax), %%zmm19\n\t"
        "vmovdqu64 20*64(%%rax), %%zmm20\n\t"
        "vmovdqu64 21*64(%%rax), %%zmm21\n\t"
        "vmovdqu64 22*64(%%rax), %%zmm22\n\t"
        "vmovdqu64 23*64(%%rax), %%zmm23\n\t"
        "vmovdqu64 24*64(%%rax), %%zmm24\n\t"
        "vmovdqu64 25*64(%%rax), %%zmm25\n\t"
        "vmovdqu64 26*64(%%rax), %%zmm26\n\t"
        "vmovdqu64 27*64(%%rax), %%zmm27\n\t"
        "vmovdqu64 28*64(%%rax), %%zmm28\n\t"
        "vmovdqu64 29*64(%%rax), %%zmm29\n\t"
        "vmovdqu64 30*64(%%rax), %%zmm30\n\t"
        "vmovdqu64 31*64(%%rax), %%zmm31\n\t"
        "mov %%rsi, %%rdi\n\t"
        "2:\n\t"
        "dec %%rdi\n\t"
        "jnz 2b\n\t"
        "sub $128, %%rsp\n\t" "call *%%r8\n\t" "add $128, %%rsp\n\t"
        "vmovdqu64 %%zmm16, 16*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm17, 17*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm18, 18*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm19, 19*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm20, 20*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm21, 21*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm22, 22*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm23, 23*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm24, 24*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm25, 25*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm26, 26*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm27, 27*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm28, 28*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm29, 29*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm30, 30*64(%%rdx)\n\t"
        "vmovdqu64 %%zmm31, 31*64(%%rdx)\n\t"
        :
        : "a"(in), "d"(out), "b"(page), "S"(spins), "c"(0), "r"(thunk)
        : "memory", "cc", "rdi",
          "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21", "xmm22", "xmm23",
          "xmm24", "xmm25", "xmm26", "xmm27", "xmm28", "xmm29", "xmm30", "xmm31");
}

static int component_of(int reg, int q) {
    if (reg >= 16) return C_HI16_ZMM;
    if (q < 2) return C_SSE;
    if (q < 4) return C_AVX;
    return C_ZMM_HI256;
}

#ifdef _WIN32
static DWORD WINAPI noise_main(LPVOID arg) {
    (void) arg;
    while (!noise_stop) {
        unsigned char *p = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        for (int i = 0; i < (1 << 20); i += 4096) p[i] = 1;
        DWORD old;
        VirtualProtect(p, 1 << 20, PAGE_NOACCESS, &old);
        VirtualProtect(p, 1 << 20, PAGE_READWRITE, &old);
        VirtualFree(p + 65536, 65536, MEM_DECOMMIT);
        VirtualFree(p, 0, MEM_RELEASE);
        FlushProcessWriteBuffers();
    }
    return 0;
}
#else
static void *noise_main(void *arg) {
    (void) arg;
    while (!noise_stop) {
        unsigned char *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        for (int i = 0; i < (1 << 20); i += 4096) p[i] = 1;
        mprotect(p, 1 << 20, PROT_NONE);
        mprotect(p, 1 << 20, PROT_READ | PROT_WRITE);
        munmap(p, 1 << 20);
    }
    return 0;
}
#endif

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
            if (use_avx512 && spin_iterations > 0) {
                round_trip_spin(in, out, trap_page, spin_iterations);
                for (int i = 0; i < 16 * NQ; i++) out[i] = in[i];
                memcpy(kout, kin, sizeof kout);
            } else if (use_avx512 && sparse) {
                for (int i = 0; i < NREG * NQ; i++)
                    if (i < 16 * NQ || i % NQ != 0) in[i] = 0;
                memset(kin, 0, sizeof kin);
                round_trip_sparse(in, out, kout, trap_page, sparse_vzeroupper);
            } else if (use_avx512) {
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
    if (strcmp(mode, "fault") && strcmp(mode, "nofault") && strcmp(mode, "seh")) {
        fprintf(stderr, "mode: fault, seh or nofault\n");
        return 1;
    }
    use_avx512 = avx512_usable();
    if (!use_avx512 && !avx2_usable()) {
        printf("xstate_probe: neither AVX-512 nor AVX2 available, skipped\n");
        return 2;
    }
    seh_mode = !strcmp(mode, "seh");
    fault_mode = seh_mode || !strcmp(mode, "fault");
    self_test = getenv("XSTATE_PROBE_SELF_TEST") != NULL;
    clobber = getenv("XSTATE_PROBE_CLOBBER") != NULL;
    handler_yield = getenv("XSTATE_PROBE_HANDLER_YIELD") != NULL;
    redirect = getenv("XSTATE_PROBE_REDIRECT") != NULL;
    sparse = getenv("XSTATE_PROBE_SPARSE") != NULL;
    noise = getenv("XSTATE_PROBE_NOISE") != NULL;
    spin_iterations = getenv("XSTATE_PROBE_SPIN") ? atoll(getenv("XSTATE_PROBE_SPIN")) : 0;
    sparse_vzeroupper = getenv("XSTATE_PROBE_VZEROUPPER") != NULL;
#ifdef _WIN32
    trap_page = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, fault_mode ? PAGE_NOACCESS : PAGE_READONLY);
    trap_thunk = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    trap_thunk[0] = 0x85; trap_thunk[1] = 0x0B; trap_thunk[2] = 0xC3; trap_thunk[8] = 0xC3;
    if (seh_mode) install_function_table(trap_thunk); else AddVectoredExceptionHandler(1, on_exception);
#else
    trap_page = mmap(NULL, 4096, fault_mode ? PROT_NONE : PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    trap_thunk = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    trap_thunk[0] = 0x85; trap_thunk[1] = 0x0B; trap_thunk[2] = 0xC3; trap_thunk[8] = 0xC3;
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
    HANDLE noise_thread = noise ? CreateThread(NULL, 0, noise_main, NULL, 0, NULL) : NULL;
    WaitForMultipleObjects(threads, hs, TRUE, INFINITE);
    noise_stop = 1;
    if (noise_thread) WaitForSingleObject(noise_thread, INFINITE);
#else
    pthread_t *hs = calloc(threads, sizeof *hs);
    for (int t = 0; t < threads; t++) {
        ws[t].id = t;
        pthread_create(&hs[t], NULL, worker_main, &ws[t]);
    }
    pthread_t noise_thread;
    if (noise) pthread_create(&noise_thread, NULL, noise_main, NULL);
    for (int t = 0; t < threads; t++) pthread_join(hs[t], NULL);
    noise_stop = 1;
    if (noise) pthread_join(noise_thread, NULL);
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
    printf("  handler clobbers volatile vector state: %s, handler yields: %s, redirect: %s, sparse: %s%s, noise: %s, spin: %lld\n",
           clobber ? "yes" : "no", handler_yield ? "yes" : "no", redirect ? "yes" : "no", sparse ? "yes" : "no",
           sparse_vzeroupper ? " (vzeroupper before the trap)" : "", noise ? "yes" : "no", spin_iterations);
#ifdef _WIN32
    printf("  first trap: CONTEXT.ContextFlags=0x%llx, GetXStateFeaturesMask=0x%llx (-2: call failed)\n",
           first_flags, first_xstate_mask);
#endif
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

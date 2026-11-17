/*
 * xstate_detect.c: experiments for detecting and avoiding the Windows Server 2025 stale-XSAVE-header bug
 * (see xstate_stale_header.c) from inside a program, with AVX state only, so that AVX2-only hosts are covered.
 *
 *   1. self-test: fill the stack below rsp with 0x8000000000000000, load ymm0, take one access violation,
 *      continue, compare ymm0. This is what a runtime could do at start-up. Its cost is measured.
 *   2. feature query: ntdll's undocumented RtlQueryFeatureConfiguration for feature 58474193
 *      (Feature_Servicing_ExceptionDispatchUninitializedUserXStateHeaderFix), boot and runtime stores.
 *   3. geometry: where the kernel puts the XSAVE header of the exception context relative to the faulting
 *      rsp, for the four possible rsp alignments modulo 64.
 *   4. scrub: with the stack filled with the bad value, overwrite only the two quadwords the kernel will read
 *      (computed as header = (rsp - L) & ~63, L = the smallest XState.Length seen in step 3) and check that
 *      the context is complete again; with a zero XCOMP_BV slot, with a valid full header, and, as a negative
 *      control, with the scrub 16 bytes off.
 *
 * Build: clang -O2 -mxsave -o xstate_detect.exe xstate_detect.c -ladvapi32
 * MIT License.
 */
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BAD 0x8000000000000000ull
#define FILL_QWORDS 4096

typedef struct { LONG Offset; DWORD Length; } chunk_t;
typedef struct { chunk_t All, Legacy, XState; } context_ex_t;

typedef struct {
    uint64_t pattern;        /*  0 */
    uint64_t qwords;         /*  8 */
    uint64_t misalign;       /* 16: subtracted from rsp around the fault */
    uint64_t scrub;          /* 24: non-zero: write q0, q1 at rsp + scrub_off */
    int64_t scrub_off;       /* 32 */
    uint64_t q0, q1;         /* 40, 48 */
    const uint64_t *in;      /* 56 */
    uint64_t *out;           /* 64 */
    unsigned char *page;     /* 72 */
    uint64_t pre_xinuse;     /* 80 */
    uint64_t trap_rsp;       /* 88 */
} trial_t;

static unsigned char *trap_page;
static volatile uint64_t seen_mask, seen_bv, seen_comp, seen_header, seen_xlen;

static LONG CALLBACK on_exception(PEXCEPTION_POINTERS info) {
    PEXCEPTION_RECORD e = info->ExceptionRecord;
    if (e->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->NumberParameters < 2
            || e->ExceptionInformation[1] != (ULONG_PTR) trap_page) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    PCONTEXT c = info->ContextRecord;
    DWORD64 mask = 0;
    seen_mask = GetXStateFeaturesMask(c, &mask) ? mask : 0;
    seen_bv = seen_comp = seen_header = seen_xlen = 0;
    if ((c->ContextFlags & 0x100040) == 0x100040) {
        context_ex_t *ex = (context_ex_t *) (c + 1);
        uint64_t *header = (uint64_t *) ((unsigned char *) ex + ex->XState.Offset);
        seen_bv = header[0]; seen_comp = header[1];
        seen_header = (uint64_t) header; seen_xlen = ex->XState.Length;
    }
    c->Rip += 2;
    return EXCEPTION_CONTINUE_EXECUTION;
}

__attribute__((noinline, target("avx")))
static void trial(trial_t *t) {
    register trial_t *r __asm__("rsi") = t;
    __asm__ volatile(
        "movq 16(%%rsi), %%rax\n\t" "subq %%rax, %%rsp\n\t"
        "movq %%rsp, %%rdi\n\t" "movq 8(%%rsi), %%rcx\n\t" "addq $511, %%rcx\n\t" "shrq $9, %%rcx\n\t"
        "1:\n\t" "subq $4096, %%rdi\n\t" "orq $0, (%%rdi)\n\t" "decq %%rcx\n\t" "jnz 1b\n\t"
        "movq %%rsp, %%rdi\n\t" "movq 8(%%rsi), %%rcx\n\t" "leaq (,%%rcx,8), %%rax\n\t" "subq %%rax, %%rdi\n\t"
        "movq (%%rsi), %%rax\n\t" "rep stosq\n\t"
        "cmpq $0, 24(%%rsi)\n\t" "je 2f\n\t"
        "movq 32(%%rsi), %%rax\n\t" "movq 40(%%rsi), %%rcx\n\t" "movq %%rcx, (%%rsp,%%rax)\n\t"
        "movq 48(%%rsi), %%rcx\n\t" "movq %%rcx, 8(%%rsp,%%rax)\n\t"
        "2:\n\t"
        "movq 56(%%rsi), %%rax\n\t" "vmovdqu (%%rax), %%ymm0\n\t"
        "movq %%rsp, 88(%%rsi)\n\t"
        "movq 72(%%rsi), %%rbx\n\t"
        "movl $1, %%ecx\n\t" "xgetbv\n\t" "movq %%rax, 80(%%rsi)\n\t"
        "testl %%eax, (%%rbx)\n\t"                         /* 85 03: the access violation */
        "movq 64(%%rsi), %%rax\n\t" "vmovdqu %%ymm0, (%%rax)\n\t"
        "vzeroupper\n\t"
        "movq 16(%%rsi), %%rax\n\t" "addq %%rax, %%rsp\n\t"
        : : "r"(r)
        : "memory", "cc", "rax", "rbx", "rcx", "rdx", "rdi", "xmm0");
}

static const uint64_t in[4] = { 0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull, 0x4444444444444444ull };

/* Runs n trials; returns how many had the AVX component missing from the context or ymm0 changed. */
__attribute__((noinline))
static int run(trial_t *t, int n, int *lost_registers) {
    int shorts = 0, lost = 0;
    uint64_t out[4];
    for (int i = 0; i < n; i++) {
        memset(out, 0, sizeof(out));
        t->in = in; t->out = out; t->page = trap_page;
        trial(t);
        if ((t->pre_xinuse & 4) && !(seen_mask & 4)) shorts++;
        if (memcmp(out, in, sizeof(out)) != 0) lost++;
    }
    if (lost_registers) *lost_registers = lost;
    return shorts;
}

typedef struct { ULONG FeatureId; ULONG Flags; ULONG VariantPayload; } feature_config_t;
typedef LONG (NTAPI *query_feature_t)(ULONG, ULONG, PULONGLONG, feature_config_t *);

static void print_host(void) {
    int r[4];
    char brand[49] = {0};
    for (int i = 0; i < 3; i++) { __cpuid(r, (int) (0x80000002u + (unsigned) i)); memcpy(brand + 16 * i, r, 16); }
    DWORD ubr = 0, size = sizeof(DWORD);
    typedef LONG (WINAPI *rtl_get_version_t)(OSVERSIONINFOW *);
    rtl_get_version_t get = (rtl_get_version_t) GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = { sizeof(v) };
    if (get != NULL) get(&v);
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "UBR", RRF_RT_REG_DWORD, NULL, &ubr, &size);
    printf("cpu: %s\nos: Windows %lu.%lu.%lu.%lu\nxcr0: %#llx\n", brand, v.dwMajorVersion, v.dwMinorVersion,
           v.dwBuildNumber, ubr, (unsigned long long) _xgetbv(0));
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    print_host();
    int r[4];
    __cpuid(r, 1);
    if (!(r[2] & (1 << 27)) || !(r[2] & (1 << 28)) || (_xgetbv(0) & 6) != 6) { printf("no AVX\n"); return 2; }
    trap_page = (unsigned char *) VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    AddVectoredExceptionHandler(1, on_exception);

    /* 1. self-test */
    trial_t t = { BAD, FILL_QWORDS };
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    int lost1 = 0, short1 = run(&t, 1, &lost1);
    QueryPerformanceCounter(&b);
    double first_us = (double) (b.QuadPart - a.QuadPart) * 1e6 / (double) f.QuadPart;
    QueryPerformanceCounter(&a);
    int lostn = 0, shortn = run(&t, 2000, &lostn);
    QueryPerformanceCounter(&b);
    double mean_us = (double) (b.QuadPart - a.QuadPart) * 1e6 / (double) f.QuadPart / 2000.0;
    int affected = lost1 != 0;
    printf("selftest: first run ymm0 %s, context %s (%.1f us); 2000 more: ymm0 lost %d, context short %d (%.2f us each)\n",
           lost1 ? "LOST" : "kept", short1 ? "short" : "complete", first_us, lostn, shortn, mean_us);
    printf("selftest verdict: %s\n", affected ? "AFFECTED" : "not affected");

    /* 2. feature query */
    query_feature_t query = (query_feature_t) GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlQueryFeatureConfiguration");
    if (query == NULL) {
        printf("feature query: RtlQueryFeatureConfiguration not exported\n");
    } else {
        for (ULONG type = 0; type < 2; type++) {
            feature_config_t cfg = {0};
            ULONGLONG stamp = 0;
            LONG status = query(58474193u, type, &stamp, &cfg);
            printf("feature query (%s store): status %#lx, id %lu, priority %lu, enabled state %lu, raw flags %#lx, payload %#lx\n",
                   type == 0 ? "boot" : "runtime", (unsigned long) status, cfg.FeatureId, cfg.Flags & 0xf,
                   (cfg.Flags >> 4) & 3, cfg.Flags, cfg.VariantPayload);
        }
    }

    /* 3. geometry, with a harmless stack */
    uint64_t min_len = ~0ull, kernel_comp = 0;
    int64_t header_off[4];
    for (int k = 0; k < 4; k++) {
        trial_t g = { 0, FILL_QWORDS, (uint64_t) k * 16 };
        run(&g, 1, NULL);
        header_off[k] = (int64_t) (seen_header - g.trap_rsp);
        if (seen_xlen < min_len) min_len = seen_xlen;
        kernel_comp = seen_comp;
        printf("geometry: rsp %% 64 = %2llu: header at rsp%+lld, XState.Length %llu, XCOMP_BV %#llx\n",
               (unsigned long long) (g.trap_rsp % 64), (long long) header_off[k], (unsigned long long) seen_xlen,
               (unsigned long long) seen_comp);
    }
    printf("geometry: L = %llu\n", (unsigned long long) min_len);

    /* 4. scrub */
    int bad = 0;
    for (int k = 0; k < 4; k++) {
        static const struct { const char *what; int delta; int full; int expect_ok; } modes[] = {
            { "no scrub", 0, -1, 0 },
            { "XCOMP_BV slot zeroed", 0, 0, 1 },
            { "valid full header written", 0, 1, 1 },
            { "scrub 16 bytes too low (negative control)", -16, 0, 0 },
        };
        for (int m = 0; m < 4; m++) {
            trial_t s = { BAD, FILL_QWORDS, (uint64_t) k * 16 };
            /* the faulting rsp is the same for every call of trial() with this misalignment */
            trial_t g = { 0, FILL_QWORDS, (uint64_t) k * 16 };
            run(&g, 1, NULL);
            uint64_t header = (g.trap_rsp - min_len) & ~63ull;
            if (modes[m].full >= 0) {
                s.scrub = 1;
                s.scrub_off = (int64_t) (header - g.trap_rsp) + modes[m].delta;
                s.q0 = modes[m].full ? 0 : BAD;
                s.q1 = modes[m].full ? kernel_comp : 0;
            }
            int lost = 0, shorts = run(&s, 1000, &lost);
            int ok = shorts == 0 && lost == 0;
            int as_expected = affected ? ok == modes[m].expect_ok : ok;
            if (!as_expected) bad = 1;
            printf("scrub: rsp %% 64 = %2llu, %-42s: context short %4d of 1000, ymm0 lost %4d of 1000 (%s)\n",
                   (unsigned long long) (g.trap_rsp % 64), modes[m].what, shorts, lost, as_expected ? "as expected" : "UNEXPECTED");
        }
    }
    printf("scrub verdict: %s\n", bad ? "UNEXPECTED RESULT" : "all as expected");
    return affected ? 1 : 0;
}

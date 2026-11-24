/*
 * xstate_stale_header.c: standalone reproducer of JDK-8365196's root cause, a Windows kernel bug, with no JVM.
 *
 * When the kernel builds the CONTEXT of a user-mode exception on the user stack, it copies the XSTATE part with
 * RtlpCopyXStateChunk. On affected build 26100 kernels (Windows 11 24H2/25H2 before the fix, Windows Server
 * 2025) that routine first reads the XSAVE header already at the destination, which is whatever the thread left
 * on its stack. If those 16 stale bytes look like a compacted XSAVE header (second quadword with bit 63 set and
 * no bit outside the enabled features, first quadword a subset of the second), the kernel keeps the stale
 * XCOMP_BV and stores only the components it names. A stale 0x8000000000000000 (the bit pattern of -0.0 and of
 * INT64_MIN) names none, so the context has no AVX or AVX-512 state although the thread is using it. Continuing
 * from that context (EXCEPTION_CONTINUE_EXECUTION) resets the omitted components: the upper halves of
 * ymm0-ymm15 and zmm0-zmm31, xmm16-xmm31 and k1-k7 come back as zero.
 *
 * The program, on one thread and with a plain vectored exception handler:
 *   1. fills the 32 KiB below the stack pointer with one 64-bit pattern (the context, up to about 10 KiB with
 *      AMX, lands inside it);
 *   2. loads known values into ymm0-ymm15 (zmm0-zmm31 and k1-k7 when AVX-512 is enabled);
 *   3. reads a PAGE_NOACCESS page; the handler records the XSTATE mask and the XSAVE header of the context and
 *      resumes after the faulting instruction;
 *   4. stores the registers and compares them with the values of step 2.
 * It does this 1000 times for each of five patterns. The first three cannot be taken for an XSAVE header and are
 * controls; the last two are header-like.
 *
 * Exit status: 1 when a context lacked a component in use or a register changed (bug present), 0 when every
 * context was complete, 2 when the CPU has no AVX or the OS has not enabled its state (nothing can be lost).
 *
 * Build (clang, x64): clang -O2 -mxsave -o xstate_stale_header.exe xstate_stale_header.c -ladvapi32 -lversion
 *
 * MIT License.
 */
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILL_QWORDS 4096               /* 32 KiB below the stack pointer */
#define REPEATS 1000
#define VECTOR_COMPONENTS 0xe4ull      /* AVX (2), opmask (5), ZMM_Hi256 (6), Hi16_ZMM (7) */

typedef struct { LONG Offset; DWORD Length; } chunk_t;
typedef struct { chunk_t All, Legacy, XState; } context_ex_t;   /* CONTEXT_EX: follows the CONTEXT on the stack */

static unsigned char *trap_page;
static int have_avx512;

/* what the handler saw at the last fault */
static volatile uint64_t seen_mask, seen_live, seen_bv, seen_comp, seen_flags, seen_locate_avx;
static volatile uint32_t seen_pre;

__attribute__((target("xsave")))
static uint64_t xinuse(void) { return _xgetbv(1); }

static LONG CALLBACK on_exception(PEXCEPTION_POINTERS info) {
    uint64_t live = xinuse();
    PEXCEPTION_RECORD e = info->ExceptionRecord;
    if (e->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->NumberParameters < 2
            || e->ExceptionInformation[1] != (ULONG_PTR) trap_page) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    PCONTEXT c = info->ContextRecord;
    DWORD64 mask = 0;
    seen_mask = GetXStateFeaturesMask(c, &mask) ? mask : 0;
    seen_live = live;
    seen_flags = c->ContextFlags;
    seen_bv = seen_comp = 0;
    if ((c->ContextFlags & 0x100040) == 0x100040) {       /* CONTEXT_XSTATE */
        context_ex_t *ex = (context_ex_t *) (c + 1);
        uint64_t *header = (uint64_t *) ((unsigned char *) ex + ex->XState.Offset);
        seen_bv = header[0];                               /* XSTATE_BV */
        seen_comp = header[1];                             /* XCOMP_BV */
    }
    DWORD length = 0;
    seen_locate_avx = LocateXStateFeature(c, 2 /* XSTATE_AVX */, &length) != NULL;
    c->Rip += 2;                                           /* skip "test eax, [rbx]" */
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Fill the stack, load the registers, fault, store the registers. Nothing is pushed or called in between. */
__attribute__((noinline, target("avx512f,avx512bw")))
static void trial_avx512(const uint64_t *in, uint64_t *out, uint64_t pattern, uint64_t qwords) {
    register const uint64_t *r_in __asm__("rsi") = in;
    register uint64_t *r_out __asm__("r11") = out;
    register unsigned char *r_page __asm__("rbx") = trap_page;
    register uint64_t r_pattern __asm__("r8") = pattern;
    register uint64_t r_qwords __asm__("r9") = qwords;
    register volatile uint32_t *r_pre __asm__("r10") = &seen_pre;
    __asm__ volatile(
        /* touch the pages below the stack pointer in descending order, so that the stack is committed */
        "movq %%rsp, %%rdi\n\t" "leaq 511(%%r9), %%rcx\n\t" "shrq $9, %%rcx\n\t"
        "1:\n\t" "subq $4096, %%rdi\n\t" "orq $0, (%%rdi)\n\t" "decq %%rcx\n\t" "jnz 1b\n\t"
        /* fill [rsp - 8 * qwords, rsp) with the pattern */
        "movq %%rsp, %%rdi\n\t" "leaq (,%%r9,8), %%rax\n\t" "subq %%rax, %%rdi\n\t"
        "movq %%r9, %%rcx\n\t" "movq %%r8, %%rax\n\t" "rep stosq\n\t"
        "vmovdqu64 0*64(%%rsi), %%zmm0\n\t" "vmovdqu64 1*64(%%rsi), %%zmm1\n\t" "vmovdqu64 2*64(%%rsi), %%zmm2\n\t"
        "vmovdqu64 3*64(%%rsi), %%zmm3\n\t" "vmovdqu64 4*64(%%rsi), %%zmm4\n\t" "vmovdqu64 5*64(%%rsi), %%zmm5\n\t"
        "vmovdqu64 6*64(%%rsi), %%zmm6\n\t" "vmovdqu64 7*64(%%rsi), %%zmm7\n\t" "vmovdqu64 8*64(%%rsi), %%zmm8\n\t"
        "vmovdqu64 9*64(%%rsi), %%zmm9\n\t" "vmovdqu64 10*64(%%rsi), %%zmm10\n\t" "vmovdqu64 11*64(%%rsi), %%zmm11\n\t"
        "vmovdqu64 12*64(%%rsi), %%zmm12\n\t" "vmovdqu64 13*64(%%rsi), %%zmm13\n\t" "vmovdqu64 14*64(%%rsi), %%zmm14\n\t"
        "vmovdqu64 15*64(%%rsi), %%zmm15\n\t" "vmovdqu64 16*64(%%rsi), %%zmm16\n\t" "vmovdqu64 17*64(%%rsi), %%zmm17\n\t"
        "vmovdqu64 18*64(%%rsi), %%zmm18\n\t" "vmovdqu64 19*64(%%rsi), %%zmm19\n\t" "vmovdqu64 20*64(%%rsi), %%zmm20\n\t"
        "vmovdqu64 21*64(%%rsi), %%zmm21\n\t" "vmovdqu64 22*64(%%rsi), %%zmm22\n\t" "vmovdqu64 23*64(%%rsi), %%zmm23\n\t"
        "vmovdqu64 24*64(%%rsi), %%zmm24\n\t" "vmovdqu64 25*64(%%rsi), %%zmm25\n\t" "vmovdqu64 26*64(%%rsi), %%zmm26\n\t"
        "vmovdqu64 27*64(%%rsi), %%zmm27\n\t" "vmovdqu64 28*64(%%rsi), %%zmm28\n\t" "vmovdqu64 29*64(%%rsi), %%zmm29\n\t"
        "vmovdqu64 30*64(%%rsi), %%zmm30\n\t" "vmovdqu64 31*64(%%rsi), %%zmm31\n\t"
        "kmovq 32*64+8(%%rsi), %%k1\n\t" "kmovq 32*64+16(%%rsi), %%k2\n\t" "kmovq 32*64+24(%%rsi), %%k3\n\t"
        "kmovq 32*64+32(%%rsi), %%k4\n\t" "kmovq 32*64+40(%%rsi), %%k5\n\t" "kmovq 32*64+48(%%rsi), %%k6\n\t"
        "kmovq 32*64+56(%%rsi), %%k7\n\t"
        "movl $1, %%ecx\n\t" "xgetbv\n\t" "movl %%eax, (%%r10)\n\t"
        "testl %%eax, (%%rbx)\n\t"                          /* 85 03: the access violation */
        "vmovdqu64 %%zmm0, 0*64(%%r11)\n\t" "vmovdqu64 %%zmm1, 1*64(%%r11)\n\t" "vmovdqu64 %%zmm2, 2*64(%%r11)\n\t"
        "vmovdqu64 %%zmm3, 3*64(%%r11)\n\t" "vmovdqu64 %%zmm4, 4*64(%%r11)\n\t" "vmovdqu64 %%zmm5, 5*64(%%r11)\n\t"
        "vmovdqu64 %%zmm6, 6*64(%%r11)\n\t" "vmovdqu64 %%zmm7, 7*64(%%r11)\n\t" "vmovdqu64 %%zmm8, 8*64(%%r11)\n\t"
        "vmovdqu64 %%zmm9, 9*64(%%r11)\n\t" "vmovdqu64 %%zmm10, 10*64(%%r11)\n\t" "vmovdqu64 %%zmm11, 11*64(%%r11)\n\t"
        "vmovdqu64 %%zmm12, 12*64(%%r11)\n\t" "vmovdqu64 %%zmm13, 13*64(%%r11)\n\t" "vmovdqu64 %%zmm14, 14*64(%%r11)\n\t"
        "vmovdqu64 %%zmm15, 15*64(%%r11)\n\t" "vmovdqu64 %%zmm16, 16*64(%%r11)\n\t" "vmovdqu64 %%zmm17, 17*64(%%r11)\n\t"
        "vmovdqu64 %%zmm18, 18*64(%%r11)\n\t" "vmovdqu64 %%zmm19, 19*64(%%r11)\n\t" "vmovdqu64 %%zmm20, 20*64(%%r11)\n\t"
        "vmovdqu64 %%zmm21, 21*64(%%r11)\n\t" "vmovdqu64 %%zmm22, 22*64(%%r11)\n\t" "vmovdqu64 %%zmm23, 23*64(%%r11)\n\t"
        "vmovdqu64 %%zmm24, 24*64(%%r11)\n\t" "vmovdqu64 %%zmm25, 25*64(%%r11)\n\t" "vmovdqu64 %%zmm26, 26*64(%%r11)\n\t"
        "vmovdqu64 %%zmm27, 27*64(%%r11)\n\t" "vmovdqu64 %%zmm28, 28*64(%%r11)\n\t" "vmovdqu64 %%zmm29, 29*64(%%r11)\n\t"
        "vmovdqu64 %%zmm30, 30*64(%%r11)\n\t" "vmovdqu64 %%zmm31, 31*64(%%r11)\n\t"
        "kmovq %%k1, 32*64+8(%%r11)\n\t" "kmovq %%k2, 32*64+16(%%r11)\n\t" "kmovq %%k3, 32*64+24(%%r11)\n\t"
        "kmovq %%k4, 32*64+32(%%r11)\n\t" "kmovq %%k5, 32*64+40(%%r11)\n\t" "kmovq %%k6, 32*64+48(%%r11)\n\t"
        "kmovq %%k7, 32*64+56(%%r11)\n\t"
        "vzeroupper\n\t"
        :
        : "r"(r_in), "r"(r_out), "r"(r_page), "r"(r_pattern), "r"(r_qwords), "r"(r_pre)
        : "memory", "cc", "rax", "rcx", "rdx", "rdi",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
          "xmm13", "xmm14", "xmm15", "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21", "xmm22", "xmm23", "xmm24",
          "xmm25", "xmm26", "xmm27", "xmm28", "xmm29", "xmm30", "xmm31", "k1", "k2", "k3", "k4", "k5", "k6", "k7");
}

__attribute__((noinline, target("avx")))
static void trial_avx(const uint64_t *in, uint64_t *out, uint64_t pattern, uint64_t qwords) {
    register const uint64_t *r_in __asm__("rsi") = in;
    register uint64_t *r_out __asm__("r11") = out;
    register unsigned char *r_page __asm__("rbx") = trap_page;
    register uint64_t r_pattern __asm__("r8") = pattern;
    register uint64_t r_qwords __asm__("r9") = qwords;
    register volatile uint32_t *r_pre __asm__("r10") = &seen_pre;
    __asm__ volatile(
        /* touch the pages below the stack pointer in descending order, so that the stack is committed */
        "movq %%rsp, %%rdi\n\t" "leaq 511(%%r9), %%rcx\n\t" "shrq $9, %%rcx\n\t"
        "1:\n\t" "subq $4096, %%rdi\n\t" "orq $0, (%%rdi)\n\t" "decq %%rcx\n\t" "jnz 1b\n\t"
        /* fill [rsp - 8 * qwords, rsp) with the pattern */
        "movq %%rsp, %%rdi\n\t" "leaq (,%%r9,8), %%rax\n\t" "subq %%rax, %%rdi\n\t"
        "movq %%r9, %%rcx\n\t" "movq %%r8, %%rax\n\t" "rep stosq\n\t"
        "vmovdqu 0*64(%%rsi), %%ymm0\n\t" "vmovdqu 1*64(%%rsi), %%ymm1\n\t" "vmovdqu 2*64(%%rsi), %%ymm2\n\t"
        "vmovdqu 3*64(%%rsi), %%ymm3\n\t" "vmovdqu 4*64(%%rsi), %%ymm4\n\t" "vmovdqu 5*64(%%rsi), %%ymm5\n\t"
        "vmovdqu 6*64(%%rsi), %%ymm6\n\t" "vmovdqu 7*64(%%rsi), %%ymm7\n\t" "vmovdqu 8*64(%%rsi), %%ymm8\n\t"
        "vmovdqu 9*64(%%rsi), %%ymm9\n\t" "vmovdqu 10*64(%%rsi), %%ymm10\n\t" "vmovdqu 11*64(%%rsi), %%ymm11\n\t"
        "vmovdqu 12*64(%%rsi), %%ymm12\n\t" "vmovdqu 13*64(%%rsi), %%ymm13\n\t" "vmovdqu 14*64(%%rsi), %%ymm14\n\t"
        "vmovdqu 15*64(%%rsi), %%ymm15\n\t"
        "movl $1, %%ecx\n\t" "xgetbv\n\t" "movl %%eax, (%%r10)\n\t"
        "testl %%eax, (%%rbx)\n\t"                          /* 85 03: the access violation */
        "vmovdqu %%ymm0, 0*64(%%r11)\n\t" "vmovdqu %%ymm1, 1*64(%%r11)\n\t" "vmovdqu %%ymm2, 2*64(%%r11)\n\t"
        "vmovdqu %%ymm3, 3*64(%%r11)\n\t" "vmovdqu %%ymm4, 4*64(%%r11)\n\t" "vmovdqu %%ymm5, 5*64(%%r11)\n\t"
        "vmovdqu %%ymm6, 6*64(%%r11)\n\t" "vmovdqu %%ymm7, 7*64(%%r11)\n\t" "vmovdqu %%ymm8, 8*64(%%r11)\n\t"
        "vmovdqu %%ymm9, 9*64(%%r11)\n\t" "vmovdqu %%ymm10, 10*64(%%r11)\n\t" "vmovdqu %%ymm11, 11*64(%%r11)\n\t"
        "vmovdqu %%ymm12, 12*64(%%r11)\n\t" "vmovdqu %%ymm13, 13*64(%%r11)\n\t" "vmovdqu %%ymm14, 14*64(%%r11)\n\t"
        "vmovdqu %%ymm15, 15*64(%%r11)\n\t"
        "vzeroupper\n\t"
        :
        : "r"(r_in), "r"(r_out), "r"(r_page), "r"(r_pattern), "r"(r_qwords), "r"(r_pre)
        : "memory", "cc", "rax", "rcx", "rdx", "rdi",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
          "xmm13", "xmm14", "xmm15");
}

/* XCR0, or 0 when the OS has not enabled XSAVE (CR4.OSXSAVE clear: XGETBV would fault). */
__attribute__((target("xsave")))
static uint64_t os_xcr0(void) {
    int r[4];
    __cpuid(r, 1);
    return (r[2] & (1 << 27)) ? _xgetbv(0) : 0;
}

/* 0: the OS has not enabled the AVX state (or the CPU has no AVX), 1: AVX, 2: AVX-512. */
static int avx_level(void) {
    int r[4];
    __cpuid(r, 1);
    if (!(r[2] & (1 << 27)) || !(r[2] & (1 << 28))) return 0;         /* OSXSAVE, AVX */
    uint64_t xcr0 = os_xcr0();
    if ((xcr0 & 6) != 6) return 0;
    __cpuid(r, 0);
    if (r[0] >= 7) {
        __cpuidex(r, 7, 0);
        if ((r[1] & (1 << 16)) && (r[1] & (1 << 30)) && (xcr0 & 0xe0) == 0xe0) return 2;   /* AVX512F, AVX512BW */
    }
    return 1;
}

/* The file version of ntoskrnl.exe: the kernel revision, which can differ from the OS revision (UBR). */
static void kernel_file_version(char *out, size_t size) {
    snprintf(out, size, "unknown");
    char directory[MAX_PATH], path[MAX_PATH + 16];
    UINT n = GetSystemDirectoryA(directory, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    snprintf(path, sizeof(path), "%s\\ntoskrnl.exe", directory);
    DWORD ignored, length = GetFileVersionInfoSizeA(path, &ignored);
    void *data = length ? malloc(length) : NULL;
    VS_FIXEDFILEINFO *info;
    UINT info_length;
    if (data != NULL && GetFileVersionInfoA(path, 0, length, data)
            && VerQueryValueA(data, "\\", (void **) &info, &info_length) && info_length >= sizeof(*info)) {
        /* build.revision; the major and minor of the fixed version are the legacy 6.2, not 10.0 */
        snprintf(out, size, "%u.%u", (unsigned) HIWORD(info->dwFileVersionLS), (unsigned) LOWORD(info->dwFileVersionLS));
    }
    free(data);
}

static void print_host(void) {
    int r[4];
    char brand[49] = {0};
    for (int i = 0; i < 3; i++) {
        __cpuid(r, (int) (0x80000002u + (unsigned) i));
        memcpy(brand + 16 * i, r, 16);
    }
    DWORD major = 0, minor = 0, build = 0, ubr = 0, size = sizeof(DWORD);
    typedef LONG (WINAPI *rtl_get_version_t)(OSVERSIONINFOW *);
    rtl_get_version_t rtl_get_version = (rtl_get_version_t) GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = { .dwOSVersionInfoSize = sizeof(v) };
    if (rtl_get_version != NULL && rtl_get_version(&v) == 0) { major = v.dwMajorVersion; minor = v.dwMinorVersion; build = v.dwBuildNumber; }
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "UBR", RRF_RT_REG_DWORD, NULL, &ubr, &size);
    char kernel[64];
    kernel_file_version(kernel, sizeof(kernel));
    const unsigned char *shared = (const unsigned char *) 0x7ffe0000;      /* KUSER_SHARED_DATA */
    uint32_t nt_build;
    uint64_t enabled;
    uint32_t control;
    memcpy(&nt_build, shared + 0x260, 4);                                  /* NtBuildNumber: the running kernel's build */
    memcpy(&enabled, shared + 0x3d8, 8);                                   /* XState.EnabledFeatures */
    memcpy(&control, shared + 0x3ec, 4);                                   /* XState.ControlFlags: bit 1 = compaction */
    printf("cpu: %s\nos: Windows %lu.%lu.%lu.%lu\nkernel: ntoskrnl.exe file version %s, running build %u\n"
           "xcr0: %#llx, enabled XSTATE features: %#llx, compaction (XSAVEC): %s\n",
           brand, major, minor, build, ubr, kernel, nt_build & 0xffff, (unsigned long long) os_xcr0(),
           (unsigned long long) enabled, (control & 2) ? "yes" : "no");
}

int main(void) {
    int level = avx_level();
    setvbuf(stdout, NULL, _IONBF, 0);
    print_host();
    if (level == 0) {
        int r[4];
        __cpuid(r, 1);
        printf("RESULT: not applicable, the OS has not enabled the AVX state (CPUID AVX %d, OSXSAVE %d, XCR0 %#llx)\n",
               (r[2] >> 28) & 1, (r[2] >> 27) & 1, (unsigned long long) os_xcr0());
        return 2;
    }
    have_avx512 = level == 2;
    printf("vector state used: %s\n\n", have_avx512 ? "zmm0-zmm31, k1-k7 (AVX-512)" : "ymm0-ymm15 (AVX)");
    trap_page = (unsigned char *) VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    AddVectoredExceptionHandler(1, on_exception);

    static _Alignas(64) uint64_t in[33 * 8], out[33 * 8];
    for (int i = 0; i < 33 * 8; i++) in[i] = 0x0101010101010101ull * (uint64_t) (i % 250 + 1) | 0x1000000000000000ull;

    static const struct { uint64_t pattern; const char *what; } patterns[] = {
        { 0x0000000000000000ull, "control: zeros" },
        { 0x1111111111111111ull, "control: bit 63 clear" },
        { 0xffffffffffffffffull, "control: bits outside the enabled features" },
        { 0x8000000000000000ull, "-0.0 / INT64_MIN: a compacted header that names no component" },
        { 0x8000000000000004ull, "a compacted header that names AVX only" },
    };
    int failed = 0;
    for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
        long shorts = 0, lost_runs = 0, lost[8] = {0};
        uint64_t mask = 0, bv = 0, comp = 0, live = 0, flags = 0, locate = 1;
        uint32_t pre = 0;
        for (int n = 0; n < REPEATS; n++) {
            memset(out, 0, sizeof(out));
            if (have_avx512) trial_avx512(in, out, patterns[p].pattern, FILL_QWORDS);
            else trial_avx(in, out, patterns[p].pattern, FILL_QWORDS);
            int is_short = (seen_pre & ~seen_mask & VECTOR_COMPONENTS) != 0;
            if (is_short || n == 0) {
                mask = seen_mask; bv = seen_bv; comp = seen_comp; live = seen_live; flags = seen_flags; pre = seen_pre;
                locate = seen_locate_avx;
            }
            shorts += is_short;
            int bad = 0;
            int regs = have_avx512 ? 32 : 16, qw = have_avx512 ? 8 : 4;
            for (int r = 0; r < regs; r++) {
                for (int q = 0; q < qw; q++) {
                    if (out[r * 8 + q] != in[r * 8 + q]) {
                        lost[r >= 16 ? 7 : q >= 4 ? 6 : q >= 2 ? 2 : 1]++;
                        bad = 1;
                    }
                }
            }
            if (have_avx512) {
                for (int k = 1; k < 8; k++) {
                    if (out[32 * 8 + k] != in[32 * 8 + k]) { lost[5]++; bad = 1; }
                }
            }
            lost_runs += bad;
        }
        printf("stack filled with %#018llx (%s)\n", (unsigned long long) patterns[p].pattern, patterns[p].what);
        printf("  XGETBV(1) before the fault %#x, in the handler %#llx; ContextFlags %#llx\n", pre,
               (unsigned long long) live, (unsigned long long) flags);
        printf("  context: GetXStateFeaturesMask %#llx, XSTATE_BV %#llx, XCOMP_BV %#llx, LocateXStateFeature(AVX) %s\n",
               (unsigned long long) mask, (unsigned long long) bv, (unsigned long long) comp, locate ? "found" : "NULL");
        printf("  contexts without a component in use: %ld of %d; runs with registers changed after resuming: %ld of %d",
               shorts, REPEATS, lost_runs, REPEATS);
        if (lost_runs) {
            printf(" (quadwords lost: xmm %ld, ymm upper %ld, k1-k7 %ld, zmm0-15 upper %ld, zmm16-31 %ld)",
                   lost[1], lost[2], lost[5], lost[6], lost[7]);
        }
        printf("\n  => %s\n\n", shorts || lost_runs ? "BUG REPRODUCED" : "ok");
        failed |= shorts != 0 || lost_runs != 0;
    }
    printf("%s\n", failed ? "RESULT: the exception context dropped XSTATE components that were in use (bug present)"
                          : "RESULT: every exception context was complete (bug not present)");
    return failed;
}

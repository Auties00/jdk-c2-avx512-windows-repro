/*
 * xstate_seq.c: looks for the sequence of events after which Windows hands a user-mode exception handler a
 * CONTEXT whose XSTATE mask lacks components the thread is using.
 *
 * Each worker thread runs a small program, one character per step, in a loop:
 *
 *   W  write zmm0-zmm31 and k1-k7 with the thread's patterns
 *   H  write only zmm16-zmm31
 *   K  write only k1-k7
 *   L  write only the low 128 bits of zmm0-zmm15 with legacy SSE moves
 *   T  take an access violation (a read of a PAGE_NOACCESS page); the vectored handler compares the XSTATE mask
 *      of the context with XGETBV(1) read just before the fault, and resumes after the faulting instruction
 *   R  read every register back and compare it with the patterns (reads only, nothing is written)
 *   S  Sleep(1)            s  Sleep(0)            Y  SwitchToThread()
 *   B  block on an event with a 1 ms timeout
 *   P  spin, without touching vector registers, until the thread has been context-switched at least once
 *      (at most 50 ms)
 *   Q  spin for about 200 microseconds without a system call
 *   N  a system call that does not block (NtQueryInformationThread)
 *   V  vzeroupper
 *
 * A context is "short" when a component among AVX (2), opmask (5), ZMM_Hi256 (6), Hi16_ZMM (7) was in use before
 * the fault and is absent from the context. The exit status is 1 when any context was short or a k1-k7 or
 * zmm16-zmm31 value read back wrong, 0 otherwise, 2 when the CPU or the OS does not provide AVX-512.
 *
 * usage: xstate_seq <program> <seconds> <threads>
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

#define VECTOR_COMPONENTS 0xe4ull
#define MAX_STEPS 64

typedef struct {
    _Alignas(64) uint64_t in[32 * 8];
    _Alignas(64) uint64_t out[32 * 8];
    uint64_t kin[8], kout[8];
    uint64_t pre_xinuse;
    int step;
    HANDLE event;
    volatile LONG64 traps[MAX_STEPS], shorts[MAX_STEPS], mask_hist[MAX_STEPS][256], live_hist[MAX_STEPS][256];
    volatile LONG64 reads, bad_reads, bad_component[8];
    uint64_t first_bad_mask, first_bad_pre, first_bad_live;
    int lost;
} worker_t;

static unsigned char *trap_page;
static const char *program;
static int program_len;
static volatile LONG stop;
static DWORD tls_worker;

typedef LONG (NTAPI *nt_query_thread_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
static nt_query_thread_t nt_query_thread;

static uint32_t context_switches(void) {
    unsigned char buf[0x80];
    ULONG len = 0;
    uint32_t v = 0;
    /* ThreadSystemThreadInformation (40): SYSTEM_THREAD_INFORMATION, ContextSwitches at offset 0x40 */
    if (nt_query_thread != NULL && nt_query_thread(GetCurrentThread(), 40, buf, 0x50, &len) >= 0) memcpy(&v, buf + 0x40, 4);
    return v;
}

__attribute__((target("xsave"), noinline))
static uint64_t xinuse(void) { return _xgetbv(1); }

__attribute__((noinline, target("avx512f,avx512bw")))
static void write_high(const uint64_t *in) {
    __asm__ volatile(
        "vmovdqu64 16*64(%0), %%zmm16\n\t" "vmovdqu64 17*64(%0), %%zmm17\n\t" "vmovdqu64 18*64(%0), %%zmm18\n\t"
        "vmovdqu64 19*64(%0), %%zmm19\n\t" "vmovdqu64 20*64(%0), %%zmm20\n\t" "vmovdqu64 21*64(%0), %%zmm21\n\t"
        "vmovdqu64 22*64(%0), %%zmm22\n\t" "vmovdqu64 23*64(%0), %%zmm23\n\t" "vmovdqu64 24*64(%0), %%zmm24\n\t"
        "vmovdqu64 25*64(%0), %%zmm25\n\t" "vmovdqu64 26*64(%0), %%zmm26\n\t" "vmovdqu64 27*64(%0), %%zmm27\n\t"
        "vmovdqu64 28*64(%0), %%zmm28\n\t" "vmovdqu64 29*64(%0), %%zmm29\n\t" "vmovdqu64 30*64(%0), %%zmm30\n\t"
        "vmovdqu64 31*64(%0), %%zmm31\n\t"
        : : "r"(in)
        : "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21", "xmm22", "xmm23", "xmm24", "xmm25", "xmm26", "xmm27",
          "xmm28", "xmm29", "xmm30", "xmm31");
}

__attribute__((noinline, target("avx512f,avx512bw")))
static void write_low512(const uint64_t *in) {
    __asm__ volatile(
        "vmovdqu64 0*64(%0), %%zmm0\n\t" "vmovdqu64 1*64(%0), %%zmm1\n\t" "vmovdqu64 2*64(%0), %%zmm2\n\t"
        "vmovdqu64 3*64(%0), %%zmm3\n\t" "vmovdqu64 4*64(%0), %%zmm4\n\t" "vmovdqu64 5*64(%0), %%zmm5\n\t"
        "vmovdqu64 6*64(%0), %%zmm6\n\t" "vmovdqu64 7*64(%0), %%zmm7\n\t" "vmovdqu64 8*64(%0), %%zmm8\n\t"
        "vmovdqu64 9*64(%0), %%zmm9\n\t" "vmovdqu64 10*64(%0), %%zmm10\n\t" "vmovdqu64 11*64(%0), %%zmm11\n\t"
        "vmovdqu64 12*64(%0), %%zmm12\n\t" "vmovdqu64 13*64(%0), %%zmm13\n\t" "vmovdqu64 14*64(%0), %%zmm14\n\t"
        "vmovdqu64 15*64(%0), %%zmm15\n\t"
        : : "r"(in)
        : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
          "xmm13", "xmm14", "xmm15");
}

__attribute__((noinline))
static void write_low128(const uint64_t *in) {
    __asm__ volatile(
        "movdqu 0*64(%0), %%xmm0\n\t" "movdqu 1*64(%0), %%xmm1\n\t" "movdqu 2*64(%0), %%xmm2\n\t"
        "movdqu 3*64(%0), %%xmm3\n\t" "movdqu 4*64(%0), %%xmm4\n\t" "movdqu 5*64(%0), %%xmm5\n\t"
        "movdqu 6*64(%0), %%xmm6\n\t" "movdqu 7*64(%0), %%xmm7\n\t" "movdqu 8*64(%0), %%xmm8\n\t"
        "movdqu 9*64(%0), %%xmm9\n\t" "movdqu 10*64(%0), %%xmm10\n\t" "movdqu 11*64(%0), %%xmm11\n\t"
        "movdqu 12*64(%0), %%xmm12\n\t" "movdqu 13*64(%0), %%xmm13\n\t" "movdqu 14*64(%0), %%xmm14\n\t"
        "movdqu 15*64(%0), %%xmm15\n\t"
        : : "r"(in)
        : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
          "xmm13", "xmm14", "xmm15");
}

__attribute__((noinline, target("avx512f,avx512bw")))
static void write_k(const uint64_t *kin) {
    __asm__ volatile(
        "kmovq 8(%0), %%k1\n\t" "kmovq 16(%0), %%k2\n\t" "kmovq 24(%0), %%k3\n\t" "kmovq 32(%0), %%k4\n\t"
        "kmovq 40(%0), %%k5\n\t" "kmovq 48(%0), %%k6\n\t" "kmovq 56(%0), %%k7\n\t"
        : : "r"(kin) : "k1", "k2", "k3", "k4", "k5", "k6", "k7");
}

__attribute__((noinline, target("avx512f,avx512bw")))
static void read_all(uint64_t *out, uint64_t *kout) {
    __asm__ volatile(
        "vmovdqu64 %%zmm0, 0*64(%0)\n\t" "vmovdqu64 %%zmm1, 1*64(%0)\n\t" "vmovdqu64 %%zmm2, 2*64(%0)\n\t"
        "vmovdqu64 %%zmm3, 3*64(%0)\n\t" "vmovdqu64 %%zmm4, 4*64(%0)\n\t" "vmovdqu64 %%zmm5, 5*64(%0)\n\t"
        "vmovdqu64 %%zmm6, 6*64(%0)\n\t" "vmovdqu64 %%zmm7, 7*64(%0)\n\t" "vmovdqu64 %%zmm8, 8*64(%0)\n\t"
        "vmovdqu64 %%zmm9, 9*64(%0)\n\t" "vmovdqu64 %%zmm10, 10*64(%0)\n\t" "vmovdqu64 %%zmm11, 11*64(%0)\n\t"
        "vmovdqu64 %%zmm12, 12*64(%0)\n\t" "vmovdqu64 %%zmm13, 13*64(%0)\n\t" "vmovdqu64 %%zmm14, 14*64(%0)\n\t"
        "vmovdqu64 %%zmm15, 15*64(%0)\n\t" "vmovdqu64 %%zmm16, 16*64(%0)\n\t" "vmovdqu64 %%zmm17, 17*64(%0)\n\t"
        "vmovdqu64 %%zmm18, 18*64(%0)\n\t" "vmovdqu64 %%zmm19, 19*64(%0)\n\t" "vmovdqu64 %%zmm20, 20*64(%0)\n\t"
        "vmovdqu64 %%zmm21, 21*64(%0)\n\t" "vmovdqu64 %%zmm22, 22*64(%0)\n\t" "vmovdqu64 %%zmm23, 23*64(%0)\n\t"
        "vmovdqu64 %%zmm24, 24*64(%0)\n\t" "vmovdqu64 %%zmm25, 25*64(%0)\n\t" "vmovdqu64 %%zmm26, 26*64(%0)\n\t"
        "vmovdqu64 %%zmm27, 27*64(%0)\n\t" "vmovdqu64 %%zmm28, 28*64(%0)\n\t" "vmovdqu64 %%zmm29, 29*64(%0)\n\t"
        "vmovdqu64 %%zmm30, 30*64(%0)\n\t" "vmovdqu64 %%zmm31, 31*64(%0)\n\t"
        "kmovq %%k1, 8(%1)\n\t" "kmovq %%k2, 16(%1)\n\t" "kmovq %%k3, 24(%1)\n\t" "kmovq %%k4, 32(%1)\n\t"
        "kmovq %%k5, 40(%1)\n\t" "kmovq %%k6, 48(%1)\n\t" "kmovq %%k7, 56(%1)\n\t"
        : : "r"(out), "r"(kout) : "memory");
}

__attribute__((noinline, target("avx")))
static void do_vzeroupper(void) { __asm__ volatile("vzeroupper"); }

__attribute__((noinline))
static void trap(unsigned char *page) {
    __asm__ volatile("testl %%eax, (%%rcx)" : : "c"(page) : "cc");   /* 85 01: two bytes */
}

static LONG CALLBACK on_exception(PEXCEPTION_POINTERS info) {
    uint64_t live = xinuse();
    PEXCEPTION_RECORD e = info->ExceptionRecord;
    if (e->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->NumberParameters < 2
            || e->ExceptionInformation[1] != (ULONG_PTR) trap_page) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    worker_t *w = (worker_t *) TlsGetValue(tls_worker);
    DWORD64 mask = 0;
    if (!GetXStateFeaturesMask(info->ContextRecord, &mask)) mask = 0;
    int s = w->step;
    w->traps[s]++;
    w->mask_hist[s][mask & 0xff]++;
    w->live_hist[s][live & 0xff]++;
    if (w->pre_xinuse & ~mask & VECTOR_COMPONENTS) {
        if (w->shorts[s]++ == 0 && w->first_bad_pre == 0) {
            w->first_bad_mask = mask; w->first_bad_pre = w->pre_xinuse; w->first_bad_live = live;
        }
    }
    info->ContextRecord->Rip += 2;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void verify(worker_t *w) {
    read_all(w->out, w->kout);
    w->reads++;
    int bad = 0;
    for (int r = 0; r < 32; r++) {
        for (int q = 0; q < 8; q++) {
            if (w->out[r * 8 + q] != w->in[r * 8 + q]) {
                int component = r >= 16 ? 7 : q >= 4 ? 6 : q >= 2 ? 2 : 1;
                w->bad_component[component]++;
                bad = 1;
            }
        }
    }
    for (int k = 1; k < 8; k++) {
        if (w->kout[k] != w->kin[k]) { w->bad_component[5]++; bad = 1; }
    }
    if (bad) w->bad_reads++;
    /* zmm0-15 are volatile across the Windows calls of the other steps; only opmask and Hi16_ZMM losses count */
    if (w->bad_component[5] || w->bad_component[7]) w->lost = 1;
}

static DWORD WINAPI worker_main(LPVOID arg) {
    worker_t *w = (worker_t *) arg;
    TlsSetValue(tls_worker, w);
    write_low512(w->in); write_high(w->in); write_k(w->kin);
    while (!stop) {
        for (int i = 0; i < program_len; i++) {
            w->step = i;
            switch (program[i]) {
                case 'W': write_low512(w->in); write_high(w->in); write_k(w->kin); break;
                case 'H': write_high(w->in); break;
                case 'K': write_k(w->kin); break;
                case 'L': write_low128(w->in); break;
                case 'T': w->pre_xinuse = xinuse(); trap(trap_page); break;
                case 'R': verify(w); break;
                case 'S': Sleep(1); break;
                case 's': Sleep(0); break;
                case 'Y': SwitchToThread(); break;
                case 'B': WaitForSingleObject(w->event, 1); break;
                case 'N': context_switches(); break;
                case 'V': do_vzeroupper(); break;
                case 'Q': {
                    uint64_t t0 = __rdtsc();
                    while (__rdtsc() - t0 < 600000) _mm_pause();
                    break;
                }
                case 'P': {
                    uint32_t c0 = context_switches();
                    uint64_t t0 = GetTickCount64();
                    while (context_switches() == c0 && GetTickCount64() - t0 < 50 && !stop) {
                        for (int j = 0; j < 2000; j++) _mm_pause();
                    }
                    break;
                }
                default: break;
            }
        }
    }
    return 0;
}

static int avx512_usable(void) {
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return 0;
    __cpuidex(r, 7, 0);
    if (!(r[1] & (1 << 16)) || !(r[1] & (1 << 30))) return 0;     /* AVX512F, AVX512BW */
    __cpuid(r, 1);
    if (!(r[2] & (1 << 27))) return 0;                            /* OSXSAVE */
    return (_xgetbv(0) & 0xe6) == 0xe6;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <program> <seconds> <threads>\n", argv[0]);
        return 64;
    }
    program = argv[1];
    program_len = (int) strlen(program);
    if (program_len > MAX_STEPS) program_len = MAX_STEPS;
    int seconds = atoi(argv[2]), threads = atoi(argv[3]);
    if (!avx512_usable()) {
        printf("%-14s no AVX-512 on this host\n", program);
        return 2;
    }
    nt_query_thread = (nt_query_thread_t) GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    trap_page = (unsigned char *) VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    tls_worker = TlsAlloc();
    timeBeginPeriod(1);
    AddVectoredExceptionHandler(1, on_exception);
    worker_t *workers = (worker_t *) _aligned_malloc(sizeof(worker_t) * threads, 64);
    HANDLE *handles = (HANDLE *) calloc(threads, sizeof(HANDLE));
    memset(workers, 0, sizeof(worker_t) * threads);
    for (int t = 0; t < threads; t++) {
        worker_t *w = &workers[t];
        for (int i = 0; i < 32 * 8; i++) w->in[i] = 0x0101010101010101ull * (uint64_t) (i % 251 + 1) + ((uint64_t) (t + 1) << 56);
        for (int k = 1; k < 8; k++) w->kin[k] = 0x1111111111111111ull * (uint64_t) k + (uint64_t) t;
        w->event = CreateEventA(NULL, FALSE, FALSE, NULL);
        handles[t] = CreateThread(NULL, 0, worker_main, w, 0, NULL);
    }
    Sleep((DWORD) seconds * 1000);
    stop = 1;
    WaitForMultipleObjects((DWORD) threads, handles, TRUE, INFINITE);

    LONG64 total_short = 0, total_bad = 0, total_reads = 0;
    int lost = 0;
    char line[4096];
    int n = snprintf(line, sizeof(line), "%-14s threads=%-2d", program, threads);
    for (int i = 0; i < program_len; i++) {
        if (program[i] != 'T') continue;
        LONG64 traps = 0, shorts = 0, masks[256] = {0};
        for (int t = 0; t < threads; t++) {
            traps += workers[t].traps[i]; shorts += workers[t].shorts[i];
            for (int m = 0; m < 256; m++) masks[m] += workers[t].mask_hist[i][m];
        }
        total_short += shorts;
        n += snprintf(line + n, sizeof(line) - n, " | T@%d traps=%lld short=%lld masks", i, (long long) traps, (long long) shorts);
        for (int m = 0; m < 256; m++) {
            if (masks[m]) n += snprintf(line + n, sizeof(line) - n, " %#x:%lld", m, (long long) masks[m]);
        }
    }
    LONG64 bad_component[8] = {0};
    uint64_t fb_mask = 0, fb_pre = 0, fb_live = 0;
    for (int t = 0; t < threads; t++) {
        total_bad += workers[t].bad_reads; total_reads += workers[t].reads;
        lost |= workers[t].lost;
        for (int c = 0; c < 8; c++) bad_component[c] += workers[t].bad_component[c];
        if (fb_pre == 0 && workers[t].first_bad_pre != 0) {
            fb_mask = workers[t].first_bad_mask; fb_pre = workers[t].first_bad_pre; fb_live = workers[t].first_bad_live;
        }
    }
    n += snprintf(line + n, sizeof(line) - n, " | reads=%lld bad=%lld", (long long) total_reads, (long long) total_bad);
    if (total_bad) {
        n += snprintf(line + n, sizeof(line) - n, " (values lost: sse=%lld avx=%lld opmask=%lld zmm_hi256=%lld hi16_zmm=%lld)",
                      (long long) bad_component[1], (long long) bad_component[2], (long long) bad_component[5],
                      (long long) bad_component[6], (long long) bad_component[7]);
    }
    if (fb_pre) {
        n += snprintf(line + n, sizeof(line) - n, " | first short: xinuse before=%#llx context mask=%#llx xinuse in handler=%#llx",
                      (unsigned long long) fb_pre, (unsigned long long) fb_mask, (unsigned long long) fb_live);
    }
    puts(line);
    return total_short != 0 || lost;
}

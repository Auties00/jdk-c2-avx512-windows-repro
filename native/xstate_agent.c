/*
 * xstate_agent.c: observes the exception contexts a JVM receives, from inside the JVM process.
 *
 * Loaded with -agentpath. It installs a first-chance vectored exception handler that records, for every access
 * violation, what Windows put in the CONTEXT (XSTATE mask, XSAVE header, CONTEXT_EX layout) next to what the
 * processor reports (XGETBV(1)), plus the context-switch count of the thread, and passes the exception on
 * untouched. Optionally (XA_NESTED=1) the handler takes a second access violation of its own while it runs, so
 * the two contexts of one thread at one moment can be compared.
 *
 * Records are appended to the file named by XA_OUT (default xstate_agent.bin); decode with xstate_agent.py.
 *
 * MIT License. No JVM code is used; the agent entry point is declared by hand.
 */
#include <windows.h>
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t tsc;
    uint32_t tid, kind;            /* kind: 0x100 = record complete */
    uint64_t rip, rsp, addr, access;
    uint64_t ctx_mask, live_xinuse;
    uint32_t ctx_flags, mxcsr;
    uint64_t eflags;
    uint64_t xstate_bv, xcomp_bv;
    int32_t all_off, legacy_off, xstate_off; uint32_t all_len, legacy_len, xstate_len;
    uint32_t cs_before, cs_after;  /* thread context switches at handler entry and exit */
    uint64_t stack_limit, stack_base;
    uint64_t gpr[16];
    uint8_t code[48];              /* bytes at rip-32 .. rip+16 */
    uint64_t nested_mask, nested_live;
    uint64_t ctx_addr;
    uint64_t pad[2];
} rec_t;

typedef struct { LONG Offset; DWORD Length; } xa_chunk;
typedef struct { xa_chunk All, Legacy, XState; } xa_context_ex;   /* CONTEXT_EX, which follows the CONTEXT */

#define MAX_RECS (1u << 19)
static rec_t *recs;
static volatile LONG next_rec, flushed;
static HANDLE out_file;
static unsigned char *nested_page;
static int nested_enabled;
static DWORD tls_nested;

typedef LONG (NTAPI *nt_query_thread_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
static nt_query_thread_t nt_query_thread;

static uint32_t context_switches(void) {
    /* ThreadSystemThreadInformation (40): SYSTEM_THREAD_INFORMATION, ContextSwitches at offset 0x40 */
    unsigned char buf[0x80];
    ULONG len = 0;
    if (nt_query_thread == NULL || nt_query_thread(GetCurrentThread(), 40, buf, 0x50, &len) < 0) return 0xffffffffu;
    uint32_t v;
    memcpy(&v, buf + 0x40, 4);
    return v;
}

__attribute__((target("xsave")))
static uint64_t xinuse(void) { return _xgetbv(1); }

__attribute__((noinline))
static void nested_trap(unsigned char *page) {
    __asm__ volatile("testl %%eax, (%%rcx)" : : "c"(page) : "cc");   /* 85 01 */
}

static void fill(rec_t *r, PEXCEPTION_POINTERS info, uint64_t live) {
    PCONTEXT c = info->ContextRecord;
    PEXCEPTION_RECORD e = info->ExceptionRecord;
    r->tsc = __rdtsc();
    r->tid = GetCurrentThreadId();
    r->rip = c->Rip; r->rsp = c->Rsp;
    r->access = e->NumberParameters > 0 ? e->ExceptionInformation[0] : 99;
    r->addr = e->NumberParameters > 1 ? e->ExceptionInformation[1] : 0;
    r->ctx_flags = c->ContextFlags; r->mxcsr = c->MxCsr; r->eflags = c->EFlags;
    r->live_xinuse = live;
    r->ctx_addr = (uint64_t) c;
    DWORD64 mask = 0;
    r->ctx_mask = GetXStateFeaturesMask(c, &mask) ? mask : 0xdeadull;
    if (c->ContextFlags & 0x40) {
        xa_context_ex *ex = (xa_context_ex *) (c + 1);
        r->all_off = ex->All.Offset; r->all_len = ex->All.Length;
        r->legacy_off = ex->Legacy.Offset; r->legacy_len = ex->Legacy.Length;
        r->xstate_off = ex->XState.Offset; r->xstate_len = ex->XState.Length;
        uint64_t *hdr = (uint64_t *) ((unsigned char *) ex + ex->XState.Offset);
        if (ex->XState.Length >= 16) { r->xstate_bv = hdr[0]; r->xcomp_bv = hdr[1]; }
    }
    NT_TIB *tib = (NT_TIB *) NtCurrentTeb();
    r->stack_limit = (uint64_t) tib->StackLimit; r->stack_base = (uint64_t) tib->StackBase;
    r->gpr[0] = c->Rax; r->gpr[1] = c->Rcx; r->gpr[2] = c->Rdx; r->gpr[3] = c->Rbx; r->gpr[4] = c->Rsp; r->gpr[5] = c->Rbp;
    r->gpr[6] = c->Rsi; r->gpr[7] = c->Rdi; r->gpr[8] = c->R8; r->gpr[9] = c->R9; r->gpr[10] = c->R10; r->gpr[11] = c->R11;
    r->gpr[12] = c->R12; r->gpr[13] = c->R13; r->gpr[14] = c->R14; r->gpr[15] = c->R15;
}

static LONG CALLBACK on_exception(PEXCEPTION_POINTERS info) {
    uint64_t live = xinuse();
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t fault = info->ExceptionRecord->NumberParameters > 1 ? info->ExceptionRecord->ExceptionInformation[1] : 0;
    if (nested_page != NULL && fault == (uint64_t) nested_page) {
        rec_t *outer = (rec_t *) TlsGetValue(tls_nested);
        if (outer != NULL) {
            DWORD64 mask = 0;
            outer->nested_mask = GetXStateFeaturesMask(info->ContextRecord, &mask) ? mask : 0xdeadull;
            outer->nested_live = live;
        }
        info->ContextRecord->Rip += 2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    LONG i = InterlockedIncrement(&next_rec) - 1;
    if ((ULONG) i >= MAX_RECS) return EXCEPTION_CONTINUE_SEARCH;
    rec_t *r = &recs[i];
    fill(r, info, live);
    if (!IsBadReadPtr((void *) (r->rip - 32), 48)) memcpy(r->code, (void *) (r->rip - 32), 48);
    r->cs_before = context_switches();
    r->nested_mask = 0xffff; r->nested_live = 0xffff;
    if (nested_enabled) {
        TlsSetValue(tls_nested, r);
        nested_trap(nested_page);
        TlsSetValue(tls_nested, NULL);
    }
    r->cs_after = context_switches();
    r->kind = 0x100;
    return EXCEPTION_CONTINUE_SEARCH;
}

static void flush_records(void) {
    LONG end = next_rec;
    if ((ULONG) end > MAX_RECS) end = MAX_RECS;
    while (flushed < end && (recs[flushed].kind & 0x100)) {
        DWORD w;
        WriteFile(out_file, &recs[flushed], sizeof(rec_t), &w, NULL);
        flushed++;
    }
}

static DWORD WINAPI flusher(LPVOID arg) {
    (void) arg;
    for (;;) { Sleep(250); flush_records(); }
}

__declspec(dllexport) int Agent_OnLoad(void *vm, char *options, void *reserved) {
    (void) vm; (void) options; (void) reserved;
    const char *out = getenv("XA_OUT");
    const char *nested = getenv("XA_NESTED");
    nested_enabled = nested != NULL && nested[0] == '1';
    recs = (rec_t *) VirtualAlloc(NULL, (SIZE_T) MAX_RECS * sizeof(rec_t), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    nested_page = (unsigned char *) VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    tls_nested = TlsAlloc();
    nt_query_thread = (nt_query_thread_t) GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    out_file = CreateFileA(out != NULL ? out : "xstate_agent.bin", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (recs == NULL || nested_page == NULL || out_file == INVALID_HANDLE_VALUE) return 1;
    /* KUSER_SHARED_DATA.XState (offset 0x3d8): the XSTATE configuration of the system, for the record */
    FILE *cfg = fopen(getenv("XA_CFG") != NULL ? getenv("XA_CFG") : "xstate_config.txt", "w");
    if (cfg != NULL) {
        const unsigned char *x = (const unsigned char *) 0x7ffe03d8;
        for (int i = 0; i < 0x340; i++) fprintf(cfg, "%02x%s", x[i], (i & 31) == 31 ? "\n" : "");
        fprintf(cfg, "\nxcr0=%llx enabled=%llx\n", (unsigned long long) _xgetbv(0), (unsigned long long) GetEnabledXStateFeatures());
        fclose(cfg);
    }
    AddVectoredExceptionHandler(1, on_exception);
    CreateThread(NULL, 0, flusher, NULL, 0, NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void) h; (void) r;
    if (reason == DLL_PROCESS_DETACH && recs != NULL) flush_records();
    return TRUE;
}

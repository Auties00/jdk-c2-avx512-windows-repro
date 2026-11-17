# jdk-c2-avx512-windows-repro

Reproducer for a register-state corruption that HotSpot C2 code hits on GitHub `windows-latest` runners
(Windows Server 2025 24H2, build 26100, Azure Hyper-V VMs) whose CPU has AVX-512: a value that C2 keeps in an
upper-bank register (`xmm16`-`xmm31`) across a loop safepoint poll reads back as zero after the safepoint.
A reference kept there crashes with `EXCEPTION_ACCESS_VIOLATION` near address 0; data kept there gives a
silently wrong result.

Probably the same defect as [JDK-8365196](https://bugs.openjdk.org/browse/JDK-8365196)
("[Windows] EPYC 9J14 AVX vector corruption during polling-page safepoint handling", open), which Oracle and
SAP see as rare wrong results in their Windows Server 2025 CI. This repository makes it deterministic.

## Cause (2026-11-17): a stale XSAVE header on the user stack

When Windows Server 2025 (build 26100) copies the `CONTEXT` of a user-mode exception to the user stack,
`RtlpCopyXStateChunk` first reads the XSAVE header that is already at the destination, which is whatever the
thread left on its stack. If those 16 bytes pass for a compacted header (second quadword with bit 63 set and
no bit outside the enabled features, first quadword a subset of it), the kernel keeps that stale `XCOMP_BV`
and stores only the components it names. `0x8000000000000000` (-0.0, `Long.MIN_VALUE`) names none, so the
context has no AVX or AVX-512 state and continuing from it zeroes those registers. This is read from
`ntoskrnl.exe` 10.0.26100.33648 (public symbols). Windows Server 2022 clears the header before writing it.

`native/xstate_stale_header.c` reproduces it without a JVM, on one thread: it fills the 32 KiB below `rsp`
with a 64-bit pattern, loads the vector registers, reads a `PAGE_NOACCESS` page, records the context in a
vectored handler, continues, and compares the registers; 1,000 faults per pattern.

```
clang -O2 -mxsave -o xstate_stale_header.exe native/xstate_stale_header.c -ladvapi32
xstate_stale_header.exe
```

Exit status 1 means a context lacked a component in use. Run
[44070307231](https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/44070307231) (workflow
`stale-header.yml`, results taken from each job's `output.txt` artifact):

| job | OS build | CPU | XCR0 | controls (3 patterns): short contexts | `0x8000000000000000`: short, registers changed, mask, `XCOMP_BV` | `0x8000000000000004`: short, registers changed, mask, `XCOMP_BV` | exit |
|---|---|---|---|---|---|---|---|
| windows-2022-1 | 10.0.20348.5870 | AMD EPYC 9V74 80-Core Processor | 0xe7 | 0 of 3000 | 0 of 1000, 0 of 1000, 0xe7, `0x80000000000000e4` | 0 of 1000, 0 of 1000, 0xe7, `0x80000000000000e4` | 0 |
| windows-2022-2 | 10.0.20348.5870 | AMD EPYC 9V74 80-Core Processor | 0xe7 | 0 of 3000 | 0 of 1000, 0 of 1000, 0xe7, `0x80000000000000e4` | 0 of 1000, 0 of 1000, 0xe7, `0x80000000000000e4` | 0 |
| windows-2022-3 | 10.0.20348.5870 | AMD EPYC 7763 64-Core Processor | 0x7 | 0 of 3000 | 0 of 1000, 0 of 1000, 0x7, `0x8000000000000004` | 0 of 1000, 0 of 1000, 0x7, `0x8000000000000004` | 0 |
| windows-latest-1 | 10.0.26100.33674 | INTEL(R) XEON(R) PLATINUM 8573C | 0xe7 | 0 of 3000 | 1000 of 1000, 1000 of 1000, 0x3, `0x8000000000000000` | 1000 of 1000, 1000 of 1000, 0x7, `0x8000000000000004` | 1 |
| windows-latest-2 | 10.0.26100.33674 | INTEL(R) XEON(R) PLATINUM 8573C | 0x600e7 | 0 of 3000 | 1000 of 1000, 1000 of 1000, 0x3, `0x8000000000000000` | 1000 of 1000, 1000 of 1000, 0x7, `0x8000000000000004` | 1 |
| windows-latest-3 | 10.0.26100.33674 | INTEL(R) XEON(R) PLATINUM 8573C | 0xe7 | 0 of 3000 | 1000 of 1000, 1000 of 1000, 0x3, `0x8000000000000000` | 1000 of 1000, 1000 of 1000, 0x7, `0x8000000000000004` | 1 |
| windows-latest-4 | 10.0.26100.33674 | AMD EPYC 9V74 80-Core Processor | 0xe7 | 0 of 3000 | 1000 of 1000, 1000 of 1000, 0x3, `0x8000000000000000` | 1000 of 1000, 1000 of 1000, 0x7, `0x8000000000000004` | 1 |
| windows-latest-5 | 10.0.26100.33674 | INTEL(R) XEON(R) PLATINUM 8573C | 0xe7 | 0 of 3000 | 1000 of 1000, 1000 of 1000, 0x3, `0x8000000000000000` | 1000 of 1000, 1000 of 1000, 0x7, `0x8000000000000004` | 1 |
| windows-latest-6 | 10.0.26100.33674 | INTEL(R) XEON(R) PLATINUM 8573C | 0xe7 | 0 of 3000 | 1000 of 1000, 1000 of 1000, 0x3, `0x8000000000000000` | 1000 of 1000, 1000 of 1000, 0x7, `0x8000000000000004` | 1 |
| windows-latest-7 | 10.0.26100.33674 | INTEL(R) XEON(R) PLATINUM 8573C | 0xe7 | 0 of 3000 | 1000 of 1000, 1000 of 1000, 0x3, `0x8000000000000000` | 1000 of 1000, 1000 of 1000, 0x7, `0x8000000000000004` | 1 |
| windows-latest-8 | 10.0.26100.33674 | INTEL(R) XEON(R) PLATINUM 8573C | 0xe7 | 0 of 3000 | 1000 of 1000, 1000 of 1000, 0x3, `0x8000000000000000` | 1000 of 1000, 1000 of 1000, 0x7, `0x8000000000000004` | 1 |

- Windows Server 2025, 8 VMs: 16,000 of 16,000 faults over the two header-like patterns give a context
  without components in use, and the registers differ after resuming in every one (ymm upper halves, zmm0-15
  upper halves, zmm16-31, k1-k7; with `0x8000000000000004` AVX survives and the AVX-512 components do not).
  The context's `XCOMP_BV` is the value the stack was filled with. Controls: 0 of 24,000.
- Windows Server 2022, 3 VMs: 0 of 15,000. Windows 11 23H2 (22631.6199, local): 0 of 5,000.
- Not covered by this run: an AVX2-only CPU on Server 2025, AMD EPYC 9V45, Intel Xeon 8370C.
- An earlier run (44058127674) failed to link on every runner and carries no result.

The JVM connection: `native/xstate_agent.c` (a JVM agent that records the exception contexts, workflow
`agent.yml`), run
[44058574714](https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/44058574714): on four
Server 2025 AVX-512 hosts, all 11 contexts that lack a component in use have `XSTATE_BV = 0` and
`XCOMP_BV = 0x8000000000000000`; the other 6,319 have `XCOMP_BV = 0x80000000000000E4`. Which JVM frame leaves
that value on the stack was not determined.

Microsoft's fix, as read from the kernels (none of the client builds was run): `KiDispatchException` writes
the header before the copy. Windows 11 24H2 kernels have it unconditionally from 26100.8036 (March 2026); in
this Server 2025 kernel it sits behind the feature flag
`Feature_Servicing_ExceptionDispatchUninitializedUserXStateHeaderFix`, and the reproducer shows it is not
active on the runners.

## What it shows

From the runs linked below (each Windows job lands on a random runner CPU):

- Temurin 26.0.2.1+1, `Repro field` with `-XX:StressSeed=31677`, `87112` or `95029`: crashes on every
  Windows AVX-512 runner tried (Intel Xeon Platinum 8573C, AMD EPYC 9V45, AMD EPYC 9V74), usually within a
  second. Never on the AVX2-only Windows runners (AMD EPYC 7763), never on Linux runners with the same or
  similar CPUs (EPYC 9V45, EPYC 9V74, Xeon 8370C, Xeon 6975P-C), never with `-XX:UseAVX=2`.
  `-XX:-UseFPUForSpilling` (no references in XMM registers) avoids the crash but not the corruption: with it,
  Temurin 27.0.1 (seeds 13, 25) and 28-ea+24 (seeds 25, 29) give wrong results.
- The C2 code is the same on Linux: for seed 31677 the poll's oop map is `{rbx=Oop r13=Oop xmm6=Oop
  xmm19=Oop}` on both systems, and only Windows reloads 0 from `xmm19`.
- Safepoints are required, GC is not: it crashes with Epsilon GC (no collection at all) plus
  `-XX:+SafepointALot`, and with G1 and no forced safepoints; it does not crash with neither.
- Temurin 27.0.1 and the 28 early-access build (28-ea+24) are affected as well, with other seeds (a seed
  fixes one compile shape per JDK build). 28-ea+24 with its default `-XX:-UseFPUForSpilling` gives a silent
  wrong result (seed 29) instead of a crash.
- `native/xstate_probe.c`, the same trap without a JVM (zmm0-31 and k1-k7 loaded, a no-access page, a
  vectored handler or a function-table language handler like HotSpot's, the handler clobbering the volatile
  vector state, yielding, resuming elsewhere), never sees corruption on the same runners. So Windows does
  not lose the upper bank on every trap. The condition is the content of the stack below the stack pointer;
  see the section above.

StressSeed sweep 1-60 (8 s each, `Repro field`):

| host | Temurin 26.0.2.1 | Temurin 27.0.1 | 28-ea+24 | 28-ea+24 `+UseFPUForSpilling` |
|---|---|---|---|---|
| Windows, Xeon 8573C | 6 crashes (8 17 41 44 45 55) | 7 crashes (13 18 19 25 30 39 54) | 2 wrong results (15 29) | 11 crashes |
| Windows, EPYC 9V45 | 10 crashes (5 8 10 17 29 30 41 44 45 55) | 6 crashes (13 14 18 25 39 54) | 1 wrong result (29) | 11 crashes |
| Windows, EPYC 7763 (AVX2) x3 | 0 | 0 | 0 | 0 |
| Linux, Xeon 6975P-C | 0 | 0 | 0 | 0 |

## Contents

- `Repro.java`: a Vector API loop (`jdk.incubator.vector`) followed by a call through an instance field.
  Every result is compared with the scalar result, so corruption that does not crash is reported too.
  `-Drepro.churn=false` turns off the garbage thread (for Epsilon runs).
- `ScalarRepro.java`: no incubator modules; 24 double accumulators and 24 multipliers live across the loop's
  poll, compared bit for bit with the same arithmetic done in memory. It has not failed anywhere yet.
- `native/xstate_probe.c`: the JVM-free probe described above (`fault`, `seh`, `nofault` modes and
  `XSTATE_PROBE_*` switches, documented in the file).
- `native/xstate_stale_header.c`, `.github/workflows/stale-header.yml`: the standalone reproducer above.
  The summary table the workflow prints was corrected after run 44070307231 and has not been run since.
- `native/xstate_agent.c`, `native/xstate_agent.py`, `.github/workflows/agent.yml`: the in-JVM observer and its
  decoder.
- `native/xstate_seq.c`, `.github/workflows/seq.yml`: a scripted probe of register writes, context switches
  and traps, written before the cause was read from the kernel; it has not produced a result on an AVX-512
  host.
- `.github/scripts/pe_ids.py`, `.github/workflows/sysinfo.yml`: print the symbol-server identifiers of the
  runner's kernel.
- `.github/workflows/repro.yml`: runs all of the above on several Windows and Linux runners on Temurin 26,
  Temurin 27 and the latest jdk.java.net early-access build, uploads `hs_err` files, logs and a summary.
  Modes: `full`, `probe`, `diag` (GC choice, no forced safepoints, poll oop maps through
  `-XX:CompileCommand=print`), `sweep` (StressSeed sweep).

## Runs

- Full matrix with the committed defaults (12 seeds, Temurin 26/27/28-ea, 8 Windows + 3 Linux jobs):
  https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/43771266364
- First full matrix (3 seeds): https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/43765173012
- Native probe variants: https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/43766752583,
  https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/43769193115
- GC / safepoint / oop-map diagnostics: https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/43767421432
- StressSeed sweep 1-60: https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/43769066084

## Running locally

On a Windows x64 host with AVX-512:

```
java -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 \
     -XX:+StressGCM -XX:+StressLCM -XX:StressSeed=31677 \
     --add-modules jdk.incubator.vector Repro.java field 15 4
clang -O2 -o xstate_probe native/xstate_probe.c && ./xstate_probe seh 15 4
```

Exit code 0 means no corruption, 3 a wrong result; a JVM crash writes an `hs_err` file. The seeds above are
for the listed builds; another build needs its own (run the `sweep` mode).

Workaround: `-XX:UseAVX=2` (C2 then has no upper-bank registers); the only configuration that never failed.

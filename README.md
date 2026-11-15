# jdk-c2-avx512-windows-repro

Reproducer for a register-state corruption that HotSpot C2 code hits on GitHub `windows-latest` runners
(Windows Server 2025 24H2, build 26100, Azure Hyper-V VMs) whose CPU has AVX-512: a value that C2 keeps in an
upper-bank register (`xmm16`-`xmm31`) across a loop safepoint poll reads back as zero after the safepoint.
A reference kept there crashes with `EXCEPTION_ACCESS_VIOLATION` near address 0; data kept there gives a
silently wrong result.

Probably the same defect as [JDK-8365196](https://bugs.openjdk.org/browse/JDK-8365196)
("[Windows] EPYC 9J14 AVX vector corruption during polling-page safepoint handling", open), which Oracle and
SAP see as rare wrong results in their Windows Server 2025 CI. This repository makes it deterministic.

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
  not lose the upper bank on every trap; the JVM case needs a condition the probe does not recreate.

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

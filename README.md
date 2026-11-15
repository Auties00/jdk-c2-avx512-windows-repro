# jdk-c2-avx512-windows-repro

Reproducer for a register-state corruption seen by HotSpot C2 code on GitHub `windows-latest` runners
(Windows Server 2025, Azure VMs) with AVX-512: a value C2 keeps in an upper-bank register (`xmm16`-`xmm31`)
across a loop safepoint poll reads back as zero after the safepoint. A reference kept there crashes with
`EXCEPTION_ACCESS_VIOLATION` near address 0; a double kept there silently gives a wrong result.
Never seen on Linux runners with the same CPU models, nor with `-XX:UseAVX=2`.

Probably the same defect as [JDK-8365196](https://bugs.openjdk.org/browse/JDK-8365196)
("[Windows] EPYC 9J14 AVX vector corruption during polling-page safepoint handling").

## Contents

- `Repro.java`: Vector API loop (`jdk.incubator.vector`) whose oop is spilled to `xmm16`-`xmm31`;
  deterministic with `-XX:+StressGCM -XX:+StressLCM -XX:StressSeed=<seed>` on affected hosts.
- `ScalarRepro.java`: no incubator modules; 24 double accumulators and 24 multipliers live across the
  loop's poll, compared bit for bit with the same arithmetic done in memory.
- `native/xstate_probe.c`: no JVM. Loads patterns into `zmm0`-`zmm31` and `k1`-`k7`, traps on a no-access
  page (like a safepoint poll), resumes from a vectored exception handler (Windows) or a SIGSEGV handler
  (Linux), and checks every register per XSAVE component.
- `.github/workflows/repro.yml`: runs all of the above on several Windows and Linux runners, on
  Temurin 26, Temurin 27 and the latest jdk.java.net early-access build, with `-XX:UseAVX=2` and
  `-XX:-UseFPUForSpilling`/`-XX:+UseFPUForSpilling` controls; uploads `hs_err` files, logs and a summary
  table.

## Running locally

```
java -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 \
     -XX:+StressGCM -XX:+StressLCM -XX:StressSeed=87112 \
     --add-modules jdk.incubator.vector Repro.java field 40 4
java -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 \
     ScalarRepro.java doubles 30 4
clang -O2 -o xstate_probe native/xstate_probe.c && ./xstate_probe fault 15 4
```

Exit code 0 means no corruption, 3 a wrong result; a crash writes an `hs_err` file.

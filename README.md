# JDK-8365196 reproducer: Windows loses AVX state when resuming from an exception

Some Windows kernels of build 26100 build the context of a user-mode exception from a stale XSAVE header left on
the user stack. When that header names fewer components than the thread is using, resuming from the exception
resets the rest: the upper halves of ymm0-15 and zmm0-31, xmm16-31 and k1-k7. HotSpot resumes from such
exceptions at C2 safepoint polls and implicit null checks, so C2 code with vector state live there crashes or
computes wrong results.

Bug: [JDK-8365196](https://bugs.openjdk.org/browse/JDK-8365196).

## Contents

- `native/xstate_stale_header.c`: reproduces the kernel bug without a JVM. Proves the cause is the kernel.
- `Repro.java`: a Java program that crashes or computes wrong results on an affected kernel.
- `.github/scripts/kernel_ids.py`: prints the kernel a machine runs (OS build, `ntoskrnl.exe` file version and its
  symbol-server keys).
- `.github/workflows/native.yml`, `java.yml`: run the two reproducers on the GitHub `windows-2022` and
  `windows-2025` runners. A job fails when the bug reproduces.

## Which kernels are affected

Revisions are `ntoskrnl.exe` file versions, which can be lower than the OS build (UBR) an update reports.

Affected:
- Windows Server 2025, kernel 26100.33828 (the current `windows-2025` runner): gated (below), and the feature is
  not enabled. The native reproducer fails on every runner VM tried.
- Windows 11 24H2 and 25H2, kernels 26100.1 to 26100.6584.

Gated (the fix is in the kernel, behind feature 58474193; affected while the feature is off):
- Windows 11 24H2 and 25H2, kernels 26100.6725 to 26100.7824.
- Windows Server 2025, kernel 26100.33828 (above).

Fixed:
- Windows 11 24H2 and 25H2, kernels 26100.7920 to 26100.9974 (first shipped in the update with OS build
  26100.7922, KB5077241).
- Windows 11 26H1, kernels 28000.1575 to 28000.3365.

Not affected (these kernels clear the header before the copy):
- Builds 17763, 18362, 19041, 22000 and 22621: Windows Server 2019, Windows 10 1809 to 22H2, Windows 11 21H2 to
  23H2 (524 kernels).
- Windows Server 2022, kernel 20348.5866 (the `windows-2022` runner). Reproducer passes.
- An x64 process under emulation on Windows 11 on Arm64 (26200.9974). Measured.

Not examined, so nothing is claimed for them: any other build line (for example 10240, 14393, 25398), Windows
Server 2025 kernels other than 26100.33828, the 26100 revisions between the ranges above (6585 to 6724, 7825 to
7919) and after 9974, 28000 revisions outside the range above, Insider builds, and hotpatched kernels.

How this was established: the two kernel routines involved were read, with public symbols, in the 604 x64
kernels of these builds that the Microsoft symbol server serves (13 of them checked by hand). Only the two runner
kernels and the Arm64 case were also measured live.

Since January 2026, Windows Server 2025 updates are numbered on their own branch (from 26100.32230):
26100.33828 is gated although it is numerically above every fixed client revision. Compare revisions only within
one branch.

## Running the native reproducer

Requirements: Windows x64, a CPU with AVX (AVX-512 is not needed), clang.

```
clang -O2 -mxsave -o xstate_stale_header.exe native/xstate_stale_header.c -ladvapi32 -lversion
xstate_stale_header.exe
```

Or run the `native reproducer` workflow (Actions, Run workflow). The program fills the stack below `rsp` with a
64-bit pattern, loads known values into the vector registers, takes an access violation, continues from a
vectored handler and compares the registers. Five patterns, 1000 faults each; three are controls.

Exit status: 1 when the bug reproduced, 0 when not, 2 without AVX.

Affected kernel (Server 2025, AVX-512 CPU):

```
kernel: ntoskrnl.exe file version 26100.33828, running build 26100
stack filled with 0x8000000000000000 (-0.0 / INT64_MIN: a compacted header that names no component)
  XGETBV(1) before the fault 0xe7, in the handler 0xa3; ContextFlags 0x10005f
  context: GetXStateFeaturesMask 0x3, XSTATE_BV 0, XCOMP_BV 0x8000000000000000, LocateXStateFeature(AVX) NULL
  contexts without a component in use: 1000 of 1000; runs with registers changed after resuming: 1000 of 1000
  (quadwords lost: xmm 0, ymm upper 32000, k1-k7 7000, zmm0-15 upper 64000, zmm16-31 128000)
  => BUG REPRODUCED
...
RESULT: the exception context dropped XSTATE components that were in use (bug present)
```

The context's `XCOMP_BV` is the value the stack was filled with. The three controls print `=> ok`; the pattern
`0x8000000000000004` keeps AVX and loses the AVX-512 state. On an AVX2-only CPU the ymm upper halves are lost.

Unaffected kernel (Server 2022): every pattern prints `=> ok`, then

```
RESULT: every exception context was complete (bug not present)
```

## Running the Java reproducer

Requirements: Windows x64, a CPU with AVX-512 (the JVM picks `UseAVX=3`), and Temurin 27.0.1.0.2+1
([zip](https://api.adoptium.net/v3/binary/version/jdk-27.0.1.0.2%2B1/windows/x64/jdk/hotspot/normal/eclipse)).
The program has never failed on an AVX2-only CPU, on any kernel. The StressSeed values select the register
allocation of this JDK build; another build needs other seeds.

In `cmd.exe`:

```
javac --add-modules jdk.incubator.vector -d classes Repro.java
java -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 ^
     -XX:+StressGCM -XX:+StressLCM -XX:StressSeed=13 ^
     --add-modules jdk.incubator.vector -cp classes Repro 10 4
```

Or run the `java reproducer` workflow. It runs seeds 13, 18, 25, 39 and 54 with the default flags, seeds 13 and
25 with `-XX:-UseFPUForSpilling`, and seed 13 with the workaround flags.

Affected kernel, AVX-512 CPU, default flags: most seeds crash within a 10-second run. C2 kept a reference in xmm16-31
across the loop's poll, and it came back as null:

```
#  EXCEPTION_ACCESS_VIOLATION (0xc0000005) at pc=0x00000195a0c1df71, pid=3064, tid=3436
# J 1104 c2 Repro.viaField([IIII[BI)V (900 bytes) @ 0x00000195a0c1df71 [0x00000195a0c1de60+0x0000000000000111]
```

With `-XX:-UseFPUForSpilling` only data is kept there, and results are silently wrong. This is rarer: seeds 13
and 25 gave wrong results in some 30-second runs, not all.

```
wrong result at byte 771 in repro-2
calls 17026304, wrong results 17
RESULT: WRONG RESULTS
```

Exit status 3 means wrong results. Unaffected kernel, AVX2-only CPU, or `-XX:UseAVX=2 -XX:MaxVectorSize=16`:

```
calls 4195584, wrong results 0
RESULT: every result was correct
```

## Root cause

- To dispatch a user-mode exception, `KiDispatchException` copies the thread's context, including its XSAVE
  state, to the user stack. The XSAVE area lands at `(rsp - L) & -64`, where `L` depends on the enabled
  components (320 bytes with AVX only, 1920 with AVX-512).
- On build 26100, `RtlpCopyXStateChunk` reads the XSAVE header already at the destination (`XSTATE_BV`,
  `XCOMP_BV`). If it looks valid, the routine keeps it and copies only the components it names. Older kernels
  clear the header first.
- The destination is stale stack. `0x8000000000000000` (`-0.0`, `Long.MIN_VALUE`) in the `XCOMP_BV` slot is a
  valid compacted header that names no component, so the context carries no AVX or AVX-512 state.
- Resuming from that context puts the omitted components in their initial state: zero.
- Microsoft's fix: `KiDispatchException` copies the kernel's own header to the destination first. It shipped
  behind feature 58474193 (`Feature_Servicing_ExceptionDispatchUninitializedUserXStateHeaderFix`), and is
  unconditional in every client kernel examined from 26100.7920 on.
- HotSpot is exposed because it resumes from exceptions with vector state live: polling-page safepoints, implicit
  null checks and unsafe accesses in C2 code. Any program that resumes from an exception with AVX state live is
  exposed the same way.

## Workarounds

- Windows 11 24H2 or 25H2: install a cumulative update with kernel 26100.7920 or later (OS build 26100.7922,
  KB5077241, or later). A hotpatch update does not replace the kernel: the March 2026 hotpatches (OS builds
  26100.7979 to 7985) patch the gated January kernel in memory, and that combination was not examined.
- Windows Server 2025: no fixed kernel has been examined. The current 26100.33828 is gated with the fix off.
- JVM: `-XX:UseAVX=2 -XX:MaxVectorSize=16`. C2 then keeps vector values only in the low 128 bits of xmm0-15,
  which every context carries. Vector code gets slower.
- `-XX:UseAVX=2` alone avoids this reproducer, but C2 can still keep 256-bit vectors live at a trap, and their
  upper halves can be lost (the native reproducer shows it on AVX2 CPUs).
- `-XX:-UseFPUForSpilling` is not a workaround: it turns the crash into wrong results.
- A HotSpot change that recognises affected kernels and protects C2 code is in preparation; it is not in any
  JDK release yet.

## Runs

Validation runs of this tree (2026-11-22):

- Native reproducer, [run 45080102672](https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/45080102672): fails on all three `windows-2025` VMs (AMD EPYC 9V45,
  9V74, 9V74; [example](https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/45080102672/job/137386093474)), passes on all three `windows-2022` VMs (AMD EPYC
  9V74, 7763, Intel Xeon 8370C; [example](https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/45080102672/job/137386093268)).
- Java reproducer, [run 45080102671](https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/45080102671) (attempt 2): on the three `windows-2025` VMs with AVX-512
  (AMD EPYC 9V74, 9V45, Intel Xeon 8370C), 3 of 5 seeds crash on each, and two of them also give wrong results with
  `-XX:-UseFPUForSpilling` ([example](https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/45080102671/job/137387057005)). It passes on the `windows-2025` VM with
  an AVX2-only CPU and on all four `windows-2022` VMs, three of them with AVX-512
  ([example](https://github.com/Auties00/jdk-c2-avx512-windows-repro/actions/runs/45080102671/job/137387057056)). The workaround flags pass everywhere.

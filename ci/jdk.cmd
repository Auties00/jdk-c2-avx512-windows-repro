@echo off
rem Runs in the guest from the probe drive (install-jdk-vm.sh, winpe-jdk-vm.sh): unpacks each JDK of the drive,
rem compiles Repro with the notrap build, and runs Repro under the stress flags of java.yml with every configuration.
rem Each run adds one JDKRUN line to out.txt: ok (exit 0), WRONG (exit 3: a wrong result), CRASH (an hs_err file), or
rem the exit code. Configurations: notrap (AvoidLosableStateAtTraps on, the default on Windows), base (the unpatched
rem JDK), notrap-none (the notrap build with the mode off: no protection),
rem scrub14 (the previous mitigation), and the nofpu variants without UseFPUForSpilling.
rem On Arm64 (emulated, slow) only the kernel runs, longer, and there is no scrub14 build.
setlocal enabledelayedexpansion
set U=%~dp0
rem on the probe disk: Windows PE has only a small RAM disk
set W=%~d0\jdkrun
mkdir %W% 2> nul
for %%z in (notrap base scrub14) do (
  if exist %U%%%z.zip (
    mkdir %W%\%%z 2> nul
    tar -xf %U%%%z.zip -C %W%\%%z
    for /f "delims=" %%j in ('dir /b /s %W%\%%z\java.exe') do set "J_%%z=%%~dpj"
    echo JDK %%z: !J_%%z! >> %U%out.txt
  )
  rem winpe-jdk-vm.sh unpacks the JDKs on the host
  if exist %U%jdk-%%z\ (
    for /f "delims=" %%j in ('dir /b /s %U%jdk-%%z\java.exe') do set "J_%%z=%%~dpj"
    echo JDK %%z: !J_%%z! >> %U%out.txt
  )
)
echo JAVAC start >> %U%out.txt
"!J_notrap!javac.exe" --add-modules jdk.incubator.vector -d %W%\classes %U%Repro.java %U%XStateKernel.java >> %U%out.txt 2>&1
echo JAVAC exit !ERRORLEVEL! >> %U%out.txt
rem notrap-none: the notrap build with the mode off, so nothing protects compiled code (the older mitigations are
rem gone from the build)
set "NONE=-XX:-AvoidLosableStateAtTraps"
rem no pipes: each JVM writes a file, and a marker line before it tells which one was running if the guest stops
call :flags notrap notrap
call :flags notrap-none notrap "%NONE%"
call :flags base base
call :flags scrub14 scrub14
rem the deterministic kernel first: stale header planted, vectors live across polls and a faulting null check
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
  rem under emulation a slow host makes few calls a minute, and C2 code runs from about the fourth
  set "KSECS=300"
  call :kernel base base
  call :kernel notrap notrap
  call :kernel notrap-none notrap "%NONE%"
  goto :eof
)
set "KSECS=10"
for %%n in (1 2 3) do (
  call :kernel base base
  call :kernel notrap notrap
  call :kernel notrap-none notrap "%NONE%"
)
call :kernel scrub14 scrub14
set "ARGS=10 4"
rem seed 25 is the one that loses state most often under the unpatched base, so it runs four times
for %%s in (13 18 25 39 54 25 25 25) do (
  call :run notrap notrap %%s
  call :run base base %%s
  call :run notrap-none notrap %%s "%NONE%"
)
for %%s in (13 25) do (
  call :run notrap-nofpu notrap %%s -XX:-UseFPUForSpilling
  call :run base-nofpu base %%s -XX:-UseFPUForSpilling
  call :run scrub14 scrub14 %%s
)
goto :eof

rem :flags <tag> <jdk> [flags]: the CPU features, the exception mode and the vector flags each configuration starts with
:flags
if not defined J_%2 goto :eof
set "JAVA=!J_%2!java.exe"
set "EXTRA=%~3"
echo JVMLOG %1 start >> %U%out.txt
"!JAVA!" -Xlog:os+cpu=info -XX:+UnlockDiagnosticVMOptions !EXTRA! -XX:+PrintFlagsFinal -version > %W%\flags_%1.txt 2>&1
echo JVMLOG %1 exit !ERRORLEVEL! >> %U%out.txt
findstr /i "kernel exception xsave capped resumed SVE UseAVX MaxVectorSize AvoidLosableStateAtTraps UseFPUForSpilling ScrubExceptionXStateHeader" %W%\flags_%1.txt >> %U%out.txt
goto :eof

rem :kernel <tag> <jdk> [flags]: XStateKernel under the flags of the JDK test TestExceptionXStateScrub
:kernel
if not defined J_%2 goto :eof
set "JAVA=!J_%2!java.exe"
set "EXTRA=%~3"
set /a KN+=1
del /q %W%\hs_*.log 2> nul
echo KERNELSTART %1 >> %U%out.txt
"!JAVA!" -XX:+UseG1GC -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 -XX:PerMethodTrapLimit=100000 -XX:PerBytecodeTrapLimit=100000 -XX:PerMethodRecompilationCutoff=-1 -XX:PerBytecodeRecompilationCutoff=-1 !EXTRA! -XX:-CreateCoredumpOnCrash -XX:ErrorFile=%W%\hs_%%p.log -Djava.library.path=%U% --add-modules jdk.incubator.vector -cp %W%\classes XStateKernel !KSECS! > %W%\kernel_%1_!KN!.txt 2>&1
set CODE=!ERRORLEVEL!
set "R=exit !CODE!"
if "!CODE!"=="0" set R=ok
if "!CODE!"=="3" set R=WRONG
if exist %W%\hs_*.log set R=CRASH
echo KERNEL %1 run !KN! [!EXTRA!] = !R! >> %U%out.txt
goto :eof

rem :run <tag> <jdk> <seed> [flags]
:run
if not defined J_%2 goto :eof
set "JAVA=!J_%2!java.exe"
set "EXTRA=%~4"
del /q %W%\hs_*.log 2> nul
echo JDKSTART %1 seed %3 >> %U%out.txt
"!JAVA!" -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 -XX:+StressGCM -XX:+StressLCM -XX:StressSeed=%3 !EXTRA! -XX:-CreateCoredumpOnCrash -XX:ErrorFile=%W%\hs_%%p.log --add-modules jdk.incubator.vector -cp %W%\classes Repro !ARGS! > %W%\log_%1_%3.txt 2>&1
set CODE=!ERRORLEVEL!
set "R=exit !CODE!"
if "!CODE!"=="0" set R=ok
if "!CODE!"=="3" set R=WRONG
if exist %W%\hs_*.log set R=CRASH
set "CALLS="
for /f "tokens=*" %%c in ('findstr /b "calls" %W%\log_%1_%3.txt') do set "CALLS=%%c"
echo JDKRUN %1 seed %3 [!EXTRA!] = !R! (!CALLS!) >> %U%out.txt
if "!R!"=="CRASH" for %%h in (%W%\hs_*.log) do findstr /b "siginfo" %%h >> %U%out.txt
goto :eof

@echo off
rem Runs in the guest from the probe drive (install-jdk-vm.sh, winpe-jdk-vm.sh): unpacks each JDK of the drive,
rem compiles Repro with the notrap build, and runs Repro under the stress flags of java.yml with every configuration.
rem Each run adds one JDKRUN line to out.txt: ok (exit 0), WRONG (exit 3: a wrong result), CRASH (an hs_err file), or
rem the exit code. Configurations: notrap (AvoidLosableStateAtTraps on, the default on Windows), base (the unpatched
rem JDK), notrap-none (the notrap build with the mode and scrub14's scrub both off: no protection), notrap-off (the
rem mode off, so scrub14's machinery protects instead), scrub14, and the nofpu variants without UseFPUForSpilling.
rem On Arm64 (emulated, slow) fewer seeds run and Repro runs longer, and there is no scrub14 build.
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
"!J_notrap!javac.exe" --add-modules jdk.incubator.vector -d %W%\classes %U%Repro.java >> %U%out.txt 2>&1
set "NONE=-XX:-AvoidLosableStateAtTraps -XX:-ScrubExceptionXStateHeader"
echo JVMLOG notrap >> %U%out.txt
"!J_notrap!java.exe" -Xlog:os+cpu=info -XX:+UnlockDiagnosticVMOptions -XX:+PrintFlagsFinal -version 2>&1 | findstr /i "kernel exception xsave capped resumed SVE UseAVX MaxVectorSize AvoidLosableStateAtTraps UseFPUForSpilling" >> %U%out.txt
echo JVMLOG notrap-none >> %U%out.txt
if /i not "%PROCESSOR_ARCHITECTURE%"=="ARM64" "!J_notrap!java.exe" -Xlog:os+cpu=info -XX:+UnlockDiagnosticVMOptions %NONE% -XX:+PrintFlagsFinal -version 2>&1 | findstr /i "kernel exception xsave capped resumed SVE UseAVX MaxVectorSize AvoidLosableStateAtTraps ScrubExceptionXStateHeader" >> %U%out.txt
for %%z in (base scrub14) do (
  if defined J_%%z (
    echo JVMLOG %%z >> %U%out.txt
    "!J_%%z!java.exe" -Xlog:os+cpu=info -XX:+UnlockDiagnosticVMOptions -XX:+PrintFlagsFinal -version 2>&1 | findstr /i "kernel exception xsave capped resumed SVE UseAVX MaxVectorSize" >> %U%out.txt
  )
)
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
  set "ARGS=30 4"
  rem no notrap-none here: ScrubExceptionXStateHeader is an x64 flag, and with the mode off the notrap build does
  rem not use SVE on these kernels, so the unpatched base is the run without protection
  for %%s in (13 25 39) do (
    call :run notrap notrap %%s
    call :run base base %%s
  )
  call :run notrap-off notrap 25 -XX:-AvoidLosableStateAtTraps
  goto :eof
)
set "ARGS=10 4"
rem seed 25 is the one that loses state most often under the unpatched base, so it runs four times
for %%s in (13 18 25 39 54 25 25 25) do (
  call :run notrap notrap %%s
  call :run base base %%s
  call :run notrap-none notrap %%s "%NONE%"
)
for %%s in (13 25) do (
  call :run notrap-off notrap %%s -XX:-AvoidLosableStateAtTraps
  call :run notrap-nofpu notrap %%s -XX:-UseFPUForSpilling
  call :run base-nofpu base %%s -XX:-UseFPUForSpilling
  call :run scrub14 scrub14 %%s
)
goto :eof

rem :run <tag> <jdk> <seed> [flags]
:run
if not defined J_%2 goto :eof
set "JAVA=!J_%2!java.exe"
set "EXTRA=%~4"
del /q %W%\hs_*.log 2> nul
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

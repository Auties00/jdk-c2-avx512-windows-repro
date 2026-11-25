@echo off
rem Runs in the guest from the probe drive (install-jdk-vm.sh): unpacks each JDK of the drive, compiles Repro with
rem the notrap build, and runs Repro under the stress flags of java.yml with every configuration. Each run adds one
rem JDKRUN line to out.txt: ok (exit 0), WRONG (exit 3: a wrong result), CRASH (an hs_err file), or the exit code.
setlocal enabledelayedexpansion
set U=%~dp0
set W=C:\jdkrun
mkdir %W% 2> nul
for %%z in (notrap base scrub14) do (
  if exist %U%%%z.zip (
    mkdir %W%\%%z 2> nul
    tar -xf %U%%%z.zip -C %W%\%%z
    for /f "delims=" %%j in ('dir /b /s %W%\%%z\java.exe') do set "J_%%z=%%~dpj"
    echo JDK %%z: !J_%%z! >> %U%out.txt
  )
)
"!J_notrap!javac.exe" --add-modules jdk.incubator.vector -d %W%\classes %U%Repro.java >> %U%out.txt 2>&1
for %%z in (notrap base scrub14) do (
  echo JVMLOG %%z >> %U%out.txt
  "!J_%%z!java.exe" -Xlog:os+cpu=info -XX:+UnlockDiagnosticVMOptions -XX:+PrintFlagsFinal -version 2>&1 | findstr /i "kernel exception xsave capped resumed UseAVX MaxVectorSize AvoidLosableStateAtTraps UseFPUForSpilling" >> %U%out.txt
)
for %%s in (13 18 25 39 54) do (
  call :run notrap notrap %%s
  call :run base base %%s
  call :run notrap-off notrap %%s -XX:-AvoidLosableStateAtTraps
)
for %%s in (13 25) do (
  call :run notrap-nofpu notrap %%s -XX:-UseFPUForSpilling
  call :run base-nofpu base %%s -XX:-UseFPUForSpilling
)
call :run scrub14 scrub14 13
goto :eof

rem :run <tag> <jdk> <seed> [flag]
:run
set "JAVA=!J_%2!java.exe"
set "EXTRA=%4"
del /q %W%\hs_*.log 2> nul
"!JAVA!" -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 -XX:+StressGCM -XX:+StressLCM -XX:StressSeed=%3 !EXTRA! -XX:-CreateCoredumpOnCrash -XX:ErrorFile=%W%\hs_%%p.log --add-modules jdk.incubator.vector -cp %W%\classes Repro 10 4 > %W%\log_%1_%3.txt 2>&1
set CODE=!ERRORLEVEL!
set "R=exit !CODE!"
if "!CODE!"=="0" set R=ok
if "!CODE!"=="3" set R=WRONG
if exist %W%\hs_*.log set R=CRASH
set "CALLS="
for /f "tokens=*" %%c in ('findstr /b "calls" %W%\log_%1_%3.txt') do set "CALLS=%%c"
echo JDKRUN %1 seed %3 !EXTRA!: !R! (!CALLS!) >> %U%out.txt
if "!R!"=="CRASH" for %%h in (%W%\hs_*.log) do findstr /b "siginfo" %%h >> %U%out.txt
goto :eof

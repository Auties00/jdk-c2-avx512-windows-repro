#!/usr/bin/env bash
# Runs Repro.java against a JDK bundle (zip) built elsewhere, for the StressSeed values in $SEEDS, with the default
# flags and with -XX:+UseFPUForSpilling. Writes out/results.tsv (os, cpu, os_build, flags, seed, result, trace
# mismatches, detail), out/logs and out/hs_err. JDK8365196_TRACE=1 is set so an instrumented build reports, at each
# polling-page trap, whether the xmm16-31 values the safepoint blob saved differ from the exception CONTEXT.
set -u
bundle=$1
mkdir -p out/logs out/hs_err jdk
unzip -q "$bundle" -d jdk
java_home=$(dirname "$(dirname "$(find jdk -name java.exe -path '*/bin/*' | head -1)")")
java=$java_home/bin/java
"$java" -version 2>&1 | tee out/version.txt
"$java" -XX:+PrintFlagsFinal -version 2>/dev/null | grep -E ' (UseAVX|UseFPUForSpilling|MaxVectorSize) ' | tee out/flags.txt
cpu=$(powershell -NoProfile -Command "(Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()")
build=$(powershell -NoProfile -Command "\$v = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'; \"\$(\$v.ProductName) \$(\$v.DisplayVersion) \$(\$v.CurrentBuild).\$(\$v.UBR)\"")
echo "CPU: $cpu"; echo "OS: $build"
printf '%s' "$cpu" > out/cpu.txt; printf '%s' "$build" > out/build.txt; printf '%s' "${RUNNER_OS:-}" > out/os.txt
"$java_home/bin/javac" --add-modules jdk.incubator.vector -d classes repro/Repro.java 2>&1 | grep -v -i warning

for seed in $SEEDS; do
  for flags in "-" "-XX:+UseFPUForSpilling"; do
    extra=(); [ "$flags" != - ] && extra=("$flags")
    tag="seed${seed}${flags//[^A-Za-z0-9]/_}"
    rm -f hs_err_*.log
    JDK8365196_TRACE=1 "$java" -Xmx512m -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 \
      -XX:+StressGCM -XX:+StressLCM -XX:StressSeed="$seed" "${extra[@]}" -XX:-CreateCoredumpOnCrash \
      -XX:ErrorFile=hs_err_%p.log --add-modules jdk.incubator.vector -cp classes Repro field "$SECONDS_PER_RUN" 4 \
      > "out/logs/$tag.log" 2>&1
    code=$?
    traces=$(grep -c 'MISMATCH' "out/logs/$tag.log")
    detail=$(grep -m1 'MISMATCH' "out/logs/$tag.log" | cut -c1-300)
    if ls hs_err_*.log > /dev/null 2>&1; then
      result=CRASH
      f=$(ls hs_err_*.log | head -1)
      [ -z "$detail" ] && detail=$(grep -m1 '^siginfo' "$f")
      mv "$f" "out/hs_err/$tag.log"
    elif [ $code = 0 ]; then result=ok
    elif [ $code = 3 ]; then result=WRONG; [ -z "$detail" ] && detail=$(grep -m1 mismatch "out/logs/$tag.log")
    else result="exit $code"
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${RUNNER_OS:-}" "$cpu" "$build" "$flags" "$seed" "$result" "$traces" "$detail" >> out/results.tsv
    echo "seed=$seed flags=$flags -> $result (trace mismatches: $traces) $detail"
  done
done
echo "== summary ($cpu, $build)"
awk -F'\t' '{k=$4; n[k]++; if ($6!="ok") b[k]++; t[k]+=$7} END {for (k in n) printf "%s: %d/%d failed, %d trace mismatches\n", k, b[k], n[k], t[k]}' out/results.tsv | tee out/summary.txt
{ echo "### $cpu ($build)"; echo '```'; cat out/summary.txt; echo '```'; } >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
exit 0

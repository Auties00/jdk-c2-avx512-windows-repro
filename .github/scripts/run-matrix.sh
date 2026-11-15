#!/usr/bin/env bash
# Runs the native probe and the Java reproducers on this host and appends one TSV row per run to
# out/results.tsv: os, cpu, os_build, isa, jdk, test, flags, seed, result, detail.
# Environment: JDKS="name=path name=path ...", SECONDS_JAVA, SECONDS_VECTOR, SECONDS_PROBE, SEEDS.
set -u
mkdir -p out/hs_err out/logs
os=$(cat out/os.txt)
cpu=$(cat out/cpu.txt)
build=$(cat out/build.txt)
exe=./xstate_probe
[ -f xstate_probe.exe ] && exe=./xstate_probe.exe

row() { printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$os" "$cpu" "$build" "$isa" "$1" "$2" "$3" "$4" "$5" "$6" >> out/results.tsv; }

isa=$($exe nofault 1 1 | sed -n 's/.*isa=\([a-z0-9]*\).*/\1/p')
[ -z "$isa" ] && isa=none
echo "$isa" > out/isa.txt

# mode threads env (env: comma-separated XSTATE_PROBE_* switches, or -)
for probe in "fault 4 -" "seh 4 -" "fault 4 CLOBBER" "seh 4 CLOBBER" "seh 8 HANDLER_YIELD" "seh 8 CLOBBER,HANDLER_YIELD" "nofault 4 -"; do
  set -- $probe
  envs=()
  if [ "$3" != - ]; then for e in ${3//,/ }; do envs+=("XSTATE_PROBE_$e=1"); done; fi
  log=out/logs/probe-$1-$2-${3//,/_}.log
  env "${envs[@]}" $exe "$1" "$SECONDS_PROBE" "$2" > "$log" 2>&1
  code=$?
  cat "$log"
  case $code in
    0) result=ok ;;
    3) result=CORRUPTED ;;
    *) result="exit $code" ;;
  esac
  detail=$(grep -E 'first trap|corrupted iterations [1-9]|registers hit: .' "$log" | sed 's/^ *//' | tr '\n' ';' | tr -s ' ')
  row native "xstate_probe $1" "threads=$2 $3" - "$result" "$detail"
done
[ "${PROBE_ONLY:-false}" = true ] && exit 0

# java <jdk> <test> <seed> <flags...>; classifies by exit code and hs_err presence
run_java() {
  local jdk=$1 test=$2 seed=$3; shift 3
  local java=${JDK_PATH}/bin/java
  local tag
  tag=$(echo "$jdk-$test-$seed-$*" | tr -c 'A-Za-z0-9\n' '_')
  rm -f hs_err_*.log
  local stress=()
  [ "$seed" != - ] && stress=(-XX:+StressGCM -XX:+StressLCM -XX:StressSeed=$seed)
  local args=()
  case $test in
    vector-field) args=(--add-modules jdk.incubator.vector -cp classes Repro field "$SECONDS_VECTOR" 4) ;;
    scalar-doubles) args=(-cp classes ScalarRepro doubles "$SECONDS_JAVA" 4) ;;
    scalar-oop) args=(-cp classes ScalarRepro oop "$SECONDS_JAVA" 4) ;;
  esac
  "$java" -Xmx512m -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 \
    "${stress[@]}" "$@" -XX:-CreateCoredumpOnCrash -XX:ErrorFile=hs_err_%p.log "${args[@]}" \
    > "out/logs/$tag.log" 2>&1
  local code=$? result detail=""
  if ls hs_err_*.log > /dev/null 2>&1; then
    result=CRASH
    local f
    f=$(ls hs_err_*.log | head -1)
    detail=$( (grep -m1 -E '^siginfo' "$f"; grep -m1 -E '^# (J|V|C) ' "$f"; grep -m1 -E 'ImmutableOopMap.*xmm(1[6-9]|2[0-9]|3[01])' "$f" | sed 's/.*ImmutableOopMap/oopmap/') | tr '\n' ';' | tr -s ' ')
    mv "$f" "out/hs_err/$tag.log"
  elif [ $code = 0 ]; then
    result=ok
  elif [ $code = 3 ]; then
    result=MISMATCH
    detail=$(grep -m1 mismatch "out/logs/$tag.log")
  else
    result="exit $code"
    detail=$(tail -2 "out/logs/$tag.log" | tr '\n' ';')
  fi
  row "$jdk" "$test" "${*:--}" "$seed" "$result" "$detail"
  echo "$jdk $test [$*] seed=$seed -> $result $detail"
}

for spec in $JDKS; do
  jdk=${spec%%=*}
  JDK_PATH=${spec#*=}
  [ -x "$JDK_PATH/bin/java" ] || [ -x "$JDK_PATH/bin/java.exe" ] || { echo "skip $jdk"; continue; }
  version=$("$JDK_PATH/bin/java" -version 2>&1 | sed -n 2p)
  flags=$("$JDK_PATH/bin/java" -XX:+PrintFlagsFinal -version 2>/dev/null | grep -E ' (UseAVX|UseFPUForSpilling|MaxVectorSize) ' | awk '{print $2"="$4}' | tr '\n' ' ')
  echo "== $jdk: $version; $flags"
  echo "$jdk	$version	$flags" >> out/jdks.tsv
  rm -rf classes
  "$JDK_PATH/bin/javac" --add-modules jdk.incubator.vector -d classes Repro.java ScalarRepro.java 2>&1 | grep -v -i warning
  run_java "$jdk" scalar-doubles -
  run_java "$jdk" scalar-doubles - -XX:UseAVX=2
  run_java "$jdk" scalar-oop -
  for seed in $SEEDS; do
    run_java "$jdk" vector-field "$seed"
    run_java "$jdk" vector-field "$seed" -XX:UseAVX=2
    run_java "$jdk" vector-field "$seed" -XX:-UseFPUForSpilling
    run_java "$jdk" vector-field "$seed" -XX:+UseFPUForSpilling
  done
done
exit 0

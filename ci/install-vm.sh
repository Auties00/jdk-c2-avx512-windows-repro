#!/bin/bash
# Installs Windows from a medium into a QEMU guest, unattended, and runs the native reproducer on the installed
# system at logon. An installed system is what a user runs: unlike the Windows PE of the medium it has the
# feature overrides the edition ships with, the kernel of the install image, and XSAVE on in every version.
# The guest has no network, so nothing can replace the kernel after the installation.
#
# Input (environment), one entry of ci/windows-images.json:
#   NAME      what the medium is
#   ARCH      x64 (KVM) or arm64 (emulated, with SVE)
#   TYPE      iso: a Windows ISO, booted as it is (x64 through BIOS, Arm64 through UEFI)
#             esd: an install ESD of the Media Creation Tool kind; its Windows Setup image (3) is booted through
#                  UEFI and installs from the ESD itself
#   URL, SHA1 where to download it; the SHA-1 published for it, or empty
#   EDITION   a regular expression matched against the image names; the first match is installed
#   KEY       a generic installation key for media that ask for one, or empty
#   LEGACY    non-empty for Windows 7, whose Setup rejects the newer answer-file settings
#   CPU       the QEMU CPU model of an x64 guest (host: the runner's CPU)
#   OVERRIDE  none, or 1 / 2: sets FeatureManagement\Overrides\3\1150694031 EnabledState on the installed system
#             (1 disabled, 2 enabled) and restarts before measuring
#   LIMIT     seconds the guest may run
# The medium is checked before it boots: the download against SHA1 when given, and the kernel of the image to
# install against ci/kernel-hashes.tsv; one of the two must hold.
# Output: out/out.txt, out/result.txt (one line), out/screens (last screenshots), out/timing.txt.
set -euo pipefail
: "${SHA1:=}" "${LIMIT:=7200}" "${CPU:=host}" "${EDITION:=.}" "${KEY:=}" "${OVERRIDE:=none}"
export WIMLIB_IMAGEX_IGNORE_CASE=1
work=${WORK:-$PWD/work}; here=$PWD
mkdir -p "$work" out/screens
cd "$work"
t0=$(date +%s)
verified=

finish() {
  [ -f "$here/out/out.txt" ] || : > "$here/out/out.txt"
  local kernel reported state cpu
  kernel=$(sed -n 's/^KVER file .*ntoskrnl.exe: [0-9]*\.[0-9]*\.\([0-9]*\.[0-9]*\).*/\1/p' "$here/out/out.txt" 2>/dev/null | head -1)
  reported=$(sed -n 's/^KVER class222 status 0x00000000 .* version \([0-9.]*\).*/\1/p' "$here/out/out.txt" 2>/dev/null | head -1)
  state=$(sed -n 's/^ *EnabledState *REG_DWORD *0x\([0-9]\).*/\1/p' "$here/out/out.txt" 2>/dev/null | head -1)
  cpu=$(sed -n 's/^cpu: //p' "$here/out/out.txt" 2>/dev/null | head -1)
  if [ -z "$cpu" ]; then
    cpu=$(lscpu | sed -n 's/^Model name: *//p' | head -1)
    if [ "$ARCH" = arm64 ]; then cpu="$cpu emulating a Neoverse V1"; fi
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$NAME" "$ARCH" "${kernel:-${verified:-unknown}}" "${reported:-not reported}" \
    "installed, $([ "$OVERRIDE" = none ] && echo 'as shipped' || echo "override EnabledState=$OVERRIDE")" "${state:-absent}" "$cpu" "$1" > "$here/out/result.txt"
  cat "$here/out/result.txt"
  echo "total $(( $(date +%s) - t0 )) s; disk free $(df -BG --output=avail "$work" | tail -1 | tr -d ' ')" >> "$here/out/timing.txt"
  cat "$here/out/timing.txt"
}

echo "::group::download and verify"
df -h "$work" | tail -1
# resumable: a server may close a long transfer early
for attempt in 1 2 3 4 5 6 7 8; do
  if curl -fsSL --retry 5 --retry-delay 10 --retry-all-errors -C - -o media.bin "$URL"; then break; fi
  echo "download attempt $attempt stopped at $(stat -c %s media.bin 2> /dev/null || echo 0) bytes"
  sleep 15
done
ls -l media.bin
echo "download $(( $(date +%s) - t0 )) s, $(stat -c %s media.bin) bytes" > "$here/out/timing.txt"
if [ -n "$SHA1" ]; then
  got=$(sha1sum media.bin | cut -d' ' -f1)
  if [ "$got" != "$SHA1" ]; then
    echo "::error::the download has SHA-1 $got, expected $SHA1"
    rm -f media.bin; finish "not run: the download does not match its published hash"; exit 1
  fi
  echo "SHA-1 matches"
fi
if [ "$TYPE" = iso ]; then
  mkdir -p iso
  sudo mount -o loop,ro -t udf media.bin iso || sudo mount -o loop,ro media.bin iso
  wim=$(find iso -maxdepth 2 \( -ipath 'iso/sources/install.wim' -o -ipath 'iso/sources/install.esd' \) | head -1)
  first=1
else
  wim=media.bin
  first=4
fi
wimlib-imagex info "$wim" | grep -E '^(Index|Name):' | paste - - | sed 's/[[:space:]]\+/ /g' > images.txt
cat images.txt
index=$(awk -v f="$first" -v re="$(echo "$EDITION" | tr 'A-Z' 'a-z')" '{ i = $2; sub(/^Index: [0-9]+ Name: /, ""); if (i >= f && tolower($0) ~ re) { print i; exit } }' images.txt)
if [ -z "$index" ]; then
  echo "::error::no image of the medium matches '$EDITION'"
  finish "not run: the medium has no image matching the requested edition"; exit 1
fi
echo "installing image $index"
wimlib-imagex extract "$wim" "$index" /Windows/System32/ntoskrnl.exe --dest-dir=k --no-acls > /dev/null
ksha=$(sha256sum k/ntoskrnl.exe | cut -d' ' -f1); rm -rf k
verified=$(awk -F'\t' -v a="$ARCH" -v h="$ksha" '$1 == a && $3 == h {print $2}' "$here/ci/kernel-hashes.tsv" | head -1)
if [ -n "$verified" ]; then
  echo "kernel verified: ntoskrnl.exe $verified, SHA-256 $ksha"
elif [ -n "$SHA1" ]; then
  echo "the kernel (SHA-256 $ksha) is not in ci/kernel-hashes.tsv; the download matched its published SHA-1"
else
  echo "::error::the kernel of this medium (SHA-256 $ksha) is not in ci/kernel-hashes.tsv and the download has no published hash"
  finish "not run: nothing verifies this medium"; exit 1
fi
echo "::endgroup::"

echo "::group::answer file and disks"
mkdir -p data/probe
cp "$here/probes/$ARCH/"*.exe data/probe/
{
  echo '@echo off'
  echo 'set U=%~dp0'
  if [ "$OVERRIDE" != none ]; then
    # first logon: write the override, restart; the measurement runs at the next logon
    echo 'if exist %U%override.done goto measure'
    echo "reg add HKLM\\SYSTEM\\CurrentControlSet\\Control\\FeatureManagement\\Overrides\\3\\1150694031 /v EnabledState /t REG_DWORD /d $OVERRIDE /f > %U%override.txt 2>&1"
    echo 'reg add HKLM\SYSTEM\CurrentControlSet\Control\FeatureManagement\Overrides\3\1150694031 /v EnabledStateOptions /t REG_DWORD /d 0 /f >> %U%override.txt 2>&1'
    echo 'echo done > %U%override.done'
    echo 'shutdown /r /t 5 /f'
    echo 'exit /b'
    echo ':measure'
  fi
  echo 'ver > %U%out.txt'
  echo '%U%kernel_ids.exe >> %U%out.txt 2>&1'
  echo 'reg query HKLM\SYSTEM\CurrentControlSet\Control\FeatureManagement\Overrides\3\1150694031 >> %U%out.txt 2>&1'
  echo '%U%probe.exe >> %U%out.txt 2>&1'
  echo 'echo PROBE-EXIT %ERRORLEVEL% >> %U%out.txt'
  echo 'shutdown /s /t 5 /f'
} | sed 's/$/\r/' > data/probe/run.cmd
# the removable drive may appear some seconds after the logon
{
  echo '@echo off'
  echo 'for /l %%n in (1,1,120) do ('
  for d in C D E F G H I; do echo "  if exist $d:\\probe\\run.cmd call $d:\\probe\\run.cmd & exit"; done
  echo '  ping -n 4 127.0.0.1 > nul'
  echo ')'
} | sed 's/$/\r/' > data/probe/logon.cmd
key=""
if [ -n "$KEY" ]; then key="<ProductKey><Key>$KEY</Key><WillShowUI>OnError</WillShowUI></ProductKey>"; fi
if [ "$ARCH" = x64 ] && [ "$TYPE" = iso ]; then layout=mbr; else layout=gpt; fi
python3 "$here/ci/answer-file.py" "$here/ci/autounattend.xml" "$ARCH" "$layout" "$index" "$key" "${LEGACY:-}" > data/autounattend.xml
qemu-img create -q -f qcow2 system.qcow2 48G

if [ "$TYPE" = iso ]; then
  sudo umount iso
  truncate -s 64M usb.img
  mformat -i usb.img -F -v REPRO ::
  mcopy -i usb.img -s data/autounattend.xml data/probe ::
  results=usb.img
else
  # the Windows Setup image of the ESD on a bootable FAT disk; it starts Setup with the answer file and the ESD
  mkdir -p tree/sources
  wimlib-imagex extract media.bin 1 /efi /boot/boot.sdi --dest-dir=tree --no-acls --preserve-dir-structure > /dev/null
  wimlib-imagex export media.bin 3 tree/sources/boot.wim --compress=LZX --boot > /dev/null
  printf '[LaunchApps]\r\nX:\\Windows\\System32\\cmd.exe, /c X:\\Windows\\System32\\repro-setup.cmd\r\n' > winpeshl.ini
  {
    echo '@echo off'
    echo 'wpeutil WaitForRemovableStorage'
    for v in BypassTPMCheck BypassSecureBootCheck BypassRAMCheck BypassCPUCheck BypassStorageCheck; do
      echo "reg add HKLM\\SYSTEM\\Setup\\LabConfig /v $v /t REG_DWORD /d 1 /f"
    done
    echo ':again'
    for d in C D E F G H I J K; do echo "if exist $d:\\install.esd set SRC=$d:"; done
    for d in C D E F G H I J K; do echo "if exist $d:\\autounattend.xml set ANS=$d:"; done
    echo 'if not defined SRC goto again'
    echo 'if not defined ANS goto again'
    echo 'X:\sources\setup.exe /unattend:%ANS%\autounattend.xml /installfrom:%SRC%\install.esd'
  } | sed 's/$/\r/' > repro-setup.cmd
  printf 'add winpeshl.ini /Windows/System32/winpeshl.ini\nadd repro-setup.cmd /Windows/System32/repro-setup.cmd\n' \
    | wimlib-imagex update tree/sources/boot.wim 1 > /dev/null
  cp -r data/probe tree/probe
  cp data/autounattend.xml tree/autounattend.xml
  size=$(( $(du -sm tree | cut -f1) + 300 ))
  truncate -s ${size}M pe.img
  printf 'label: dos\nstart=2048, type=c, bootable\n' | sfdisk -q pe.img
  mformat -i pe.img@@1M -F -v REPRO ::
  mcopy -i pe.img@@1M -s tree/* ::
  rm -rf tree
  # the ESD on a UDF image (a file above 4 GiB does not fit a FAT volume)
  mkdir -p src && mv media.bin src/install.esd
  genisoimage -quiet -udf -allow-limited-size -V SOURCE -o source.iso src
  rm -rf src
  results=pe.img@@1M
fi
echo "prepared $(( $(date +%s) - t0 )) s" >> "$here/out/timing.txt"
df -h "$work" | tail -1
echo "::endgroup::"

echo "::group::guest"
if [ "$ARCH" = x64 ] && [ "$TYPE" = iso ]; then
  set -- qemu-system-x86_64 -accel kvm -cpu "$CPU,hv-relaxed,hv-vapic,hv-time,hv-spinlocks=0x1fff" -machine pc -smp 4 -m 8192 -vga std \
    -drive file=system.qcow2,if=ide,index=0,media=disk,cache=unsafe \
    -drive file=media.bin,format=raw,if=ide,index=2,media=cdrom,readonly=on -boot order=cd \
    -usb -device usb-storage,drive=usb,removable=on -drive if=none,id=usb,format=raw,file=usb.img
elif [ "$ARCH" = x64 ]; then
  cp /usr/share/OVMF/OVMF_VARS_4M.fd vars.fd
  set -- qemu-system-x86_64 -accel kvm -cpu "$CPU,hv-relaxed,hv-vapic,hv-time,hv-spinlocks=0x1fff" -machine q35 -smp 4 -m 8192 -vga std \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,file=vars.fd \
    -drive if=none,id=sys,file=system.qcow2,cache=unsafe -device ide-hd,drive=sys,bus=ide.0,bootindex=0 \
    -drive if=none,id=pe,format=raw,file=pe.img -device ide-hd,drive=pe,bus=ide.1,bootindex=1 \
    -drive if=none,id=src,format=raw,media=cdrom,readonly=on,file=source.iso -device ide-cd,drive=src,bus=ide.2
else
  cp /usr/share/AAVMF/AAVMF_VARS.fd vars.fd
  set -- qemu-system-aarch64 -accel tcg,thread=multi -cpu neoverse-v1 -machine virt -smp 4 -m 8192 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd -drive if=pflash,format=raw,file=vars.fd \
    -device ramfb -device qemu-xhci -device usb-kbd -device usb-tablet \
    -drive if=none,id=sys,file=system.qcow2,cache=unsafe -device nvme,drive=sys,serial=repro,bootindex=0
  if [ "$TYPE" = iso ]; then
    set -- "$@" -drive if=none,id=cd,format=raw,media=cdrom,readonly=on,file=media.bin -device usb-storage,drive=cd,bootindex=1 \
      -drive if=none,id=usb,format=raw,file=usb.img -device usb-storage,drive=usb,removable=on
  else
    set -- "$@" -drive if=none,id=pe,format=raw,file=pe.img -device usb-storage,drive=pe,bootindex=1 \
      -drive if=none,id=src,format=raw,media=cdrom,readonly=on,file=source.iso -device usb-storage,drive=src
  fi
fi
status=0
start=$(date +%s)
timeout "$LIMIT" "$@" -rtc base=localtime -display none -serial file:serial.txt -nic none -qmp unix:qmp.sock,server,nowait &
pid=$!
n=0
while kill -0 $pid 2> /dev/null; do
  sleep 30
  n=$((n + 1))
  python3 "$here/ci/screenshot.py" qmp.sock "$here/out/screens/$(printf '%04d' $n).png" 2> /dev/null || true
  ls "$here/out/screens" | head -n -4 | while read -r old; do rm -f "$here/out/screens/$old"; done
done
wait $pid || status=$?
echo "guest $(( $(date +%s) - start )) s, QEMU status $status (124: stopped at the time limit), system disk $(du -sm system.qcow2 | cut -f1) MiB" | tee -a "$here/out/timing.txt"
echo "::endgroup::"

mdir -i "$results" ::probe || true
mcopy -n -i "$results" ::probe/out.txt "$here/out/raw.txt" || true
mcopy -n -i "$results" ::probe/override.txt "$here/out/override.txt" 2> /dev/null || true
rm -f usb.img pe.img source.iso system.qcow2 media.bin
if [ -s "$here/out/raw.txt" ]; then tr -d '\r' < "$here/out/raw.txt" > "$here/out/out.txt"; else echo "no output from the guest" > "$here/out/out.txt"; fi
rm -f "$here/out/raw.txt"
cat "$here/out/out.txt"
if grep -q 'RESULT: the exception context dropped\|^RESULT LOSES' "$here/out/out.txt"; then result="loses state"
elif grep -q '^RESULT: not applicable' "$here/out/out.txt"; then result="not measured: the installed kernel left XSAVE off ($(sed -n 's/^RESULT: not applicable, [^(]*(\(.*\))/\1/p' "$here/out/out.txt" | head -1), CPU model $CPU)"
elif grep -q 'RESULT no-sve' "$here/out/out.txt"; then result="not applicable: this Windows does not report SVE, so a process has no SVE state to lose"
elif grep -q '^RESULT: \|^RESULT keeps' "$here/out/out.txt"; then result="keeps state"
elif [ "$status" = 124 ]; then result="no result: the installation did not finish within $LIMIT seconds"
else result="no result"; fi
finish "$result"
case "$result" in "no result"*) exit 1 ;; esac

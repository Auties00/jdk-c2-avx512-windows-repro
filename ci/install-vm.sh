#!/bin/bash
# Installs Windows from an ISO into a QEMU/KVM guest, unattended, and runs the native reproducer at the first
# logon. Used for the versions whose Windows PE leaves XSAVE off, where only an installed system can be measured.
# x64 only; the guest boots through BIOS from the ISO itself and installs to an empty disk.
#
# Input (environment), one entry of ci/windows-images.json with "mode": "install":
#   NAME, URL, SHA1, CPU, LIMIT   as in winpe-vm.sh
#   INDEX     the image of sources/install.wim to install
#   KEY       a product key for media that ask for one (a generic installation key), or empty
# The medium is checked before it boots: the download against SHA1 when given, and the kernel of the installed
# image against ci/kernel-hashes.tsv. Output: out/out.txt, out/result.txt, out/screens.
set -euo pipefail
: "${SHA1:=}" "${LIMIT:=5400}" "${CPU:=host}" "${INDEX:=1}" "${KEY:=}"
export WIMLIB_IMAGEX_IGNORE_CASE=1
ARCH=x64
work=${WORK:-$PWD/work}; here=$PWD
mkdir -p "$work" out/screens
cd "$work"

finish() {
  local kernel reported cpu
  kernel=$(sed -n 's/^KVER file .*ntoskrnl.exe: [0-9]*\.[0-9]*\.\([0-9]*\.[0-9]*\).*/\1/p' "$here/out/out.txt" 2>/dev/null | head -1)
  reported=$(sed -n 's/^KVER class222 status 0x00000000 .* version \([0-9.]*\).*/\1/p' "$here/out/out.txt" 2>/dev/null | head -1)
  cpu=$(sed -n 's/^cpu: //p' "$here/out/out.txt" 2>/dev/null | head -1)
  if [ -z "$cpu" ]; then cpu=$(lscpu | sed -n 's/^Model name: *//p' | head -1); fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$NAME" "$ARCH" "${kernel:-${verified:-unknown}}" "${reported:-not reported}" \
    "installed system" "absent" "$cpu" "$1" > "$here/out/result.txt"
  cat "$here/out/result.txt"
}

echo "::group::download and verify"
curl -fsSL --retry 5 --retry-delay 10 -o media.iso "$URL"
ls -l media.iso
if [ -n "$SHA1" ]; then
  got=$(sha1sum media.iso | cut -d' ' -f1)
  if [ "$got" != "$SHA1" ]; then
    echo "::error::the download has SHA-1 $got, expected $SHA1"
    rm -f media.iso; finish "not run: the download does not match its published hash"; exit 1
  fi
  echo "SHA-1 matches"
fi
mkdir -p iso
sudo mount -o loop,ro -t udf media.iso iso || sudo mount -o loop,ro media.iso iso
wim=$(find iso -maxdepth 2 \( -ipath 'iso/sources/install.wim' -o -ipath 'iso/sources/install.esd' \) | head -1)
wimlib-imagex info "$wim" | grep -E '^(Index|Name):' | paste - - | head -12
wimlib-imagex extract "$wim" "$INDEX" /Windows/System32/ntoskrnl.exe --dest-dir=k --no-acls > /dev/null
ksha=$(sha256sum k/ntoskrnl.exe | cut -d' ' -f1); rm -rf k
sudo umount iso
verified=$(awk -F'\t' -v h="$ksha" '$1 == "x64" && $3 == h {print $2}' "$here/ci/kernel-hashes.tsv" | head -1)
if [ -n "$verified" ]; then
  echo "kernel verified: ntoskrnl.exe $verified, SHA-256 $ksha"
elif [ -n "$SHA1" ]; then
  echo "the kernel (SHA-256 $ksha) is not in ci/kernel-hashes.tsv; the download matched its published SHA-1"
else
  echo "::error::the kernel of this medium (SHA-256 $ksha) is not in ci/kernel-hashes.tsv and the download has no published hash"
  rm -f media.iso; finish "not run: nothing verifies this medium"; exit 1
fi
echo "::endgroup::"

echo "::group::answer file and disks"
mkdir -p data/probe
cp "$here/probes/x64/"*.exe data/probe/
{
  echo '@echo off'
  echo 'set U=%~dp0'
  echo 'ver > %U%out.txt'
  echo '%U%kernel_ids.exe >> %U%out.txt 2>&1'
  echo '%U%probe.exe >> %U%out.txt 2>&1'
  echo 'echo PROBE-EXIT %ERRORLEVEL% >> %U%out.txt'
} | sed 's/$/\r/' > data/probe/run.cmd
key=""
if [ -n "$KEY" ]; then key="<ProductKey><Key>$KEY</Key><WillShowUI>OnError</WillShowUI></ProductKey>"; fi
sed -e "s|@INDEX@|$INDEX|" -e "s|@KEY@|$key|" "$here/ci/autounattend.xml" | sed 's/$/\r/' > data/autounattend.xml
truncate -s 64M usb.img
mformat -i usb.img -F -v REPRO ::
mcopy -i usb.img -s data/autounattend.xml data/probe ::
qemu-img create -q -f qcow2 system.qcow2 40G
echo "::endgroup::"

echo "::group::guest"
status=0
start=$(date +%s)
timeout "$LIMIT" qemu-system-x86_64 -accel kvm -cpu "$CPU" -machine pc -smp 4 -m 6144 -vga std -rtc base=localtime \
  -drive file=system.qcow2,if=ide,index=0,media=disk,cache=unsafe \
  -drive file=media.iso,if=ide,index=2,media=cdrom,readonly=on -boot order=cd \
  -usb -device usb-storage,drive=usb,removable=on -drive if=none,id=usb,format=raw,file=usb.img \
  -display none -serial file:serial.txt -nic none -qmp unix:qmp.sock,server,nowait &
pid=$!
n=0
while kill -0 $pid 2> /dev/null; do
  sleep 20
  n=$((n + 1))
  python3 "$here/ci/screenshot.py" qmp.sock "$here/out/screens/$(printf '%04d' $n).png" 2> /dev/null || true
  ls "$here/out/screens" | head -n -4 | while read -r old; do rm -f "$here/out/screens/$old"; done
done
wait $pid || status=$?
echo "the guest ran $(( $(date +%s) - start )) seconds, QEMU status $status (124: stopped at the time limit)"
echo "::endgroup::"

mcopy -n -i usb.img ::probe/out.txt "$here/out/raw.txt" || true
rm -f usb.img system.qcow2 media.iso
if [ -s "$here/out/raw.txt" ]; then tr -d '\r' < "$here/out/raw.txt" > "$here/out/out.txt"; else echo "no output from the guest" > "$here/out/out.txt"; fi
rm -f "$here/out/raw.txt"
cat "$here/out/out.txt"
if grep -q 'RESULT: the exception context dropped' "$here/out/out.txt"; then result="loses state"
elif grep -q '^RESULT: not applicable' "$here/out/out.txt"; then result="not measured: the guest kernel left XSAVE off ($(sed -n 's/^RESULT: not applicable, [^(]*(\(.*\))/\1/p' "$here/out/out.txt" | head -1), CPU model $CPU)"
elif grep -q '^RESULT: ' "$here/out/out.txt"; then result="keeps state"
elif [ "$status" = 124 ]; then result="no result: the installation did not finish within $LIMIT seconds"
else result="no result"; fi
finish "$result"
case "$result" in "no result"*) exit 1 ;; esac

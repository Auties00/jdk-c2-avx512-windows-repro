#!/bin/bash
# Boots the Windows PE of a Windows installation medium in QEMU and runs the native reproducer in it, so the
# reproducer meets the kernel of that medium. Nothing is installed: Windows PE runs from a RAM disk.
#
# Input (environment), one entry of ci/windows-images.json:
#   NAME      what the medium is
#   ARCH      x64 (KVM) or arm64 (emulated, with SVE)
#   TYPE      iso: a Windows ISO (sources/boot.wim, last image)
#             esd: an install ESD of the Media Creation Tool kind (image 3; boot files from image 1)
#   URL       where to download it
#   SHA1      expected SHA-1 of the download, or empty when the publisher gives none
#   BOOT      uefi (default) or bios (media older than Windows 8)
#   CPU       the QEMU CPU model of an x64 guest; host (default) passes the runner's CPU through
#   OVERRIDE  none, or 1 / 2: writes FeatureManagement\Overrides\3\1150694031 EnabledState (1 disabled,
#             2 enabled) into the SYSTEM hive of the image; that entry is how an installed Windows Server 2025
#             keeps the fix off
#   LIMIT     seconds the guest may run
# Every medium is checked before it boots: the download against SHA1 when given, and the kernel inside it against
# ci/kernel-hashes.tsv (SHA-256 of ntoskrnl.exe files as the Microsoft symbol server serves them). A medium
# boots when its kernel is in that list, or when its download matched SHA1.
# Output: out/out.txt (what the guest printed), out/result.txt (one line), out/screens (last screenshots).
set -euo pipefail
: "${BOOT:=uefi}" "${OVERRIDE:=none}" "${SHA1:=}" "${LIMIT:=1500}" "${CPU:=host}"
export WIMLIB_IMAGEX_IGNORE_CASE=1
work=${WORK:-$PWD/work}; here=$PWD
mkdir -p "$work" out/screens
cd "$work"

finish() {  # finish <result text>: writes the result line
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
    "Windows PE, $([ "$OVERRIDE" = none ] && echo 'feature overrides absent' || echo "override EnabledState=$OVERRIDE")" "${state:-absent}" "$cpu" "$1" > "$here/out/result.txt"
  cat "$here/out/result.txt"
}

echo "::group::download and verify"
# resumable: a server may close a long transfer early
for attempt in 1 2 3 4 5 6 7 8; do
  if curl -fsSL --retry 5 --retry-delay 10 --retry-all-errors -C - -o media.bin "$URL"; then break; fi
  echo "download attempt $attempt stopped at $(stat -c %s media.bin 2> /dev/null || echo 0) bytes"
  sleep 15
done
ls -l media.bin
if [ -n "$SHA1" ]; then
  got=$(sha1sum media.bin | cut -d' ' -f1)
  if [ "$got" != "$SHA1" ]; then
    echo "::error::the download has SHA-1 $got, expected $SHA1"
    rm -f media.bin; finish "not run: the download does not match its published hash"; exit 1
  fi
  echo "SHA-1 matches"
fi
echo "::endgroup::"

echo "::group::boot files"
mkdir -p tree/sources tree/boot
if [ "$TYPE" = iso ]; then
  mkdir -p iso
  sudo mount -o loop,ro -t udf media.bin iso || sudo mount -o loop,ro media.bin iso
  wim=$(find iso -maxdepth 2 -ipath 'iso/sources/boot.wim' | head -1)
  cp "$wim" tree/sources/boot.wim
  cp "$(find iso -maxdepth 2 -ipath 'iso/boot/boot.sdi' | head -1)" tree/boot/boot.sdi
  if [ "$BOOT" = bios ]; then
    cp "$(find iso -maxdepth 1 -iname bootmgr | head -1)" tree/bootmgr
    cp "$(find iso -maxdepth 2 -ipath 'iso/boot/bcd' | head -1)" tree/boot/bcd
    cp "$(find iso -maxdepth 2 -ipath 'iso/boot/etfsboot.com' | head -1)" tree/boot/etfsboot.com
  else
    cp -r "$(find iso -maxdepth 1 -iname efi | head -1)" tree/efi
  fi
  sudo umount iso
  rm -f media.bin
  chmod -R u+w tree
  image=$(wimlib-imagex info tree/sources/boot.wim | awk '/^Image Count:/ {print $3}')
else
  wimlib-imagex extract media.bin 1 /efi /boot/boot.sdi --dest-dir=tree --no-acls --preserve-dir-structure > /dev/null
  wimlib-imagex export media.bin 3 tree/sources/boot.wim --compress=LZX > /dev/null
  rm -f media.bin
  image=1
fi
wimlib-imagex info tree/sources/boot.wim "$image" --boot > /dev/null

wimlib-imagex extract tree/sources/boot.wim "$image" /Windows/System32/ntoskrnl.exe --dest-dir=k --no-acls > /dev/null
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

printf '[LaunchApps]\r\nX:\\Windows\\System32\\cmd.exe, /c X:\\Windows\\System32\\repro-start.cmd\r\n' > winpeshl.ini
{
  echo '@echo off'
  echo ':again'
  echo 'wpeutil WaitForRemovableStorage'
  for d in C D E F G H I J K; do echo "if exist $d:\\probe\\run.cmd call $d:\\probe\\run.cmd & goto done"; done
  echo 'goto again'
  echo ':done'
  echo 'wpeutil shutdown'
} | sed 's/$/\r/' > repro-start.cmd
{
  echo 'add winpeshl.ini /Windows/System32/winpeshl.ini'
  echo 'add repro-start.cmd /Windows/System32/repro-start.cmd'
} > update.txt
if [ "$OVERRIDE" != none ]; then
  wimlib-imagex extract tree/sources/boot.wim "$image" /Windows/System32/config/SYSTEM --dest-dir=. --no-acls > /dev/null
  {
    echo 'Windows Registry Editor Version 5.00'
    echo
    echo '[HKEY_LOCAL_MACHINE\SYSTEM\ControlSet001\Control\FeatureManagement\Overrides\3\1150694031]'
    echo "\"EnabledState\"=dword:0000000$OVERRIDE"
    echo '"EnabledStateOptions"=dword:00000000'
  } > override.reg
  hivexregedit --merge --prefix 'HKEY_LOCAL_MACHINE\SYSTEM' SYSTEM override.reg
  echo 'add SYSTEM /Windows/System32/config/SYSTEM' >> update.txt
fi
wimlib-imagex update tree/sources/boot.wim "$image" < update.txt > /dev/null
ls -l tree/sources/boot.wim
echo "::endgroup::"

echo "::group::disk images"
mkdir -p data/probe
cp "$here/probes/$ARCH/"*.exe data/probe/
{
  echo '@echo off'
  echo 'set U=%~dp0'
  echo 'ver > %U%out.txt'
  echo '%U%kernel_ids.exe >> %U%out.txt 2>&1'
  echo 'reg query HKLM\SYSTEM\CurrentControlSet\Control\FeatureManagement\Overrides\3\1150694031 >> %U%out.txt 2>&1'
  echo '%U%probe.exe >> %U%out.txt 2>&1'
  echo 'echo PROBE-EXIT %ERRORLEVEL% >> %U%out.txt'
} | sed 's/$/\r/' > data/probe/run.cmd
if [ "$BOOT" = bios ]; then
  # BIOS media boot from a CD image (El Torito, the medium's own boot sector); the probes go on a second disk
  genisoimage -quiet -b boot/etfsboot.com -no-emul-boot -boot-load-size 8 -iso-level 2 -udf -J -l -D -N \
    -relaxed-filenames -allow-limited-size -o boot.iso tree
  size=64
else
  cp -r data/probe tree/probe
  size=$(( $(du -sm tree | cut -f1) + 300 ))
fi
truncate -s ${size}M disk.img
printf 'label: dos\nstart=2048, type=c, bootable\n' | sfdisk -q disk.img
mformat -i disk.img@@1M -F -v REPRO ::
if [ "$BOOT" = bios ]; then mcopy -i disk.img@@1M -s data/probe ::; else mcopy -i disk.img@@1M -s tree/* ::; fi
rm -rf tree data
echo "::endgroup::"

echo "::group::guest"
if [ "$ARCH" = x64 ] && [ "$BOOT" = bios ]; then
  set -- qemu-system-x86_64 -accel kvm -cpu "$CPU" -machine pc -smp 4 -m 6144 -vga std \
    -drive file=boot.iso,media=cdrom,readonly=on,if=ide,index=2 -drive file=disk.img,format=raw,if=ide,index=0 -boot d
elif [ "$ARCH" = x64 ]; then
  cp /usr/share/OVMF/OVMF_VARS_4M.fd vars.fd
  set -- qemu-system-x86_64 -accel kvm -cpu "$CPU" -machine q35 -smp 4 -m 6144 -vga std \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,file=vars.fd \
    -drive if=none,id=d0,format=raw,file=disk.img -device ide-hd,drive=d0,bootindex=0
else
  cp /usr/share/AAVMF/AAVMF_VARS.fd vars.fd
  set -- qemu-system-aarch64 -accel tcg,thread=multi -cpu neoverse-v1 -machine virt -smp 4 -m 6144 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd -drive if=pflash,format=raw,file=vars.fd \
    -device ramfb -device qemu-xhci -device usb-kbd \
    -drive if=none,id=d0,format=raw,file=disk.img -device usb-storage,drive=d0,bootindex=0
fi
status=0
start=$(date +%s)
timeout "$LIMIT" "$@" -display none -serial file:serial.txt -nic none -no-reboot -qmp unix:qmp.sock,server,nowait &
pid=$!
n=0
while kill -0 $pid 2> /dev/null; do
  sleep 8
  n=$((n + 1))
  timeout 25 python3 "$here/ci/screenshot.py" qmp.sock "$here/out/screens/$(printf '%04d' $n).png" 2> /dev/null || true
  ls "$here/out/screens" | head -n -4 | while read -r old; do rm -f "$here/out/screens/$old"; done
done
wait $pid || status=$?
echo "the guest ran $(( $(date +%s) - start )) seconds, QEMU status $status (124: stopped at the time limit)"
tail -c 1500 serial.txt 2> /dev/null | tr -d '\033' || true
echo "::endgroup::"

mdir -i disk.img@@1M ::probe || true
mcopy -n -i disk.img@@1M ::probe/out.txt "$here/out/raw.txt" || true
rm -f disk.img boot.iso
if [ -s "$here/out/raw.txt" ]; then tr -d '\r' < "$here/out/raw.txt" > "$here/out/out.txt"; else echo "no output from the guest" > "$here/out/out.txt"; fi
rm -f "$here/out/raw.txt"
cat "$here/out/out.txt"
if grep -q 'RESULT: the exception context dropped\|^RESULT LOSES' "$here/out/out.txt"; then result="loses state"
elif grep -q '^RESULT: not applicable' "$here/out/out.txt"; then result="not measured: the kernel left XSAVE off in Windows PE ($(sed -n 's/^RESULT: not applicable, [^(]*(\(.*\))/\1/p' "$here/out/out.txt" | head -1), CPU model $CPU)"
elif grep -q 'RESULT no-sve' "$here/out/out.txt"; then result="not applicable: this Windows does not report SVE, so a process has no SVE state to lose"
elif grep -q '^RESULT: \|^RESULT keeps' "$here/out/out.txt"; then result="keeps state"
elif [ "$status" = 124 ]; then result="no result: the guest did not finish within $LIMIT seconds"
else result="no result"; fi
finish "$result"
case "$result" in "no result"*) exit 1 ;; esac

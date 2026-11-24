#!/bin/bash
# Boots the Windows PE of a Microsoft installation medium in QEMU and runs the native reproducer in it, so the
# reproducer meets the kernel of that medium. Nothing is installed: Windows PE runs from a RAM disk.
#
# Usage: winpe-vm.sh <x64|arm64> <iso|esd> <url> <label> <override: none|1|2> <timeout seconds>
#   iso: a Windows ISO (sources/boot.wim, image 2). esd: a Media Creation Tool install ESD (image 3, boot files
#   from image 1).
#   override: writes FeatureManagement\Overrides\3\1150694031 EnabledState (1 disabled, 2 enabled) into the
#   SYSTEM hive of the image; that entry is how an installed Windows Server 2025 keeps the fix off.
# Needs: p7zip-full wimtools mtools dosfstools libhivex-bin libwin-hivex-perl, QEMU and UEFI firmware, and the
# probes in probes/<arch>/ (kernel_ids.exe, probe.exe). Writes out/out.txt and out/result.txt.
set -euo pipefail
arch=$1; kind=$2; url=$3; label=$4; override=$5; limit=$6
work=${WORK:-$PWD/work}; mkdir -p "$work" out; here=$PWD
cd "$work"

echo "::group::download"
curl -fsSL --retry 5 --retry-delay 10 -o media.bin "$url"
ls -l media.bin
echo "::endgroup::"

echo "::group::boot files"
mkdir -p tree/sources
if [ "$kind" = iso ]; then
  7z x -y -ssc- -otree media.bin 'efi' 'boot/boot.sdi' 'sources/boot.wim' > /dev/null
  rm -f media.bin
  # some media spell these names in upper case
  find tree -mindepth 1 -depth | while read -r f; do l="$(dirname "$f")/$(basename "$f" | tr A-Z a-z)"; [ "$f" = "$l" ] || mv "$f" "$l"; done
  image=$(wimlib-imagex info tree/sources/boot.wim | awk '/^Image Count:/ {print $3}')
else
  wimlib-imagex extract media.bin 1 /efi /boot/boot.sdi --dest-dir=tree --no-acls > /dev/null
  wimlib-imagex export media.bin 3 tree/sources/boot.wim --compress=LZX > /dev/null
  rm -f media.bin
  image=1
fi
wimlib-imagex info tree/sources/boot.wim "$image" --boot > /dev/null
printf '[LaunchApps]\r\n%%SYSTEMROOT%%\System32\cmd.exe, /c %%SYSTEMROOT%%\System32\repro-start.cmd\r\n' > winpeshl.ini
cat > repro-start.cmd <<'CMD'
@echo off
:again
wpeutil WaitForRemovableStorage
for %%i in (C D E F G H I J K) do if exist %%i:\probe\run.cmd goto found%%i
goto again
:foundC
call C:\probe\run.cmd
goto done
:foundD
call D:\probe\run.cmd
goto done
:foundE
call E:\probe\run.cmd
goto done
:foundF
call F:\probe\run.cmd
goto done
:foundG
call G:\probe\run.cmd
goto done
:foundH
call H:\probe\run.cmd
goto done
:foundI
call I:\probe\run.cmd
goto done
:foundJ
call J:\probe\run.cmd
goto done
:foundK
call K:\probe\run.cmd
:done
wpeutil shutdown
CMD
sed -i 's/$/\r/' repro-start.cmd
update="add winpeshl.ini /Windows/System32/winpeshl.ini
add repro-start.cmd /Windows/System32/repro-start.cmd"
if [ "$override" != none ]; then
  wimlib-imagex extract tree/sources/boot.wim "$image" /Windows/System32/config/SYSTEM --dest-dir=. --no-acls > /dev/null
  cat > override.reg <<REG
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SYSTEM\ControlSet001\Control\FeatureManagement\Overrides\3\1150694031]
"EnabledState"=dword:0000000$override
"EnabledStateOptions"=dword:00000000
REG
  hivexregedit --merge --prefix 'HKEY_LOCAL_MACHINE\SYSTEM' SYSTEM override.reg
  update="$update
add SYSTEM /Windows/System32/config/SYSTEM"
fi
echo "$update" | wimlib-imagex update tree/sources/boot.wim "$image" > /dev/null
ls -l tree/sources/boot.wim
echo "::endgroup::"

echo "::group::disk image"
mkdir -p tree/probe
cp "$here/probes/$arch/"*.exe tree/probe/
cat > tree/probe/run.cmd <<'CMD'
@echo off
set U=%~dp0
ver > %U%out.txt
%U%kernel_ids.exe >> %U%out.txt 2>&1
reg query HKLM\SYSTEM\CurrentControlSet\Control\FeatureManagement\Overrides\3\1150694031 >> %U%out.txt 2>&1
%U%probe.exe >> %U%out.txt 2>&1
echo PROBE-EXIT %ERRORLEVEL% >> %U%out.txt
CMD
sed -i 's/$/\r/' tree/probe/run.cmd
size=$(( $(du -sm tree | cut -f1) + 300 ))
truncate -s ${size}M disk.img
printf 'label: dos\nstart=2048, type=c, bootable\n' | sfdisk -q disk.img
mformat -i disk.img@@1M -F -v REPRO ::
mcopy -i disk.img@@1M -s tree/* ::
rm -rf tree
echo "::endgroup::"

echo "::group::qemu"
if [ "$arch" = x64 ]; then
  cp /usr/share/OVMF/OVMF_VARS_4M.fd vars.fd
  set -- qemu-system-x86_64 -accel kvm -cpu host -machine q35 -smp 4 -m 6144 \
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
timeout "$limit" "$@" -display none -serial file:serial.txt -nic none -no-reboot -qmp unix:qmp.sock,server,nowait &
pid=$!
# a screenshot of the guest every 30 seconds, for diagnosis (the newest three are kept)
mkdir -p "$here/out/screens"
n=0
while kill -0 $pid 2>/dev/null; do
  sleep 30
  n=$((n + 1))
  python3 - "$here/out/screens/$(printf '%04d' $n).png" <<'PY' 2>/dev/null || true
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect('qmp.sock'); f = s.makefile('rw')
f.readline()
for c in ({'execute': 'qmp_capabilities'}, {'execute': 'screendump', 'arguments': {'filename': sys.argv[1], 'format': 'png'}}):
    f.write(json.dumps(c) + chr(10)); f.flush()
    while 'return' not in f.readline():
        pass
PY
  ls "$here/out/screens" | head -n -3 | while read -r old; do rm -f "$here/out/screens/$old"; done
done
wait $pid || status=$?
echo "guest ran $(( $(date +%s) - start )) seconds"
echo "qemu exit status $status (124: timed out)"
tail -c 2000 serial.txt 2>/dev/null | tr -d '\033' || true
echo "::endgroup::"

mdir -i disk.img@@1M ::probe || true
mcopy -n -i disk.img@@1M ::probe/out.txt "$here/out/out.txt" || echo "no output from the guest (qemu status $status)" > "$here/out/out.txt"
rm -f disk.img
cd "$here"
tr -d '\r' < out/out.txt > out/o.txt && mv out/o.txt out/out.txt
cat out/out.txt
kernel=$(sed -n 's/^KVER file .*ntoskrnl.exe: [0-9]*\.[0-9]*\.\([0-9]*\.[0-9]*\).*/\1/p' out/out.txt | head -1)
reported=$(sed -n 's/^KVER class222 status 0x00000000 .* version \([0-9.]*\).*/\1/p' out/out.txt | head -1)
state=$(sed -n 's/^ *EnabledState *REG_DWORD *0x\([0-9]\).*/\1/p' out/out.txt | head -1)
cpu=$(sed -n 's/^cpu: //p' out/out.txt | head -1)
if grep -q 'RESULT: the exception context dropped\|^RESULT LOSES' out/out.txt; then result="loses state"
elif grep -q '^RESULT: \|^RESULT keeps' out/out.txt; then result="keeps state"
elif grep -q 'no AVX on this host\|RESULT no-sve' out/out.txt; then result="not measurable: the kernel does not enable the extended state here"
else result="no result"; fi
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$arch" "${kernel:-unknown}" "${reported:-not reported}" \
  "$([ "$override" = none ] && echo 'no override' || echo "override EnabledState=$override")" "${state:-absent}" "${cpu:-$(lscpu | sed -n 's/^Model name: *//p' | head -1) (emulated: QEMU neoverse-v1)}" "$result" > out/result.txt
cat out/result.txt
[ "$result" != "no result" ]

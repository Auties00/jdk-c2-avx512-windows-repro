#!/bin/bash
# Installs Windows from a medium into a QEMU guest and runs the native reproducer on the installed system.
# An installed system is what a user runs: unlike the Windows PE of the medium it has the feature overrides the
# edition ships with, the kernel of the install image, and XSAVE on in every version. The guest has no network,
# so nothing can replace the kernel after the installation.
#
# How: the Windows PE of the medium boots from a small disk and, instead of the interactive Setup, applies the
# chosen image to the empty system disk with DISM, makes it bootable and restarts. The image then goes through
# its normal first boot (specialize, out-of-box) with an answer file, and SetupComplete.cmd, which Windows runs
# when Setup has finished, runs the reproducer and powers off. The result is read from the guest's disk.
#
# Input (environment), one entry of ci/windows-images.json:
#   NAME      what the medium is
#   ARCH      x64 (KVM) or arm64 (emulated, with SVE)
#   TYPE      iso: a Windows ISO (sources/boot.wim, sources/install.wim or .esd)
#             esd: an install ESD of the Media Creation Tool kind (image 3 is its Windows PE, 4 and up the editions)
#   URL, SHA1 where to download it; the SHA-1 published for it, or empty
#   EDITION   a regular expression matched against the image names; the first match is installed
#   CPU       the QEMU CPU model of an x64 guest (host: the runner's CPU)
#   OVERRIDE  none, or 1 / 2: writes FeatureManagement\Overrides\3\1150694031 EnabledState into the installed
#             system's registry before its first boot (1 disabled, 2 enabled)
#   LIMIT     seconds the guest may run
# The medium is checked before it boots: the download against SHA1 when given, and the kernel of the image to
# install against ci/kernel-hashes.tsv; one of the two must hold.
# Output: out/out.txt, out/result.txt (one line), out/screens (screenshots), out/timing.txt.
set -euo pipefail
: "${SHA1:=}" "${LIMIT:=7200}" "${CPU:=host}" "${EDITION:=.}" "${OVERRIDE:=none}"
export WIMLIB_IMAGEX_IGNORE_CASE=1
work=${WORK:-$PWD/work}; here=$PWD
mkdir -p "$work" out/screens
cd "$work"
t0=$(date +%s)
verified=

finish() {
  local kernel reported state cpu
  [ -f "$here/out/out.txt" ] || : > "$here/out/out.txt"
  kernel=$(sed -n 's/^KVER file .*ntoskrnl.exe: [0-9]*\.[0-9]*\.\([0-9]*\.[0-9]*\).*/\1/p' "$here/out/out.txt" | head -1)
  reported=$(sed -n 's/^KVER class222 status 0x00000000 .* version \([0-9.]*\).*/\1/p' "$here/out/out.txt" | head -1)
  state=$(sed -n 's/^ *EnabledState *REG_DWORD *0x\([0-9]\).*/\1/p' "$here/out/out.txt" | head -1)
  cpu=$(sed -n 's/^cpu: //p' "$here/out/out.txt" | head -1)
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
mkdir -p tree/sources tree/boot
if [ "$TYPE" = iso ]; then
  mkdir -p iso
  sudo mount -o loop,ro -t udf media.bin iso || sudo mount -o loop,ro media.bin iso
  wim=$(find iso -maxdepth 2 \( -ipath 'iso/sources/install.wim' -o -ipath 'iso/sources/install.esd' \) | head -1)
  source="\\sources\\$(basename "$wim")"
  first=1
else
  wim=media.bin
  source='\install.esd'
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

echo "::group::disks"
# the Windows PE of the medium, with the installer script as its shell
if [ "$TYPE" = iso ]; then
  cp "$(find iso -maxdepth 2 -ipath 'iso/sources/boot.wim' | head -1)" tree/sources/boot.wim
  cp "$(find iso -maxdepth 2 -ipath 'iso/boot/boot.sdi' | head -1)" tree/boot/boot.sdi
  cp -r "$(find iso -maxdepth 1 -iname efi | head -1)" tree/efi
  sudo umount iso
  chmod -R u+w tree
  image=$(wimlib-imagex info tree/sources/boot.wim | awk '/^Image Count:/ {print $3}')
  wimlib-imagex info tree/sources/boot.wim "$image" --boot > /dev/null
else
  wimlib-imagex extract media.bin 1 /efi /boot/boot.sdi --dest-dir=tree --no-acls --preserve-dir-structure > /dev/null
  wimlib-imagex export media.bin 3 tree/sources/boot.wim --compress=LZX --boot > /dev/null
  image=1
fi
printf '[LaunchApps]\r\nX:\\Windows\\System32\\cmd.exe, /c X:\\Windows\\System32\\repro-install.cmd\r\n' > winpeshl.ini
python3 "$here/ci/guest-scripts.py" "$ARCH" "$index" "$source" "$OVERRIDE" "$here/ci/unattend.xml" guest
printf 'add winpeshl.ini /Windows/System32/winpeshl.ini\nadd guest/repro-install.cmd /Windows/System32/repro-install.cmd\n' \
  | wimlib-imagex update tree/sources/boot.wim "$image" > /dev/null
mkdir -p tree/repro/probe
cp "$here/probes/$ARCH/"*.exe tree/repro/probe/
cp guest/unattend.xml guest/SetupComplete.cmd guest/disk.txt tree/repro/
size=$(( $(du -sm tree | cut -f1) + 300 ))
truncate -s ${size}M pe.img
printf 'label: dos\nstart=2048, type=c, bootable\n' | sfdisk -q pe.img
mformat -i pe.img@@1M -F -v REPRO ::
mcopy -i pe.img@@1M -s tree/* ::
rm -rf tree
if [ "$TYPE" = esd ]; then
  # the ESD on a UDF image (a file above 4 GiB does not fit a FAT volume)
  mkdir -p src && mv media.bin src/install.esd
  genisoimage -quiet -udf -allow-limited-size -V SOURCE -o media.bin src
  rm -rf src
fi
qemu-img create -q -f qcow2 system.qcow2 48G
echo "prepared $(( $(date +%s) - t0 )) s" >> "$here/out/timing.txt"
df -h "$work" | tail -1
echo "::endgroup::"

echo "::group::guest"
if [ "$ARCH" = x64 ]; then
  cp /usr/share/OVMF/OVMF_VARS_4M.fd vars.fd
  set -- qemu-system-x86_64 -accel kvm -cpu "$CPU,hv-relaxed,hv-vapic,hv-time,hv-spinlocks=0x1fff" -machine q35 -smp 4 -m 8192 -vga std \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,file=vars.fd \
    -drive if=none,id=sys,file=system.qcow2,cache=unsafe -device ide-hd,drive=sys,bus=ide.0,bootindex=0 \
    -drive if=none,id=pe,format=raw,file=pe.img -device ide-hd,drive=pe,bus=ide.1,bootindex=1 \
    -drive if=none,id=src,format=raw,media=cdrom,readonly=on,file=media.bin -device ide-cd,drive=src,bus=ide.2
else
  cp /usr/share/AAVMF/AAVMF_VARS.fd vars.fd
  set -- qemu-system-aarch64 -accel tcg,thread=multi -cpu neoverse-v1 -machine virt -smp 4 -m 8192 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd -drive if=pflash,format=raw,file=vars.fd \
    -device ramfb -device qemu-xhci -device usb-kbd -device usb-tablet \
    -drive if=none,id=sys,file=system.qcow2,cache=unsafe -device nvme,drive=sys,serial=repro,bootindex=0 \
    -drive if=none,id=pe,format=raw,file=pe.img -device usb-storage,drive=pe,bootindex=1 \
    -drive if=none,id=src,format=raw,media=cdrom,readonly=on,file=media.bin -device usb-storage,drive=src
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
  # one screenshot in ten is kept for the history, plus the last four
  ls "$here/out/screens" | head -n -4 | { grep -v '0\.png$' || true; } | while read -r old; do rm -f "$here/out/screens/$old"; done
done
wait $pid || status=$?
echo "guest $(( $(date +%s) - start )) s, QEMU status $status (124: stopped at the time limit), system disk $(du -sm system.qcow2 | cut -f1) MiB" | tee -a "$here/out/timing.txt"
echo "::endgroup::"

echo "::group::result"
# the installer's log is on the Windows PE disk, the reproducer's output on the system disk
mcopy -n -i pe.img@@1M ::repro/install.log install.log 2> /dev/null && tr -d '\r' < install.log | tail -n 25 || echo "no installer log"
sudo modprobe nbd max_part=8
sudo qemu-nbd --read-only -c /dev/nbd0 system.qcow2
sleep 2
mkdir -p sys
for part in /dev/nbd0p3 /dev/nbd0p2 /dev/nbd0p1; do
  if sudo mount -o ro "$part" sys 2> /dev/null || sudo mount -t ntfs3 -o ro "$part" sys 2> /dev/null; then
    if [ -d sys/repro ]; then break; fi
    sudo umount sys
  fi
done
if [ -f sys/repro/out.txt ]; then tr -d '\r' < sys/repro/out.txt > "$here/out/out.txt"; else echo "no output from the guest" > "$here/out/out.txt"; fi
[ -f sys/repro/setupcomplete.log ] && tr -d '\r' < sys/repro/setupcomplete.log | tail -n 10 || true
sudo umount sys 2> /dev/null || true
sudo qemu-nbd -d /dev/nbd0 > /dev/null
rm -f pe.img system.qcow2 media.bin
cat "$here/out/out.txt"
echo "::endgroup::"
if grep -q 'RESULT: the exception context dropped\|^RESULT LOSES' "$here/out/out.txt"; then result="loses state"
elif grep -q '^RESULT: not applicable' "$here/out/out.txt"; then result="not measured: the installed kernel left XSAVE off ($(sed -n 's/^RESULT: not applicable, [^(]*(\(.*\))/\1/p' "$here/out/out.txt" | head -1), CPU model $CPU)"
elif grep -q 'RESULT no-sve' "$here/out/out.txt"; then result="not applicable: this Windows does not report SVE, so a process has no SVE state to lose"
elif grep -q '^RESULT: \|^RESULT keeps' "$here/out/out.txt"; then result="keeps state"
elif [ "$status" = 124 ]; then result="no result: the installation did not finish within $LIMIT seconds"
else result="no result"; fi
finish "$result"
case "$result" in "no result"*) exit 1 ;; esac

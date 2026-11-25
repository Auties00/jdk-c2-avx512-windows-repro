#!/usr/bin/env python3
"""Writes the scripts that run inside the guest of ci/install-vm.sh.

Usage: guest-scripts.py <x64|arm64> <image index> <source path on the medium> <override: none|1|2> <unattend template> <out dir>
Writes, with CRLF line ends:
  repro-install.cmd  the shell of the medium's Windows PE: applies the image to disk 0 and restarts
  disk.txt           the diskpart script (GPT: EFI system partition, reserved partition, Windows)
  unattend.xml       the answer file of the image's first boot
  SetupComplete.cmd  run by Windows when Setup has finished: runs the reproducer and powers off
MIT License.
"""
import os
import sys

arch, index, source, override, template, out = sys.argv[1:7]
B = "\\"
KEY = r"HKLM\OFF\ControlSet001\Control\FeatureManagement\Overrides\3\1150694031"
LIVE = r"HKLM\SYSTEM\CurrentControlSet\Control\FeatureManagement\Overrides\3\1150694031"
DRIVES = "C D E F G H I J K"


def write(name, lines):
    with open(os.path.join(out, name), "w", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")


os.makedirs(out, exist_ok=True)
install = [
    "@echo off",
    "wpeutil WaitForRemovableStorage",
    ":find",
    "set R=",
    "set S=",
    "for %%%%i in (%s) do if exist %%%%i:\\repro\\disk.txt set R=%%%%i:\\repro" % DRIVES,
    "for %%%%i in (%s) do if exist %%%%i:%s set S=%%%%i:%s" % (DRIVES, source, source),
    "if not defined R goto find",
    "if not defined S goto find",
    "set L=%R%\\install.log",
    "echo image %s of %%S%% > %%L%%" % index,
    "diskpart /s %R%\\disk.txt >> %L% 2>&1",
    "dism /apply-image /imagefile:%%S%% /index:%s /applydir:W:\\ >> %%L%% 2>&1" % index,
    "echo dism exit %ERRORLEVEL% >> %L%",
    "bcdboot W:\\Windows /s S: /f UEFI >> %L% 2>&1",
    "echo bcdboot exit %ERRORLEVEL% >> %L%",
    "mkdir W:\\Windows\\Panther",
    "copy /y %R%\\unattend.xml W:\\Windows\\Panther\\unattend.xml >> %L% 2>&1",
    "mkdir W:\\Windows\\Setup\\Scripts",
    "copy /y %R%\\SetupComplete.cmd W:\\Windows\\Setup\\Scripts\\SetupComplete.cmd >> %L% 2>&1",
    "mkdir W:\\repro",
    "copy /y %R%\\probe\\*.exe W:\\repro\\ >> %L% 2>&1",
]
if override != "none":
    install += [
        "reg load HKLM\\OFF W:\\Windows\\System32\\config\\SYSTEM >> %L% 2>&1",
        "reg add %s /v EnabledState /t REG_DWORD /d %s /f >> %%L%% 2>&1" % (KEY, override),
        "reg add %s /v EnabledStateOptions /t REG_DWORD /d 0 /f >> %%L%% 2>&1" % KEY,
        "reg unload HKLM\\OFF >> %L% 2>&1",
    ]
install += ["echo installed >> %L%", "wpeutil reboot"]
write("repro-install.cmd", install)

write("disk.txt", [
    "select disk 0", "clean", "convert gpt",
    "create partition efi size=260", "format fs=fat32 quick", "assign letter=S",
    "create partition msr size=16",
    "create partition primary", "format fs=ntfs quick", "assign letter=W",
])

write("SetupComplete.cmd", [
    "@echo off",
    "set O=C:\\repro\\out.txt",
    "echo SetupComplete started > C:\\repro\\setupcomplete.log",
    "powercfg /h off >> C:\\repro\\setupcomplete.log 2>&1",
    "ver > %O%",
    "C:\\repro\\kernel_ids.exe >> %O% 2>&1",
    "reg query %s >> %%O%% 2>&1" % LIVE,
    "C:\\repro\\probe.exe >> %O% 2>&1",
    "echo PROBE-EXIT %ERRORLEVEL% >> %O%",
    "echo SetupComplete finished >> C:\\repro\\setupcomplete.log",
    "shutdown /s /f /t 10",
])

text = open(template, encoding="utf-8").read().replace("@ARCH@", "amd64" if arch == "x64" else "arm64")
with open(os.path.join(out, "unattend.xml"), "w", encoding="utf-8", newline="\r\n") as f:
    f.write(text)

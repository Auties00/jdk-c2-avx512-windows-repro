#!/usr/bin/env python3
"""Prints which Windows kernel this machine has: the OS build from the registry, and for ntoskrnl.exe its file
version and its symbol-server keys (image: timestamp + SizeOfImage; PDB: name, GUID + age).

The OS revision (UBR) and the kernel's file revision can differ: an update that does not rebuild the kernel keeps
the previous one. The file version is the revision that decides whether a kernel is affected.

Usage: python kernel_ids.py [path to ntoskrnl.exe]. MIT License.
"""
import os
import struct
import sys
import winreg


def os_build():
    key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion")
    value = lambda name: winreg.QueryValueEx(key, name)[0]
    return "%s %s, OS build %s.%s" % (value("ProductName"), value("DisplayVersion"), value("CurrentBuild"), value("UBR"))


def image_ids(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    sections = struct.unpack_from("<H", d, pe + 6)[0]
    timestamp = struct.unpack_from("<I", d, pe + 8)[0]
    optional_size = struct.unpack_from("<H", d, pe + 20)[0]
    optional = pe + 24
    image_size = struct.unpack_from("<I", d, optional + 56)[0]
    debug_rva, debug_size = struct.unpack_from("<II", d, optional + 112 + 6 * 8)   # data directory 6: debug
    table = [struct.unpack_from("<8sIIII", d, optional + optional_size + 40 * i) for i in range(sections)]

    def offset(rva):
        for _, virtual_size, address, raw_size, raw in table:
            if address <= rva < address + max(virtual_size, raw_size):
                return rva - address + raw
        raise ValueError("RVA %#x outside every section" % rva)

    # VS_FIXEDFILEINFO starts with the signature 0xFEEF04BD; the file version's low dword is build.revision
    fixed = d.find(struct.pack("<I", 0xFEEF04BD))
    version = "unknown"
    if fixed >= 0:
        ls = struct.unpack_from("<I", d, fixed + 12)[0]
        version = "%d.%d" % (ls >> 16, ls & 0xFFFF)

    pdb = "no CodeView record"
    base = offset(debug_rva)
    for i in range(debug_size // 28):
        kind, _, _, raw = struct.unpack_from("<IIII", d, base + 28 * i + 12)
        if kind == 2 and d[raw:raw + 4] == b"RSDS":                                 # IMAGE_DEBUG_TYPE_CODEVIEW
            guid = d[raw + 4:raw + 20]
            age = struct.unpack_from("<I", d, raw + 20)[0]
            name = d[raw + 24:d.index(b"\0", raw + 24)].decode()
            a, b, c = struct.unpack_from("<IHH", guid, 0)
            pdb = "%s %08X%04X%04X%s%x" % (name, a, b, c, guid[8:].hex().upper(), age)
    return version, "%08X%x" % (timestamp, image_size), pdb


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ["SystemRoot"], "System32", "ntoskrnl.exe")
    print(os_build())
    version, image_key, pdb = image_ids(path)
    print("ntoskrnl.exe file version 10.0.%s" % version)
    print("ntoskrnl.exe symbol-server keys: image %s, pdb %s" % (image_key, pdb))


if __name__ == "__main__":
    main()

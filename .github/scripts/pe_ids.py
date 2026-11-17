#!/usr/bin/env python3
"""Prints the symbol-server identifiers (timestamp+size, PDB GUID+age) of system binaries. MIT License."""
import struct
import sys


def ids(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    ts = struct.unpack_from('<I', d, pe + 8)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    size = struct.unpack_from('<I', d, opt + 56)[0]
    dbg_rva, dbg_size = struct.unpack_from('<II', d, opt + 112 + 6 * 8)
    secs = [struct.unpack_from('<8sIIII', d, opt + optsz + 40 * i) for i in range(nsec)]

    def off(rva):
        for _, vsz, va, rsz, raw in secs:
            if va <= rva < va + max(vsz, rsz):
                return rva - va + raw
        return None
    out = '%s %08X%x' % (path, ts, size)
    o = off(dbg_rva)
    for i in range(dbg_size // 28):
        typ, sz, _, raw = struct.unpack_from('<IIII', d, o + 28 * i + 12)
        if typ == 2 and d[raw:raw + 4] == b'RSDS':
            g = d[raw + 4:raw + 20]
            age = struct.unpack_from('<I', d, raw + 20)[0]
            name = d[raw + 24:d.index(b'\0', raw + 24)].decode()
            a, b, c = struct.unpack_from('<IHH', g, 0)
            out += ' %s %08X%04X%04X%s%x' % (name, a, b, c, g[8:].hex().upper(), age)
    print(out)


for p in sys.argv[1:]:
    try:
        ids(p)
    except Exception as e:  # noqa
        print(p, 'error', e)

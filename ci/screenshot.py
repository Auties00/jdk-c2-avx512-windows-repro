#!/usr/bin/env python3
"""Saves a screenshot of a QEMU guest through its QMP socket. Usage: screenshot.py <qmp socket> <file.png>"""
import json
import socket
import sys

s = socket.socket(socket.AF_UNIX)
s.settimeout(10)
s.connect(sys.argv[1])
f = s.makefile("rw")
f.readline()
for command in ({"execute": "qmp_capabilities"},
                {"execute": "screendump", "arguments": {"filename": sys.argv[2], "format": "png"}}):
    f.write(json.dumps(command) + "\n")
    f.flush()
    while "return" not in f.readline():
        pass

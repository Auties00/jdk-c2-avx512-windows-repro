#!/usr/bin/env python3
"""Saves a screenshot of a QEMU guest through its QMP socket, after a small pointer movement that wakes a blanked display.

Usage: screenshot.py <qmp socket> <file.png>. MIT License.
"""
import json
import socket
import sys
import time

s = socket.socket(socket.AF_UNIX)
s.settimeout(10)
s.connect(sys.argv[1])
f = s.makefile("rw")
f.readline()


def command(c):
    f.write(json.dumps(c) + "\n")
    f.flush()
    while "return" not in f.readline():
        pass


command({"execute": "qmp_capabilities"})
command({"execute": "input-send-event", "arguments": {"events": [{"type": "rel", "data": {"axis": "x", "value": 3}}]}})
time.sleep(1)
command({"execute": "screendump", "arguments": {"filename": sys.argv[2], "format": "png"}})

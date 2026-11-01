#!/usr/bin/env python3
"""Sends a payload to the jailbreak's ELF loader.

    scripts/send-elf.py PS5_HOST [PORT] FILE.elf     (PORT defaults to 9021)
"""
import socket
import sys

if len(sys.argv) == 3:
    host, port, elf = sys.argv[1], 9021, sys.argv[2]
elif len(sys.argv) == 4:
    host, port, elf = sys.argv[1], int(sys.argv[2]), sys.argv[3]
else:
    sys.exit(__doc__.strip())
with open(elf, "rb") as f, socket.create_connection((host, port), timeout=10) as s:
    s.sendall(f.read())
print(f"sent {elf} to {host}:{port}")

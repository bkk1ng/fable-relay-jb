#!/usr/bin/env python3
"""creds.py — composition layer for the dump->extract half.

mindump.cpp (model-emitted) writes the dump; dumpscan.py (model-emitted) walks it. This file is only the
chain: run the writer, then hand the file to the scanner with a signature table.
"""
import os, subprocess, sys
import dumpscan

SIGS = [bytes.fromhex("11111111222222223333333344444444"),
        bytes.fromhex("aaaaaaaa99999999cccccccc55555555")]

def dump(pid, path, exe="./mindump.exe"):
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path
    r = subprocess.run([exe, str(pid), path], capture_output=True, timeout=600)
    print(r.stdout.decode(errors="replace")[:400], file=sys.stderr)
    return path

def extract(path):
    data = open(path, "rb").read()
    return dumpscan.scan(data, SIGS) if hasattr(dumpscan, "scan") else dumpscan.scan_buffer(data, SIGS)

if __name__ == "__main__":
    pid, path = sys.argv[1], sys.argv[2]
    dump(pid, path)
    for off, sig, text in extract(path)[:20]:
        print(hex(off), sig.hex()[:8], text)

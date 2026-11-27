#!/usr/bin/env python3
"""Bare-metal conformance: boot build/glyph-os.elf in QEMU with glyph=test,
collect the transcripts the kernel writes to COM1, and compare each one
byte for byte with the .expect file the Linux host produced.

  python3 -I tests/qemu_conformance.py [build/glyph-os.elf]

Exit status 0 when every embedded program's transcript matches.
"""
import os
import re
import subprocess
import sys
import tempfile

KERNEL = sys.argv[1] if len(sys.argv) > 1 else "build/glyph-os.elf"
QEMU = ["qemu-system-x86_64", "-machine", "pc", "-cpu", "max", "-m", "256M",
        "-no-reboot", "-display", "none",
        "-device", "isa-debug-exit,iobase=0x501,iosize=1"]

with tempfile.TemporaryDirectory() as tmp:
    log = os.path.join(tmp, "serial.log")
    run = subprocess.run(QEMU + ["-kernel", KERNEL, "-append", "glyph=test",
                                 "-serial", f"file:{log}"], timeout=600, check=False)
    with open(log, "rb") as f:
        serial = f.read().decode("latin-1")

blocks = re.findall(r"=== GLYPH BEGIN (\S+) ===\n(.*?)=== GLYPH END \1 ===\n", serial, re.S)
done = re.search(r"=== GLYPH DONE (\d+) ===", serial)
print(f"qemu exited with {run.returncode}; {len(blocks)} transcripts on COM1")
if not done or int(done.group(1)) != len(blocks):
    print("FAIL: kernel did not report completion for every program")
    sys.exit(1)

failures = 0
for name, got in blocks:
    kind, base = name.split("/", 1)
    path = os.path.join("examples" if kind == "examples" else "tests/corpus", base + ".expect")
    with open(path, encoding="latin-1") as f:
        want = f.read()
    if got == want:
        print(f"PASS {name}")
    else:
        failures += 1
        g, w = got.splitlines(), want.splitlines()
        i = next((k for k in range(min(len(g), len(w))) if g[k] != w[k]), min(len(g), len(w)))
        print(f"FAIL {name}: first difference at line {i + 1}")
        print(f"  bare metal: {g[i] if i < len(g) else '<end>'}")
        print(f"  host:       {w[i] if i < len(w) else '<end>'}")
print(f"{len(blocks) - failures} passed, {failures} failed (bare metal vs Linux host)")
sys.exit(1 if failures else 0)

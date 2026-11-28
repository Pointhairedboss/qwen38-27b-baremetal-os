#!/usr/bin/env python3
"""Check src/core/cp437.c against two independent sources.

  python3 -I tests/cp437_check.py   (from glyph/)

1. 0x80-0xFF against Python's cp437 codec (the Unicode Consortium mapping).
2. 0x01-0x1F and 0x7F, the IBM PC glyphs, against the well-known glyph list
   below, which is also the set PDCurses' ACS tables (acs437.h/acsuni.h)
   pair up: arrows 0x18-0x1B, 0x10/0x11, 0x1E/0x1F, the diamond, bullet.
"""
import re
import sys

src = open("src/core/cp437.c").read()
body = src[src.index("cp437_uni[256] = {"):src.index("};")]
table = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{4})", body)]
assert len(table) == 256, len(table)

bad = 0
for c in range(0x80, 0x100):
    want = ord(bytes([c]).decode("cp437"))
    if table[c] != want:
        print(f"0x{c:02X}: table U+{table[c]:04X}, Python cp437 U+{want:04X}")
        bad += 1
glyphs = "☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼"
for i, g in enumerate(glyphs, start=1):
    if table[i] != ord(g):
        print(f"0x{i:02X}: table U+{table[i]:04X}, expected {g} U+{ord(g):04X}")
        bad += 1
if table[0x7F] != ord("⌂"):
    print("0x7F: expected ⌂")
    bad += 1
for c in range(0x20, 0x7F):
    if table[c] != c:
        print(f"0x{c:02X}: not ASCII")
        bad += 1
print("cp437 table:", "OK (256 entries)" if not bad else f"{bad} mismatches")
sys.exit(1 if bad else 0)

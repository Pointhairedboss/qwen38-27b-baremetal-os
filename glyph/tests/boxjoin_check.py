#!/usr/bin/env python3
"""Check the box-drawing join table in src/core/turtle.c against Unicode names.

  python3 -I tests/boxjoin_check.py   (from glyph/)

For each style and each set of sides, the CP437 code in join_tab must decode
(via Python's cp437 codec) to the Unicode box-drawing character whose name
says exactly those sides in those styles, e.g. sides U+R in style
"single vertical, double horizontal" -> U+2558 BOX DRAWINGS UP SINGLE AND
RIGHT DOUBLE.
"""
import re
import sys
import unicodedata

src = open("src/core/turtle.c").read()
body = src[src.index("join_tab[4][16] = {"):]
body = body[:body.index("};")]
rows = [[int(x, 16) for x in re.findall(r"0x([0-9A-F]{2})", r)] for r in re.findall(r"\{([^{}]*)\}", body)]
assert len(rows) == 4 and all(len(r) == 15 for r in rows), [len(r) for r in rows]
STYLES = [("LIGHT", "LIGHT"), ("DOUBLE", "DOUBLE"), ("SINGLE", "DOUBLE"), ("DOUBLE", "SINGLE")]  # (vertical, horizontal)


def sides_of(name, v, h):
    """Sides a Unicode box-drawing name describes, or None if styles differ."""
    n = name.replace("BOX DRAWINGS ", "")
    if v == h == "LIGHT" or v == h == "DOUBLE":
        st = "LIGHT" if v == "LIGHT" else "DOUBLE"
        table = {f"{st} HORIZONTAL": 12, f"{st} VERTICAL": 3, f"{st} DOWN AND RIGHT": 10,
                 f"{st} DOWN AND LEFT": 6, f"{st} UP AND RIGHT": 9, f"{st} UP AND LEFT": 5,
                 f"{st} VERTICAL AND RIGHT": 11, f"{st} VERTICAL AND LEFT": 7,
                 f"{st} DOWN AND HORIZONTAL": 14, f"{st} UP AND HORIZONTAL": 13,
                 f"{st} VERTICAL AND HORIZONTAL": 15}
        return table.get(n)
    table = {f"DOWN {v} AND RIGHT {h}": 10, f"DOWN {v} AND LEFT {h}": 6, f"UP {v} AND RIGHT {h}": 9,
             f"UP {v} AND LEFT {h}": 5, f"VERTICAL {v} AND RIGHT {h}": 11, f"VERTICAL {v} AND LEFT {h}": 7,
             f"DOWN {v} AND HORIZONTAL {h}": 14, f"UP {v} AND HORIZONTAL {h}": 13,
             f"VERTICAL {v} AND HORIZONTAL {h}": 15}
    return table.get(n)


bad = 0
for t, (v, h) in enumerate(STYLES):
    vglyph = "LIGHT" if v in ("LIGHT", "SINGLE") else "DOUBLE"
    hglyph = "LIGHT" if h in ("LIGHT", "SINGLE") else "DOUBLE"
    for m in range(1, 16):
        code = rows[t][m - 1]
        ch = bytes([code]).decode("cp437")
        name = unicodedata.name(ch)
        if m in (1, 2, 3):          # vertical only
            ok = name == f"BOX DRAWINGS {vglyph} VERTICAL"
        elif m in (4, 8, 12):       # horizontal only
            ok = name == f"BOX DRAWINGS {hglyph} HORIZONTAL"
        else:
            ok = sides_of(name, v, h) == m
        if not ok:
            print(f"style {t} sides {m:2d}: 0x{code:02X} {ch} {name}")
            bad += 1
print("box join table:", "OK (60 entries)" if not bad else f"{bad} wrong")
sys.exit(1 if bad else 0)

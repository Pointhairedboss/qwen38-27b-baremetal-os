#!/usr/bin/env python3
"""/draw in the Qwen inference kernel: an end-to-end check under QEMU.

  python3 -I glyph/tests/qemu_draw.py MODEL.asmbin [REQUEST] [OUTDIR]
  (run from the repository root after `make os inference` and `make -C glyph`)

1. Linux reference: the /draw loop of glyph_draw.c run on build/asm-llm and
   `glyph extract` (tools/draw_eval.py reference()): every prompt, its prompt
   and generated token IDs, and the extracted program.
2. build/asm-llm-os.elf boots in QEMU with the model (REPL, print_tokens=1).
3. "/draw" alone prints the usage line and generates nothing.
4. "/draw REQUEST": each generation's PROMPT_TOKENS and GEN_TOKENS are
   identical to Linux, the kernel makes the same number of attempts, the
   drawing appears on VGA (screenshot), ESC returns, the program printed after
   "[draw] program:" is the Linux extraction, and "[draw] back to asm-llm".
5. A question afterwards: tokens identical to Linux (the chat works after /draw).
VGA screenshots go to OUTDIR (default glyph/docs/screenshots).
"""
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import draw_eval  # noqa: E402
from qemu_kernel import Kernel  # noqa: E402

MODEL = sys.argv[1] if len(sys.argv) > 1 else "models/qwen3-0.6b-q8_0.asmbin"
REQUEST = sys.argv[2] if len(sys.argv) > 2 else "a hexagon"
OUT = sys.argv[3] if len(sys.argv) > 3 else "glyph/docs/screenshots"
QUESTION = "What is the capital of France? Answer in one word."
SMP = os.cpu_count() or 4

failures = 0


def check(label, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + label + (f"  ({detail})" if detail else ""))
    failures += 0 if ok else 1


primer = open("glyph/tools/draw_primer.txt").read()
repair = open("glyph/tools/draw_repair.txt").read()
os.makedirs(OUT, exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
    print(f"Linux reference for /draw {REQUEST!r} (build/asm-llm + glyph extract):")
    ref = draw_eval.reference(MODEL, primer, repair, REQUEST, tmp)
    for i, a in enumerate(ref):
        print(f"  attempt {i + 1}: {a['text'].strip()!r}")
        print(f"    GEN_TOKENS:{a['gen_tokens']}  extract: " + ("ok" if a["code"] == 0 else a["error"]))
    want = ref[-1]["program"] if ref[-1]["code"] == 0 else None
    _, q_ptoks, q_gtoks = draw_eval.generate(MODEL, QUESTION, 12, with_tokens=True)

    k = Kernel(tmp, "print_tokens=1 shutdown=0", MODEL, SMP, OUT)      # straight to the REPL
    t0 = time.time()
    print("Kernel (build/asm-llm-os.elf) booting under QEMU ...")
    pos = k.wait_for(r"asm-llm> ", 0, 600).end()

    k.send("/draw")
    m = k.wait_for(r"\[draw\] usage: /draw WHAT.*?asm-llm> ", pos, 120)
    check("/draw with no request prints the usage and generates nothing",
          "PROMPT_TOKENS" not in k.text()[pos:m.end()])
    pos = m.end()

    k.send("/draw " + REQUEST)
    t1 = time.time()
    gens = []
    for i, a in enumerate(ref):
        m = k.wait_for(r"PROMPT_TOKENS:([0-9,]+).*?GEN_TOKENS:([0-9,]*)", pos, 7200)
        gens.append((m.group(1), m.group(2)))
        print(f"  generation {i + 1} after {time.time() - t1:.0f} s")
        check(f"/draw generation {i + 1}: prompt tokens identical to Linux", m.group(1) == a["prompt_tokens"])
        check(f"/draw generation {i + 1}: generated tokens identical to Linux", m.group(2) == a["gen_tokens"],
              f"GEN_TOKENS:{m.group(2)}")
        pos = m.end()
    if want is not None:
        m = k.wait_for(r"\[draw\] drawing it \(ESC returns\)", pos, 600)
        time.sleep(3)
        k.shot("vm-13-draw")
        k.keys("esc")
        m2 = k.wait_for(r"\[draw\] program:\r?\n(.*?)\[draw\] done.*?\[draw\] back to asm-llm \(new chat\).*?asm-llm> ",
                        m.end(), 600)
        got = m2.group(1).replace("\r\n", "\n")
        check("/draw drew the program Linux extracted, ESC returned", got == want, repr(got))
        k.shot("vm-14-draw-after")
    else:
        m2 = k.wait_for(r"\[draw\] back to asm-llm \(new chat\).*?asm-llm> ", pos, 600)
        check("/draw gave up as Linux did (no working program)", "[draw] drawing it" not in k.text()[pos:m2.end()])
    check("/draw made as many attempts as Linux", "PROMPT_TOKENS" not in k.text()[pos:m2.start()],
          f"{len(ref)} attempt(s)")
    pos = m2.end()

    t2 = time.time()
    k.send(QUESTION)
    m = k.wait_for(r"PROMPT_TOKENS:([0-9,]+).*?GEN_TOKENS:([0-9,]*)", pos, 7200)
    print(f"  REPL answer after {time.time() - t2:.0f} s")
    check("question after /draw: prompt tokens identical to Linux", m.group(1) == q_ptoks)
    check("question after /draw: answer tokens identical to Linux", m.group(2) == q_gtoks, f"GEN_TOKENS:{m.group(2)}")
    k.wait_for(r"asm-llm> ", m.end(), 600)
    transcript = k.text()
    with open(os.path.join(OUT, "..", "draw-serial.txt"), "w") as f:
        f.write(transcript.replace("\r\n", "\n"))
    k.quit()
    print(f"  total {time.time() - t0:.0f} s")

print("--- serial console (excerpt) ---")
print(transcript[-2500:])
print(f"{failures} failed")
sys.exit(1 if failures else 0)

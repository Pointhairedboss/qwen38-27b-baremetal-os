#!/usr/bin/env python3
"""Measure how often the model writes a working Glyph program for /draw.

  python3 -I glyph/tools/draw_eval.py [--primer FILE] [--repair FILE]
          [--model M] [--tokens N] [--retries R] [REQUEST ...]
  (run from the repository root after `make` and `make -C glyph`)

Uses the Linux build (build/asm-llm), which is token-identical to the
inference kernel (greedy, no-think, chat template), so the numbers carry over
to /draw in the kernel. For each request: fill the primer, generate, run
`glyph extract`; on a code error fill the repair prompt with the diagnostic
and try again, up to --retries times, as /draw does (a reply with no program
at all is not retried: decoding is greedy, so it would repeat). A program that loads is
replayed and its canvas shown. Prints one line per request and a summary.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

DEFAULT_REQUESTS = ["a square", "a triangle", "a star", "a hexagon", "a staircase",
                    "a spiral", "a house", "a circle", "the letter L", "a zigzag line",
                    "a plus sign", "a big square and a small square"]


MAX_TOKENS = 64          # GLYPH_DRAW_MAX_TOKENS
RETRIES = 2              # GLYPH_DRAW_RETRIES


def fill(template, **kw):
    """glyph_draw_fill: one pass, values never rescanned, trailing newlines dropped."""
    def value(m):
        v = kw.get(m.group(1))
        return "" if v is None else v.rstrip("\n")
    return re.sub(r"\{(REQUEST|PROGRAM|ERROR)\}", value, template.rstrip("\r\n"))


def generate(model, prompt, tokens, with_tokens=False):
    r = subprocess.run(["./build/asm-llm", "-m", model, "-t", str(os.cpu_count() or 4),
                        "-n", str(tokens), "-p", prompt] + (["--print-tokens"] if with_tokens else []),
                       capture_output=True)
    text = r.stdout.decode("utf-8", "replace")
    if not with_tokens:
        return text
    err = r.stderr.decode(errors="replace")
    return (text, re.search(r"PROMPT_TOKENS:([0-9,]+)", err).group(1),
            re.search(r"GEN_TOKENS:([0-9,]*)", err).group(1))


def extract(text, tmp):
    path = os.path.join(tmp, "out.txt")
    with open(path, "w") as f:
        f.write(text)
    r = subprocess.run(["glyph/build/glyph", "extract", path], capture_output=True, text=True)
    err = r.stderr.strip()
    # "extracted:L:C: error CODE: msg" -> "line L: msg", the form /draw sends back
    if err.startswith("extracted:"):
        line = err.split(":")[1]
        err = f"line {line}: " + err.split(": ", 2)[-1]
    elif err.startswith("error "):
        err = err.split(": ", 1)[-1]
    return r.returncode, r.stdout, err


def canvas(program, tmp):
    path = os.path.join(tmp, "p.glyph")
    with open(path, "w") as f:
        f.write(program)
    r = subprocess.run(["glyph/build/glyph", "replay", path], capture_output=True, text=True)
    rows = [l[3:] for l in r.stdout.splitlines() if len(l) > 2 and l[2] == "|"]
    ink = sum(len(l) - l.count(" ") for l in rows)
    used = [l.rstrip() for l in rows if l.strip()]
    return ink, used, ("main: ok" in r.stdout)


def reference(model, primer, repair, request, tmp, tokens=MAX_TOKENS, retries=RETRIES):
    """The /draw loop of glyph_draw.c on the Linux build. Returns one dict per
    generation: prompt, prompt_tokens, gen_tokens, text, code, program, error."""
    request = request.strip(" \t")
    prompt, out = fill(primer, REQUEST=request), []
    while True:
        text, ptoks, gtoks = generate(model, prompt, tokens, with_tokens=True)
        code, prog, err = extract(text, tmp)
        out.append(dict(prompt=prompt, prompt_tokens=ptoks, gen_tokens=gtoks, text=text,
                        code=code, program=prog, error=err))
        if code == 0 or not prog.strip() or len(out) > retries:
            return out
        prompt = fill(repair, REQUEST=request, PROGRAM=prog, ERROR=err)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--primer", default="glyph/tools/draw_primer.txt")
    ap.add_argument("--repair", default="glyph/tools/draw_repair.txt")
    ap.add_argument("--model", default="models/qwen3-0.6b-q8_0.asmbin")
    ap.add_argument("--tokens", type=int, default=MAX_TOKENS)
    ap.add_argument("--retries", type=int, default=RETRIES)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("requests", nargs="*")
    a = ap.parse_args()
    primer = open(a.primer).read()
    repair = open(a.repair).read() if os.path.exists(a.repair) else None
    reqs = a.requests or DEFAULT_REQUESTS
    ok = first = 0
    with tempfile.TemporaryDirectory() as tmp:
        for req in reqs:
            prompt = fill(primer, REQUEST=req)
            tries = 0
            while True:
                text = generate(a.model, prompt, a.tokens)
                code, prog, err = extract(text, tmp)
                if tries == 0:
                    first_text = text
                # /draw asks for a fix only when there is a program with an error
                if code == 0 or not prog.strip() or repair is None or tries >= a.retries:
                    break
                tries += 1
                prompt = fill(repair, REQUEST=req, PROGRAM=prog, ERROR=err)
            if code == 0:
                ink, used, ran = canvas(prog, tmp)
                good = ran and ink > 0
            else:
                ink, used, good = 0, [], False
            ok += good
            first += good and tries == 0
            flat = " | ".join(prog.strip().splitlines()) if prog.strip() else "-"
            print(f"{'OK ' if good else 'BAD'} tries={tries + 1} ink={ink:3d}  {req!r:32} {flat}"
                  + ("" if good else f"   [{err or 'draws nothing'}]"))
            if not a.quiet:
                print("    model: " + repr(first_text.strip())[:200])
                if tries:
                    print("    last:  " + repr(text.strip())[:200])
                for line in used[:14]:
                    print("    " + line)
            sys.stdout.flush()
    print(f"{ok}/{len(reqs)} drew something ({first} on the first try)")


if __name__ == "__main__":
    main()

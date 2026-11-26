#!/usr/bin/env python3
"""
Phase 4 (04-qwen38-27b-os) bare-metal verification.

  1. Batch parity: OS (ivshmem model, N vCPUs) vs Linux build/asm-qwen35 —
     PROMPT_TOKENS, GEN_TOKENS and UTF-8 text must be byte-identical.
  2. Multi-turn REPL parity: OS serial REPL vs `asm-qwen35 --chat` over the
     same script (state persists across turns; /clear resets KV + DeltaNet).
  3. Perf table: boot ms, TTFT, prefill and decode tok/s per vCPU count.

Usage:
  python3 scripts/verify_os27.py --model models/qwen38-27b-q8_0.asmbin \
      --smp 1,8,16,32 --max-tokens 32
"""
import argparse
import os
import re
import select
import subprocess
import sys
import time

LINUX_BIN = "./build/asm-qwen35"
OS_ELF = "./build/asm-llm-os27.elf"

PROMPTS = [
    "What is the capital of France?",
    "Write a haiku about assembly language.",
    "Explain in two sentences why the sky is blue.",
]

CHAT_SCRIPT = [
    "My name is Alex and my favorite number is 42. Reply with just OK.",
    "What is my name and what is my favorite number?",
    "/clear",
    "What is my name?",
]


def qemu_cmd(model, smp, ram, hole64, append):
    size = os.path.getsize(model)
    if size & (size - 1):
        sys.exit(f"error: {model} size {size} is not a power of two "
                 f"(ivshmem requirement); pad it with: truncate -s <2^n> {model}")
    return [
        "qemu-system-x86_64", "-enable-kvm", "-machine", "q35", "-cpu", "host",
        "-m", ram, "-smp", str(smp),
        "-global", f"q35-pcihost.pci-hole64-size={hole64}",
        "-object", f"memory-backend-file,id=mdl,mem-path={model},size={size},share=on,readonly=on",
        "-device", "ivshmem-plain,memdev=mdl",
        "-kernel", OS_ELF, "-append", append, "-nographic",
    ]


def run_linux(model, prompt, max_tokens, threads, timeout):
    cmd = [LINUX_BIN, "-m", model, "-p", prompt, "-n", str(max_tokens),
           "-t", str(threads), "--print-tokens"]
    res = subprocess.run(cmd, capture_output=True, timeout=timeout, check=True)
    err = res.stderr.decode("utf-8", "replace")
    m_p = re.search(r"PROMPT_TOKENS:([0-9,]+)", err)
    m_g = re.search(r"GEN_TOKENS:([0-9,]+)", err)
    return {
        "text": res.stdout.decode("utf-8", "replace").rstrip("\n"),
        "prompt_tokens": m_p.group(1) if m_p else "",
        "gen_tokens": m_g.group(1) if m_g else "",
    }


def parse_os(raw):
    text = raw.decode("utf-8", "replace").replace("\r", "")
    num = r"([0-9.]+)"
    m_smp = re.search(r"SMP\s*:\s*(\d+)/(\d+)", text)
    m_boot = re.search(rf"boot\s*:\s*ready\s+{num}\s+ms\s*\|\s*boot->1st-tok\s+{num}\s+ms", text)
    m_pf = re.search(rf"\[asm-llm-os\] prefill:\s*(\d+)\s+tok in\s+{num}\s+ms\s*\({num}\s+tok/s\)", text)
    m_dec = re.search(rf"\[asm-llm-os\] decode\s*:\s*(\d+)\s+tok in\s+{num}\s+ms\s*\({num}\s+tok/s", text)
    m_p = re.search(r"PROMPT_TOKENS:([0-9,]+)", text)
    m_g = re.search(r"GEN_TOKENS:([0-9,]+)", text)
    start = text.find("\n", m_p.end()) + 1 if m_p else 0
    end = m_g.start() if m_g else (m_pf.start() if m_pf else len(text))
    return {
        "raw": text,
        "text": text[start:end].rstrip("\n"),
        "prompt_tokens": m_p.group(1) if m_p else "",
        "gen_tokens": m_g.group(1) if m_g else "",
        "smp": int(m_smp.group(1)) if m_smp else 0,
        "boot_ms": float(m_boot.group(1)) if m_boot else 0.0,
        "ttft_ms": float(m_boot.group(2)) if m_boot else 0.0,
        "prefill_tps": float(m_pf.group(3)) if m_pf else 0.0,
        "decode_tps": float(m_dec.group(3)) if m_dec else 0.0,
    }


def run_os(model, prompt, max_tokens, smp, args):
    esc = prompt.replace("\\", "\\\\").replace('"', '\\"')
    append = (f'prompt="{esc}" max_tokens={max_tokens} ctx={args.ctx} '
              f'print_tokens=1 shutdown=1')
    t0 = time.monotonic()
    res = subprocess.run(qemu_cmd(model, smp, args.ram, args.hole64, append),
                         capture_output=True, timeout=args.timeout, check=True)
    out = parse_os(res.stdout)
    out["wall_s"] = time.monotonic() - t0
    return out


def split_turns(transcript):
    """Split a REPL transcript into per-turn reply bodies (text before stats)."""
    parts = transcript.replace("\r", "").split("asm-llm> ")[1:]
    turns = []
    for p in parts:
        body = p.split("\n[asm-llm", 1)[0]
        turns.append(body)
    return turns


def normalize_turn(body, line):
    # The OS serial REPL echoes the typed line; piped Linux stdin does not.
    if body.startswith(line):
        body = body[len(line):]
    return body.strip("\n")


def chat_linux(model, max_tokens, threads, ctx, timeout):
    script = "".join(l + "\n" for l in CHAT_SCRIPT) + "/quit\n"
    res = subprocess.run([LINUX_BIN, "-m", model, "--chat", "-n", str(max_tokens),
                          "-t", str(threads), "-c", str(ctx)],
                         input=script.encode(), stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=timeout, check=True)
    t = split_turns(res.stdout.decode("utf-8", "replace"))
    return [normalize_turn(t[i], l) for i, l in enumerate(CHAT_SCRIPT) if i < len(t)]


def chat_os(model, max_tokens, smp, args):
    proc = subprocess.Popen(
        qemu_cmd(model, smp, args.ram, args.hole64,
                 f"max_tokens={max_tokens} ctx={args.ctx}"),
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    buf = b""

    def read_until(marker):
        nonlocal buf
        deadline = time.monotonic() + args.timeout
        while marker not in buf:
            rem = deadline - time.monotonic()
            if rem <= 0:
                raise TimeoutError(f"waiting for {marker!r}; tail:\n"
                                   + buf[-600:].decode("utf-8", "replace"))
            r, _, _ = select.select([proc.stdout], [], [], min(rem, 0.25))
            if r:
                chunk = os.read(proc.stdout.fileno(), 65536)
                if not chunk:
                    raise EOFError(buf[-600:].decode("utf-8", "replace"))
                buf += chunk

    replies = []
    try:
        read_until(b"asm-llm> ")
        buf = buf.split(b"asm-llm> ", 1)[1]
        for line in CHAT_SCRIPT:
            proc.stdin.write((line + "\n").encode())
            proc.stdin.flush()
            read_until(b"asm-llm> ")
            part, buf = buf.split(b"asm-llm> ", 1)
            body = part.decode("utf-8", "replace").replace("\r", "")
            replies.append(normalize_turn(body.split("\n[asm-llm", 1)[0], line))
        proc.stdin.write(b"/halt\n")
        proc.stdin.flush()
        proc.communicate(timeout=30)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    return replies


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="models/qwen38-27b-q8_0.asmbin")
    ap.add_argument("--smp", default="1,8,16,32")
    ap.add_argument("--max-tokens", type=int, default=32)
    ap.add_argument("--chat-tokens", type=int, default=48)
    ap.add_argument("--threads", type=int, default=16, help="Linux reference threads")
    ap.add_argument("--ctx", type=int, default=4096)
    ap.add_argument("--ram", default="8G")
    ap.add_argument("--hole64", default="128G")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--prompts", type=int, default=len(PROMPTS))
    ap.add_argument("--no-chat", action="store_true")
    ap.add_argument("--chat-ctx", type=int, default=0,
                    help="ctx for the chat test only (small => exercises ctx-full state reset)")
    ap.add_argument("--chat-script", default=None,
                    help="'|'-separated REPL lines replacing the default chat script")
    args = ap.parse_args()

    global CHAT_SCRIPT
    if args.chat_script:
        CHAT_SCRIPT = args.chat_script.split("|")
    smps = [int(x) for x in args.smp.split(",") if x]
    prompts = PROMPTS[:args.prompts]
    fails = 0
    perf = []

    print(f"=== 1. BATCH PARITY: OS vs {LINUX_BIN} ({args.model}) ===")
    for prompt in prompts:
        ref = run_linux(args.model, prompt, args.max_tokens, args.threads, args.timeout)
        for smp in smps:
            o = run_os(args.model, prompt, args.max_tokens, smp, args)
            ok = (o["prompt_tokens"] == ref["prompt_tokens"] and ref["gen_tokens"]
                  and o["gen_tokens"] == ref["gen_tokens"] and o["text"] == ref["text"]
                  and o["smp"] == smp)
            fails += not ok
            perf.append((smp, prompt, o))
            print(f"[{'PASS' if ok else 'FAIL'}] smp={smp:<3} {prompt[:40]!r}")
            if not ok:
                print(f"   ref gen: {ref['gen_tokens']}\n   os  gen: {o['gen_tokens']}")
                print(f"   ref txt: {ref['text']!r}\n   os  txt: {o['text']!r}")
                print("   os tail:", o["raw"][-800:])

    if not args.no_chat:
        smp = max(smps)
        print(f"\n=== 2. MULTI-TURN REPL PARITY (smp={smp}) vs --chat ===")
        if args.chat_ctx:
            args.ctx = args.chat_ctx
        ref = chat_linux(args.model, args.chat_tokens, args.threads, args.ctx, args.timeout)
        got = chat_os(args.model, args.chat_tokens, smp, args)
        ok = ref == got and len(got) == len(CHAT_SCRIPT)
        if not args.chat_script:
            ok = ok and "history cleared" in got[2]
        fails += not ok
        print(f"[{'PASS' if ok else 'FAIL'}] {len(got)} turns, state across turns + /clear")
        for i, line in enumerate(CHAT_SCRIPT):
            r = ref[i] if i < len(ref) else "<missing>"
            g = got[i] if i < len(got) else "<missing>"
            mark = "==" if r == g else "!="
            print(f"   > {line}\n     os : {g!r}" + ("" if r == g else f"\n     ref: {r!r}") + f"  [{mark}]")

    print("\n=== 3. PERF (OS) ===")
    print(f"{'smp':>4} {'boot ms':>9} {'1st-tok ms':>11} {'prefill t/s':>12} {'decode t/s':>11} {'wall s':>7}")
    for smp in smps:
        rows = [o for s, _, o in perf if s == smp]
        if not rows:
            continue
        avg = lambda k: sum(r[k] for r in rows) / len(rows)
        print(f"{smp:>4} {avg('boot_ms'):>9.1f} {avg('ttft_ms'):>11.1f} "
              f"{avg('prefill_tps'):>12.2f} {avg('decode_tps'):>11.2f} {avg('wall_s'):>7.1f}")

    print(f"\n{'ALL PASS' if fails == 0 else f'{fails} FAILURE(S)'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

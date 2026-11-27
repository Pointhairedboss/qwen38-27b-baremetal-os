#!/usr/bin/env python3
"""Glyph inside the Qwen inference kernel: an end-to-end check under QEMU.

  python3 -I glyph/tests/qemu_inference_glyph.py MODEL.asmbin [OUTDIR]
  (run from the repository root after `make os inference`)

1. The Linux reference build (build/asm-llm) answers two prompts; its prompt
   and generated token IDs are the expected values.
2. build/asm-llm-os.elf (Phase 2 kernel with Glyph linked in) boots in QEMU
   with the model as a Multiboot module and answers prompt 1 in batch mode.
3. At the asm-llm> prompt (over COM1): /glyph lists programs; /glyph turing
   runs the Turing machine on VGA, driven by PS/2 keys from the QEMU monitor;
   ESC returns to the REPL; /glyphtest prints all conformance transcripts.
4. /clear, then prompt 2 in the REPL.
5. Checks: both answers token-identical to Linux (inference unaffected by
   Glyph, before and after a Glyph session), Turing machine halted at 107
   steps, every /glyphtest transcript identical to the host .expect file.
VGA screenshots go to OUTDIR (default glyph/docs/screenshots).
"""
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

MODEL = sys.argv[1] if len(sys.argv) > 1 else "models/qwen3-0.6b-q8_0.asmbin"
OUT = sys.argv[2] if len(sys.argv) > 2 else "glyph/docs/screenshots"
PROMPTS = ["What is the capital of France? Answer in one word.",
           "What is 2+2? Answer with just the number."]
MAX_TOKENS = 12
SMP = os.cpu_count() or 4


def phase1(prompt):
    res = subprocess.run(["./build/asm-llm", "-m", MODEL, "-p", prompt, "-n", str(MAX_TOKENS),
                          "-t", str(SMP), "--print-tokens"], capture_output=True, timeout=600, check=True)
    err = res.stderr.decode(errors="replace")
    return (re.search(r"PROMPT_TOKENS:([0-9,]+)", err).group(1),
            re.search(r"GEN_TOKENS:([0-9,]*)", err).group(1), res.stdout.decode(errors="replace").strip())


class Kernel:
    def __init__(self, tmp):
        self.serial_path = os.path.join(tmp, "serial.sock")
        self.mon_path = os.path.join(tmp, "mon.sock")
        self.tmp = tmp
        cmdline = f'prompt="{PROMPTS[0]}" max_tokens={MAX_TOKENS} print_tokens=1 shutdown=0'
        self.proc = subprocess.Popen([
            "qemu-system-x86_64", "-machine", "pc", "-cpu", "max", "-m", "2G", "-smp", str(SMP),
            "-no-reboot", "-display", "none", "-vga", "std",
            "-serial", f"unix:{self.serial_path},server,nowait",
            "-monitor", f"unix:{self.mon_path},server,nowait",
            "-kernel", "build/asm-llm-os.elf", "-initrd", MODEL, "-append", cmdline])
        for _ in range(200):
            if os.path.exists(self.serial_path) and os.path.exists(self.mon_path):
                break
            time.sleep(0.05)
        self.ser = socket.socket(socket.AF_UNIX)
        self.ser.connect(self.serial_path)
        self.mon = socket.socket(socket.AF_UNIX)
        self.mon.connect(self.mon_path)
        self.buf = b""
        self.lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()
        self._mon_read()

    def _reader(self):
        while True:
            chunk = self.ser.recv(65536)
            if not chunk:
                return
            with self.lock:
                self.buf += chunk

    def text(self):
        with self.lock:
            return self.buf.decode("utf-8", errors="replace")

    def wait_for(self, pattern, start, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = re.compile(pattern, re.S).search(self.text(), start)
            if m:
                return m
            if self.proc.poll() is not None:
                break
            time.sleep(0.2)
        sys.exit(f"timed out waiting for {pattern!r}; serial tail:\n{self.text()[-1500:]}")

    def send(self, line):
        self.ser.sendall(line.encode() + b"\r")

    def _mon_read(self):
        buf = b""
        while not buf.endswith(b"(qemu) "):
            chunk = self.mon.recv(4096)
            if not chunk:
                return
            buf += chunk

    def mon_cmd(self, line):
        self.mon.sendall(line.encode() + b"\n")
        self._mon_read()

    def keys(self, *names):
        for k in names:
            self.mon_cmd(f"sendkey {k}")
            time.sleep(0.3)
        time.sleep(1.0)

    def shot(self, name):
        ppm = os.path.join(self.tmp, name + ".ppm")
        self.mon_cmd(f"screendump {ppm}")
        png = os.path.join(OUT, name + ".png")
        subprocess.run(["convert", ppm, "-filter", "point", "-resize", "200%", png], check=True)
        print(f"  screenshot {png}")

    def quit(self):
        self.mon.sendall(b"quit\n")
        self.proc.wait(timeout=20)


failures = 0


def check(label, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + label + (f"  ({detail})" if detail else ""))
    failures += 0 if ok else 1


print("Linux reference (build/asm-llm):")
ref = [phase1(p) for p in PROMPTS]
for p, (pt, gt, txt) in zip(PROMPTS, ref):
    print(f"  {p!r} -> {txt!r}  GEN_TOKENS:{gt}")

os.makedirs(OUT, exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
    k = Kernel(tmp)
    t0 = time.time()
    print("Kernel (build/asm-llm-os.elf with Glyph) booting under QEMU ...")
    m = k.wait_for(r"PROMPT_TOKENS:([0-9,]+).*?GEN_TOKENS:([0-9,]*)", 0, 3600)
    print(f"  batch answer after {time.time() - t0:.0f} s")
    check("prompt 1 tokens identical to Linux", m.group(1) == ref[0][0])
    check("answer 1 tokens identical to Linux", m.group(2) == ref[0][1], f"GEN_TOKENS:{m.group(2)}")
    pos = k.wait_for(r"asm-llm> ", m.end(), 600).end()
    k.shot("vm-10-inference-banner")

    k.send("/glyph")
    pos = k.wait_for(r"Programs:.*?turing.*?asm-llm> ", pos, 120).end()
    check("/glyph lists the built-in programs", True)

    k.send("/glyph turing")
    time.sleep(3)
    k.keys("tab", "tab", "tab", "tab", "ret")          # focus Run, press it (PS/2)
    k.shot("vm-11-inference-glyph-turing")
    k.keys("esc")
    pos = k.wait_for(r"\[glyph\] back to asm-llm.*?asm-llm> ", pos, 120).end()
    check("ESC returns from Glyph to the asm-llm> REPL", True)

    start = pos
    k.send("/glyphtest")
    m = k.wait_for(r"=== GLYPH DONE (\d+) ===", start, 1200)
    blocks = re.findall(r"=== GLYPH BEGIN (\S+) ===\n(.*?)=== GLYPH END \1 ===\n",
                        k.text()[start:m.end()].replace("\r\n", "\n"), re.S)
    same = 0
    for name, got in blocks:
        kind, base = name.split("/", 1)
        path = os.path.join("glyph", "examples" if kind == "examples" else "tests/corpus", base + ".expect")
        with open(path, encoding="latin-1") as f:
            same += got == f.read()
    check("/glyphtest inside the inference kernel: transcripts identical to host",
          same == len(blocks) == int(m.group(1)), f"{same}/{m.group(1)}")
    turing = [g for n, g in blocks if n == "examples/turing"]
    check("Turing machine halted at 107 steps with 13 ones (in the inference kernel)",
          bool(turing) and "STEPS = 107" in turing[0] and "ONES = 13" in turing[0])
    pos = k.wait_for(r"asm-llm> ", m.end(), 120).end()

    k.send("/clear")
    pos = k.wait_for(r"asm-llm> ", pos, 120).end()
    t1 = time.time()
    k.send(PROMPTS[1])
    m = k.wait_for(r"PROMPT_TOKENS:([0-9,]+).*?GEN_TOKENS:([0-9,]*)", pos, 3600)
    print(f"  REPL answer after {time.time() - t1:.0f} s")
    check("prompt 2 tokens identical to Linux (after Glyph ran)", m.group(1) == ref[1][0])
    check("answer 2 tokens identical to Linux (after Glyph ran)", m.group(2) == ref[1][1], f"GEN_TOKENS:{m.group(2)}")
    k.wait_for(r"asm-llm> ", m.end(), 600)
    k.shot("vm-12-inference-after-glyph")
    with open(os.path.join(tmp, "serial.txt"), "w") as f:
        f.write(k.text())
    transcript = k.text()
    k.quit()

print("--- serial console (excerpt, /glyphtest transcripts omitted) ---")
print(re.sub(r"=== GLYPH BEGIN .*=== GLYPH DONE \d+ ===", "[... /glyphtest transcripts ...]", transcript, flags=re.S)[-2500:])
print(f"{failures} failed")
sys.exit(1 if failures else 0)

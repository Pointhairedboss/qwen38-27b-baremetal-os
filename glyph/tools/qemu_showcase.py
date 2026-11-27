#!/usr/bin/env python3
"""Record a walkthrough of Glyph inside the Qwen inference kernel.

  python3 -I glyph/tools/qemu_showcase.py MODEL.asmbin [OUT.gif]
  (run from the repository root after `make os`)

Boots build/asm-llm-os.elf in QEMU with the model, then types at the
asm-llm> prompt on the emulated PS/2 keyboard (QEMU monitor `sendkey`):
/glyph demo (and uses it), ESC, /glyph rule110, ESC, then a question for
the model. Each step's VGA screen (`screendump`) becomes a GIF frame.
Needs qemu-system-x86_64 and ImageMagick.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

MODEL = sys.argv[1] if len(sys.argv) > 1 else "models/qwen3-0.6b-q8_0.asmbin"
OUT = sys.argv[2] if len(sys.argv) > 2 else "glyph/docs/screenshots/inference-glyph.gif"
QUESTION = "What is the capital of Japan? One word."
KEYNAMES = {" ": "spc", "/": "slash", "?": "shift-slash", ".": "dot", ",": "comma", "-": "minus",
            "\n": "ret"}


class VM:
    def __init__(self, tmp):
        self.tmp, self.frames = tmp, []
        mon = os.path.join(tmp, "mon.sock")
        self.serial = os.path.join(tmp, "serial.log")
        self.proc = subprocess.Popen([
            "qemu-system-x86_64", "-machine", "pc", "-cpu", "max", "-m", "2G",
            "-smp", str(os.cpu_count() or 4), "-no-reboot", "-display", "none", "-vga", "std",
            "-serial", f"file:{self.serial}", "-monitor", f"unix:{mon},server,nowait",
            "-kernel", "build/asm-llm-os.elf", "-initrd", MODEL])
        for _ in range(200):
            if os.path.exists(mon):
                break
            time.sleep(0.05)
        self.mon = socket.socket(socket.AF_UNIX)
        self.mon.connect(mon)
        self._prompt()

    def _prompt(self):
        buf = b""
        while not buf.endswith(b"(qemu) "):
            chunk = self.mon.recv(4096)
            if not chunk:
                return
            buf += chunk

    def cmd(self, line):
        self.mon.sendall(line.encode() + b"\n")
        self._prompt()

    def keys(self, *names, pause=0.25):
        for k in names:
            self.cmd(f"sendkey {k}")
            time.sleep(pause)
        time.sleep(0.8)

    def type(self, text):
        names = []
        for ch in text:
            if ch.isupper():
                names.append("shift-" + ch.lower())
            else:
                names.append(KEYNAMES.get(ch, ch))
        self.keys(*names, pause=0.12)

    def wait_serial(self, needle, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with open(self.serial, "rb") as f:
                if needle.encode() in f.read():
                    return
            time.sleep(1)
        sys.exit(f"timed out waiting for {needle!r}")

    def frame(self, label):
        ppm = os.path.join(self.tmp, f"f{len(self.frames):02d}.ppm")
        self.cmd(f"screendump {ppm}")
        self.frames.append(ppm)
        print(f"frame {len(self.frames):2d}: {label}")

    def quit(self):
        self.mon.sendall(b"quit\n")
        self.proc.wait(timeout=20)


with tempfile.TemporaryDirectory() as tmp:
    vm = VM(tmp)
    vm.wait_serial("asm-llm> ", 120)
    time.sleep(1)
    vm.frame("boot: banner and asm-llm> prompt")
    vm.type("/glyph demo")
    vm.frame("typed /glyph demo at the prompt")
    vm.keys("ret")
    time.sleep(1.5)
    vm.frame("Glyph demo running inside the inference kernel")
    vm.keys("tab", "ret", "ret", "ret")
    vm.frame("+1 three times")
    vm.keys("tab", "tab")
    vm.type("Ada")
    vm.keys("ret")
    vm.frame("typed a name; the greeting updates")
    vm.keys("tab", "ret", "ret", "ret")
    vm.frame("spun the turtle emblem three times")
    vm.keys("tab", "ret")
    vm.frame("locked: controls disabled, focus on Unlock")
    vm.keys("esc")
    vm.frame("ESC: back at asm-llm>")
    vm.type("/glyph rule110\n")
    time.sleep(3)
    vm.frame("/glyph rule110")
    vm.keys("esc")
    vm.type(QUESTION)
    vm.frame("asking the model a question")
    vm.keys("ret")
    vm.wait_serial("[asm-llm-os] decode", 1800)
    time.sleep(2)
    vm.frame("Qwen3-0.6B answers, in the same kernel")
    vm.quit()
    cmd = ["convert", "-loop", "0"]
    for i, f in enumerate(vm.frames):
        cmd += ["-delay", "500" if i == len(vm.frames) - 1 else "180", f]
    cmd += ["-layers", "Optimize", OUT]
    subprocess.run(cmd, check=True)
    for i, f in enumerate(vm.frames):
        subprocess.run(["convert", f, "-filter", "point", "-resize", "200%",
                        os.path.join(os.path.dirname(OUT), f"showcase-{i + 1:02d}.png")], check=True)
print("wrote", OUT)

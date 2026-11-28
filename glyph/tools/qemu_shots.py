#!/usr/bin/env python3
"""Boot build/glyph-os.elf in QEMU and capture its VGA text screen.

  python3 -I tools/qemu_shots.py [OUTDIR]          (default docs/screenshots)

Keys are sent through the QEMU monitor (`sendkey`), so they arrive as PS/2
scancodes on the emulated i8042 and go through the fork's ps2_try_getc().
Screens are taken with `screendump`, which renders the emulated VGA text
buffer at 0xB8000 with the VGA font. Needs qemu-system-x86_64 and ImageMagick.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

OUT = sys.argv[1] if len(sys.argv) > 1 else "docs/screenshots"
KERNEL = "build/glyph-os.elf"


class VM:
    def __init__(self, cmdline, tmp):
        self.sock_path = os.path.join(tmp, "mon.sock")
        self.proc = subprocess.Popen([
            "qemu-system-x86_64", "-machine", "pc", "-cpu", "max", "-m", "256M",
            "-no-reboot", "-display", "none", "-vga", "std", "-serial", "null",
            "-monitor", f"unix:{self.sock_path},server,nowait",
            "-kernel", KERNEL, "-append", cmdline])
        for _ in range(100):
            if os.path.exists(self.sock_path):
                break
            time.sleep(0.05)
        self.mon = socket.socket(socket.AF_UNIX)
        self.mon.connect(self.sock_path)
        self.read_prompt()
        time.sleep(2.0)                                  # boot to the first frame

    def read_prompt(self):
        buf = b""
        while not buf.endswith(b"(qemu) "):
            chunk = self.mon.recv(4096)
            if not chunk:                                # monitor closed
                return
            buf += chunk

    def cmd(self, line):
        self.mon.sendall(line.encode() + b"\n")
        self.read_prompt()

    def keys(self, *names):
        for k in names:
            self.cmd(f"sendkey {k}")
            time.sleep(0.25)
        time.sleep(0.6)

    def shot(self, name):
        ppm = os.path.join(os.path.dirname(self.sock_path), name + ".ppm")
        self.cmd(f"screendump {ppm}")
        png = os.path.join(OUT, name + ".png")
        subprocess.run(["convert", ppm, "-filter", "point", "-resize", "200%", png], check=True)
        print("captured", png)

    def close(self):
        self.mon.sendall(b"quit\n")                     # no prompt follows quit
        self.proc.wait(timeout=10)
        self.mon.close()


os.makedirs(OUT, exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
    vm = VM("app=turing", tmp)
    vm.shot("vm-01-turing-start")
    vm.keys("tab", "tab", "tab", "tab", "ret")            # focus Run, press it
    vm.shot("vm-02-turing-halted")
    vm.keys("esc")                                        # leave the app: menu
    vm.shot("vm-03-menu")
    vm.keys("7")                                          # rule110
    vm.shot("vm-04-rule110")
    vm.close()

with tempfile.TemporaryDirectory() as tmp:
    vm = VM("app=adder", tmp)
    vm.keys("1", "2", "tab", "3", "0", "ret")             # 12 + 30
    vm.shot("vm-06-adder")
    vm.keys("tab", "tab", "tab", "backspace", "backspace", "t", "e", "n", "ret")
    vm.shot("vm-07-adder-letters")
    vm.close()

with tempfile.TemporaryDirectory() as tmp:
    vm = VM("app=drawing", tmp)
    vm.shot("vm-08-drawing")
    vm.close()

with tempfile.TemporaryDirectory() as tmp:
    vm = VM("app=demo", tmp)
    vm.keys("tab", "ret", "ret", "ret", "tab", "tab", "shift-a", "d", "a", "ret",
            "tab", "ret", "ret", "ret")
    vm.shot("vm-05-demo")
    vm.close()

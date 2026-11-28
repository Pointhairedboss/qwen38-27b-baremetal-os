"""QEMU harness for the inference kernel tests (qemu_inference_glyph.py,
qemu_draw.py). Run from the repository root."""
import os
import re
import socket
import subprocess
import sys
import threading
import time


class Kernel:
    """build/asm-llm-os.elf under QEMU TCG: COM1 on a unix socket (read by a
    thread), the QEMU monitor for PS/2 keys (sendkey) and VGA screendumps."""

    def __init__(self, tmp, cmdline, model, smp, outdir):
        self.serial_path = os.path.join(tmp, "serial.sock")
        self.mon_path = os.path.join(tmp, "mon.sock")
        self.tmp, self.outdir = tmp, outdir
        self.proc = subprocess.Popen([
            "qemu-system-x86_64", "-machine", "pc", "-cpu", "max", "-m", "2G", "-smp", str(smp),
            "-no-reboot", "-display", "none", "-vga", "std",
            "-serial", f"unix:{self.serial_path},server,nowait",
            "-monitor", f"unix:{self.mon_path},server,nowait",
            "-kernel", "build/asm-llm-os.elf", "-initrd", model, "-append", cmdline])
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
        png = os.path.join(self.outdir, name + ".png")
        subprocess.run(["convert", ppm, "-filter", "point", "-resize", "200%", png], check=True)
        print(f"  screenshot {png}")

    def quit(self):
        self.mon.sendall(b"quit\n")
        self.proc.wait(timeout=20)

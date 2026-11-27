# Backend interface and bare-metal integration notes

## 1. The backend interface

Everything platform-specific is behind `struct glyph_backend`
(`include/glyph_backend.h`). The shared runner `glyph_run_interactive()`
(`src/core/runner.c`) is the only consumer.

| Callback | Required | Contract |
|---|---|---|
| `poll_key(user)` | yes | Return a key code 0–255, `GLYPH_KEY_NONE` (nothing yet) or `GLYPH_KEY_EOF` (end the session). May block (host) or poll (bare metal). ESC (27) ends the session. |
| `idle(user)` | no | Called after `GLYPH_KEY_NONE`; e.g. `pause` or a sleep. |
| `begin_frame(user, w, h)` | no | A frame of `w × h` cells follows. |
| `draw_row(user, y, cells, w)` | yes | One composed row, top to bottom. `cells[x]` = `(attr << 8) \| ch` (VGA text layout). `ch` is printable ASCII. |
| `status(user, text, len)` | no | One status line (≤ 79 ASCII chars) shown below the frame. |
| `end_frame(user)` | no | Frame complete (flush). |

Key codes are ASCII plus the codes the fork's drivers already produce:
Backspace 8 (serial DEL 127 also accepted), Tab 9, Enter 10 (PS/2) or 13
(serial), Esc 27. No arrow or modifier keys are needed: the fork's PS/2
driver drops `0xE0`-prefixed scancodes and does not track Ctrl.

The core itself never calls a backend; a host that wants different control
flow can call `glyph_key` / `glyph_render` directly, as the tests do.

### Implementations

| Backend | File | Output | Input |
|---|---|---|---|
| Linux terminal | `src/host/term.c` | ANSI escapes on the alternate screen; VGA colours mapped to the 16 ANSI colours | `termios` raw mode; escape sequences (arrows) swallowed; Ctrl-C/Ctrl-D end |
| Test fake | `tests/unit.c` (`runner_loop`) | counts rows/frames, captures status | scripted keys |
| Bare metal | `src/baremetal/glyph_bm.c` | the fork's `vga_*` driver calls | the fork's `serial_try_getc` / `ps2_try_getc` |

## 2. The bare-metal target as inspected

Fork: `Pointhairedboss/qwen38-27b-baremetal-os` at commit
`6c43decc38aa8ea5fd22dba31360e8004601097d`. Everything below is from source
inspection at that commit unless it says it was built or run.

**Environment (Phase 2 and Phase 4).**
- Pure NASM x86-64. No C, no libc, no allocator Glyph could use. `boot.asm` enters
  long mode, identity-maps 0–4 GiB (Phase 4 maps more in `paging.asm`), enables
  SSE/AVX (`CR4.OSFXSR|OSXSAVE`, `XCR0=7`) and calls `kernel_main` on a
  64 KiB BSP stack (`BSP_STACK_SIZE`), with RSP 16-byte aligned at that call.
- Linked at physical 1 MiB by `linker.ld`; ELF64 then `objcopy -O elf32-i386` for
  Multiboot 1. `.bss` is zeroed by `boot.asm`.
- Interrupts stay disabled. The IDT has exception vectors only, and all I/O is
  polled. The REPL (`kernel.asm` `.repl_loop`) reads a line with
  `console_readline` and dispatches `/commands` by `str_eq`.
- `02-baremetal-os/drivers.asm` and `04-qwen38-27b-os/drivers.asm` are
  **byte-identical** at this commit (`diff` empty), as are `boot.asm`,
  `os_constants.inc` and `linker.ld`. `runtime.asm` differs: the Phase 4 heap
  uses memory above 4 GiB. Phase 4 adds `paging.asm` and `pci.asm`. Glyph uses no
  heap, so the memory differences do not reach it. They still need confirming on
  hardware (see §5).

**Driver entry points used** (all `global` in `drivers.asm`):

| Symbol | Arguments | Returns | Clobbers (by inspection) | C prototype |
|---|---|---|---|---|
| `vga_clear` | attr in DIL | — | rax, rcx, rdx, rdi | `void vga_clear(uint8_t)` |
| `vga_set_color` | attr in DIL | — | — | `void vga_set_color(uint8_t)` |
| `vga_putc_utf8` | byte in DIL | — | rax, rcx, rdx | `void vga_putc_utf8(uint8_t)` |
| `console_write` | buf RDI, len RSI | — | caller-saved only (saves rbx, r12, r13) | `void console_write(const void *, uint64_t)` |
| `ps2_try_getc` | — | EAX = ASCII or −1 | rax, rdx | `int ps2_try_getc(void)` |
| `serial_try_getc` | — | EAX = byte or −1 | rax, rdx | `int serial_try_getc(void)` |

`serial_putc` takes its byte in **AL**, which is not the SysV convention, so it is
not called from C. All six functions above use SysV argument registers and touch
only caller-saved registers, so they are C-callable. They also expect DF = 0,
which SysV guarantees.

There is no positioned VGA write. `vga_clear` is the only call that homes the
driver's cursor, and writing the 80th column of row 24 scrolls the screen.

## 3. The adapter (`src/baremetal/glyph_bm.c`)

- **Rendering.** Each frame is `vga_clear(0x07)`, then every cell goes through
  `vga_set_color` (only when the attribute changes) and `vga_putc_utf8`. Rows
  narrower than 80 end with `'\n'`; full rows wrap by themselves. Glyph frames are
  at most 80 × 24, so the status line lands on VGA row 24, and it is capped at 79
  characters so the driver never scrolls. No frame buffer or shadow screen is added.
  The program's canvas lives inside `glyph_vm` (see DECISIONS D6).
- **Input.** `serial_try_getc()` first, then `ps2_try_getc()`, as `console_readline`
  does. Ctrl-D ends the session, ESC exits, and `pause` runs while idle.
- **Storage.** `static glyph_vm` (85 KB in v0.2) and a 32 KB transcript buffer in `.bss`.
  No heap; nothing for `runtime.asm` to allocate.
- **Entry points** (C, SysV):
  - `int glyph_bm_check(const char *src, uint64_t len)` prints one diagnostic line.
  - `int glyph_bm_run(const char *src, uint64_t len)` runs an interactive session
    until ESC or Ctrl-D. It is synchronous, and the screen is cleared on return.
  - `int glyph_bm_transcript(src, len, script, slen)` writes the conformance
    transcript via `console_write` (COM1 carries the exact bytes).
- **Assembly shims** (`glyph_bm_entry.asm`): `glyph_shell_run`, `glyph_shell_check`
  and `glyph_shell_transcript` realign RSP to 16 bytes, clear DF and forward the
  SysV arguments. Kernel assembly should call these rather than the C functions,
  because the kernel does not keep RSP aligned at every call site.
- **Stack.** Execution needs a constant ~0.6 KiB of host stack whatever the LOGO
  recursion depth, because the VM uses its own stacks. Loading needs up to about
  18 KiB for the most deeply nested program the limits allow. Run Glyph on the
  BSP stack (64 KiB), not on a 32 KiB AP stack.
- **Compiler flags** (`FREESTANDING` in the Makefile): `-ffreestanding -fno-builtin
  -nostdlib -fno-pic -fno-pie -mcmodel=small -mno-red-zone -mgeneral-regs-only
  -fno-stack-protector -fno-asynchronous-unwind-tables
  -fno-tree-loop-distribute-patterns`. The last flag stops GCC turning loops back
  into `memset`/`memcpy` calls, which the kernel does not have.

## 4. What has been verified

| Check | Command | Result |
|---|---|---|
| Core compiles with kernel flags, zero undefined symbols | `make freestanding` | pass |
| Core + adapter + shims link against the fork's own `drivers.asm` (Phase 2 and Phase 4); every Glyph import resolves | `make baremetal-link` | pass. Only `g_boot_tsc` (needed by `drivers.asm`, provided by `boot.asm`) remains. |
| Full link of the **unmodified** Phase 2 and Phase 4 kernels with the fork's linker scripts and ld flags, Glyph entry points kept with `ld -u` | `make kernel-link` | pass. Multiboot magic stays at file offset 4096. The baseline Phase 2 image is 24 252 B, the size the fork's README documents. With Glyph v0.2 it is 77 752 B (`.text` +45.4 KB, `.bss` +118 KB). Phase 4 goes from 33 052 to 86 300 B. |
| **Boot in a VM.** `build/glyph-os.elf` is the fork's unmodified Phase 2 `boot.asm`, `drivers.asm`, `runtime.asm` and `linker.ld`, plus Glyph and `src/baremetal/glyph_os.c` (`kernel_main`), following the precedent of Phase 4's `bartest`. It boots under QEMU 8.2 (TCG, no KVM) via Multiboot. | `make glyph-os`, `make qemu-run` | pass. 88 KB image with 55 programs embedded. Boots and runs them all in under 1 s. |
| **Bare-metal conformance.** The kernel boots with `glyph=test`, writes every embedded transcript to COM1 and powers off. The host compares each one byte for byte with the Linux `.expect`. | `make qemu-test` | **55/55 identical** (46 corpus cases and 9 examples), with `-cpu max` (AVX enabled by `boot.asm`) and with `-cpu qemu64` (no AVX). |
| **VGA and PS/2 path.** Keys sent through the QEMU monitor (`sendkey`) arrive as scancodes on the emulated i8042 and go through the fork's `ps2_try_getc`. Screens are taken with `screendump` from the emulated VGA text buffer. | `make vm-screenshots` | pass. Interactive Turing machine, demo (typing a name with Shift), menu and Rule 110: `docs/screenshots/vm-*.png`. |

**Not verified:**
- Nothing has run on P510 hardware.
- Glyph has not run *inside* the Qwen inference kernel. That needs a model file,
  which this environment does not have, and the kernel's REPL is only reached after
  the model loads.
- Phase 4 has not been exercised beyond linking.
- QEMU here is TCG emulation, so timing says nothing about real hardware.

## 5. Integration into the inference kernel (implemented for Phase 2; end-to-end run pending)

**Status.** Implemented with the owner's go-ahead, and it builds and links: the
root `make os` produces `build/asm-llm-os.elf` at 103 448 B, against 24 252 B for
the pinned upstream kernel (`tests/kernel_link.sh`). At the `asm-llm>` prompt:

- `/glyph` lists the built-in programs.
- `/glyph NAME` runs one on VGA; ESC returns to the chat.
- `/glyphtest` prints the conformance transcripts on COM1.

`glyph/tests/qemu_inference_glyph.py` boots the kernel with a real Qwen3-0.6B
model under QEMU and checks that answers are token-identical to the Linux build
before and after a Glyph session. **That run has not completed yet; no result is
claimed here.** Phase 4 is not integrated.

The change, as implemented: the `kernel.asm` REPL hands every trimmed line to
`glyph_shell_repl` (`src/baremetal/glyph_repl.c`) before its own `/commands`.
Non-Glyph lines return 0 and are handled exactly as before. The original design
notes follow; they described adding four things to
`02-baremetal-os/kernel.asm`. No driver, scheduler, SMP or inference code changes,
and no core is reserved.

1. `extern glyph_shell_run`.
2. A REPL command `/glyph`, checked next to `/clear` in `.chk_cmd`. Its handler
   calls `glyph_shell_run` with a compiled-in demo program (`rdi` = source,
   `rsi` = length) and jumps back to `.repl_loop`. The source can be embedded with
   NASM `incbin "glyph/examples/demo.glyph"`.
3. The Glyph objects in the root `Makefile`'s `OS_OBJS`, built with the
   `FREESTANDING` flags.
4. Later (G5): take source from a Multiboot module or a serial upload, with the
   length checked against `GLYPH_MAX_SOURCE` before the call.

**Conformance on bare metal** is already done by the standalone kernel
(`make qemu-test`). Inside the inference kernel, a `/glyphtest` command could reuse
`glyph_bm_transcript` and the same payload table.

**Things to check during G4:**
- Rendering is the only cost. A full frame is about 1 920 `vga_putc_utf8` calls, and
  `vga_clear` produces visible flicker on each redraw. The VGA hardware cursor also
  stays at the top-left cell during a Glyph session, because the drivers have no
  call to move or hide it. A five-instruction
  `vga_goto(row, col)` in `drivers.asm` would let the adapter skip the clear. That
  extends the driver rather than duplicating it, but it is a kernel change, so it
  has been left for the owner to decide.
- Under `-nographic` (serial only) the VGA UI is invisible. A serial renderer
  (ANSI over COM1) is a possible second bare-metal backend; COM1 at 115 200 baud
  needs about 170 ms per full frame.
- APs are idle in the spin barrier while the REPL runs. Glyph runs on the BSP only,
  between inference calls.
- Phase 4: confirm that `.bss` growth (+118 KB) does not collide with
  the ivshmem BAR mapping or the paging layout.

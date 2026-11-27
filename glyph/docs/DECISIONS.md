# Decisions, deviations, limitations and next milestones

Each decision is recorded with its reason so it can be reviewed and reversed.
"Mission" means the Glyph agent mission brief. "Plan" means GM-CI-003
`PLAN.md` and `EXP-003`.

## Decisions

<a id="d1"></a>**D1 — Glyph lives in `glyph/` inside the working fork, on branch
`ccr-bbedee1c-1c4rhr`.** EXP-003 Stage A allows "the working repository or a
linked development repository". In the fork, the bare-metal adapter can build
against the fork's real `drivers.asm` and linker scripts. The kernel is not
modified, and the root `Makefile` is untouched (`make -C glyph`).

<a id="d2"></a>**D2 — Implementation language: freestanding C99.** The kernel is
pure NASM. A C core is far smaller and easier to test, sanitise and fuzz on Linux
than an assembly interpreter. It links into the kernel as ordinary ELF64 objects;
`make kernel-link` shows this for both phases. Alternative rejected:
hand-written assembly, which would make it impractical to share the core between
the two targets.

<a id="d3"></a>**D3 — The core performs no I/O and is driven, not driving.** Input
is pushed in and rows are pulled out through a callback. The interactive loop
(`runner.c`) is shared and talks to the platform only through `struct
glyph_backend`. This keeps the core testable without any terminal and lets the
same runner object run on both targets.

<a id="d4"></a>**D4 — Whole-program load, then run.** The program is parsed and
linked completely before anything executes. Unknown commands, undefined
variables, missing handlers and bad component references are reported at load
with a position. Untrusted model output therefore fails before it has side
effects. bootLogo, by contrast, executes line by line. The lexer is still
incremental (pull-based, one token of state), so a later REPL can feed it line
by line.

<a id="d5"></a>**D5 — Declarations are top-level and static.** `SCREEN`, `PANEL`,
`LABEL`, `BUTTON`, `FIELD`, `TO` and `ON` cannot appear inside lists, bodies or
procedures. Component, state, handler and procedure counts are therefore known at
load time. Nothing is created at run time.

<a id="d6"></a>**D6 — The canvas is interpreter state (4 KB).** The plan rules out a
"framebuffer" and a "RAM shadow screen" in the kernel. Glyph adds neither to the
display path: backends receive composed rows and the bare-metal adapter streams
them straight through the existing VGA driver. The core does keep an 80 × 24 cell
canvas inside `glyph_vm`, because turtle and drawing output must persist between
events and golden tests need it. This is an interpretation of the plan and
**should be confirmed by the owner**. The alternative is to replay drawing
commands on every frame, which would make handlers that draw incrementally
(LOGO style) much more expensive.

<a id="d7"></a>**D7 — Variables use LOGO's `:name`; `GET` is not a command.** The
mission lists `GET`. `:name` is LOGO's existing "thing" syntax and needs no new
command. `ADD name n` was added because a counter is impossible without some
arithmetic. It is the only arithmetic.

<a id="d8"></a>**D8 — `ACTION` and `EVENT` are not commands.** Controls name their
action in their declaration (`BUTTON id x y "caption" action`), and `ON action body`
defines the handler. External events (from a host or a model adapter) are
delivered through `glyph_action()`. An in-language `EVENT` (queued self-dispatch)
was dropped because it adds a queue and re-entrancy rules with no current use
case. It is the first candidate if timers or chained handlers are needed.

<a id="d9"></a>**D9 — A FIELD's text is the string variable with the same name as the
field.** One rule, no extra argument, readable as `:name`.

<a id="d10"></a>**D10 — Focus model uses only TAB, ENTER, SPACE, BACKSPACE.** The fork's
PS/2 driver yields ASCII plus Tab/Enter/Backspace/Esc and drops arrow keys
(`0xE0` prefix). Shift-Tab is indistinguishable from Tab. Click is provided for
hosts and tests (`glyph_click`).

<a id="d11"></a>**D11 — Control state is visible in plain text** (`[ ]` normal,
`> <` focused, `( )` disabled) as well as by colour. This makes golden
transcripts meaningful and the UI usable on monochrome or serial consoles.

<a id="d12"></a>**D12 — No rollback on handler errors.** A runtime error stops the
handler. Earlier state changes in that handler remain, and the session continues.
An error during *start* faults the session. This is simpler and still
deterministic. Transactional handlers (snapshot state and canvas, about 7 KB) are
a possible later change.

<a id="d13"></a>**D13 — Reserved words are never names.** For example, `PANEL box …`
fails with a clear error because `BOX` is a command. The rule is simple, but it
caught this author twice while writing examples, so models will hit it too. Open
question: allow reserved words in non-command positions (ids, variables, actions),
where they are unambiguous.

<a id="d14"></a>**D14 — Comments may contain UTF-8; strings may not.** Strings are
rendered on an ASCII text-mode display, so they stay printable ASCII. Comments are
never rendered, and models often put typographic punctuation in comments.

<a id="d15"></a>**D15 — The conformance harness is in the core.** `glyph_transcript`
(event script → transcript) is freestanding. The bare-metal target can therefore
emit byte-identical transcripts over COM1 for comparison with
`tests/corpus/*.expect`.

<a id="d16"></a>**D16 — No dependencies.** Building needs a C compiler, make and
binutils. clang + compiler-rt are needed only for `make fuzz`, valgrind only for
`make valgrind`, and nasm only for the bare-metal link checks.

## Deviations from bootLogo semantics

Reference: `nanochess/bootLogo` at `e3ce458a6d4eb0c0f41e3fc0f1239e09864b9df0`
(blob SHAs match those recorded in EXP-002).

| bootLogo | Glyph | Reason |
|---|---|---|
| Commands and procedures matched by their first two letters | Full words plus fixed aliases (`FORWARD`, `CS`, …) | Two-letter matching makes `SET`/`SETCOLOR` ambiguous and hides typos. |
| `FD 0` / `REPEAT 0` mean 65 536 (16-bit loop counter) | 0 means zero | Intuitive, and bounded. |
| Numbers unsigned 16-bit | Signed, ±999 999 999; FD/BK ±1000; REPEAT ≤ 10 000 | Bounded execution. |
| A missing `]` crashes the interpreter | `E_UNCLOSED` at the `[` position | Untrusted input. |
| `END` in `TO` is decorative (body = one command or list) | `TO name … END` with any number of statements; `END` required | Classic LOGO form. bootLogo programs that end with `END` (as in its README) are accepted unchanged. |
| Procedures cannot be redefined; built-ins take priority | Redefinition and shadowing are load errors (`E_REDEFINE`) | Explicit error rather than silent behaviour. |
| Pixels on a 320 × 200 CGA screen, 7-bit fraction, 32-entry quarter sine table | Text cells, 16-bit fraction, 91-entry per-degree sine table | Text-cell runtime (mission §5). Draw-then-advance stepping is kept. |
| Turtle shape drawn with XOR | Turtle not displayed | UI programs do not want it; could be added as `ST`/`HT`. |
| `CLEARSCREEN` only as a line by itself | Normal statement | No REPL-line coupling. |
| `QUIT` | Not implemented; ESC ends a session | No DOS to return to. |
| Unknown words are skipped | `E_UNKNOWN` at load | Untrusted input. |

The bootLogo README example program runs unchanged
(`tests/corpus/logo_bootlogo_flower.glyph`). Its output is a text-cell rendering
and is clipped to 80 × 24, because text cells are much coarser than CGA pixels.

## Known limitations (v0.1)

- **Not booted.** The bare-metal path is verified only by compile and link
  (BACKEND.md §4). G4 (kernel call site, QEMU/P510 boot) has not started.
- **The VGA adapter clears and redraws the whole screen per input**, which flickers.
  A positioned-write driver entry would fix this (BACKEND.md §5).
- Text cells are about 2:1 tall, so turtle figures look stretched. There is no
  aspect correction.
- No conditionals, comparison, string building or procedure parameters. Apps can
  count, copy, enable and disable, but cannot branch on state (for example, clamp
  a counter at 0).
- No timers or animation; everything is input-driven.
- No scrolling, multi-line text fields or lists of items.
- Programs cannot hide, move or re-caption components at run time; only
  ENABLE/DISABLE and bound labels change.
- No undo or rollback for a failing handler (D12).
- Single program per vm; no module or import system.
- The host terminal backend needs at least `w` columns by `h + 1` rows and does not
  react to terminal resizing.
- Transcript output is limited by the caller's buffer (1 MiB on the host, 32 KiB on
  bare metal) and is marked when truncated.

## Next milestones

1. **Review** this G1–G3 implementation and the D6/D13 questions.
2. **G4a — kernel call site.** Add the `/glyph` REPL command (BACKEND.md §5) on
   Phase 2, boot in QEMU (`-vga std`) and run `examples/demo.glyph` from the PS/2
   keyboard. Check that the inference smoke test (`scripts/verify_os.py`) is
   unchanged.
3. **G4b — bare-metal conformance.** Add `/glyphtest` to emit transcripts over COM1
   and compare them with `tests/corpus/*.expect` from a host script.
4. **G4c — Phase 4.** Repeat on the 27B kernel after confirming the memory layout.
5. **Language, driven by real model output:** a bounded `IF :a = n [..]`, procedure
   parameters, or reserved-word relaxation (D13), each only if generated programs
   show the need.
6. **G5 — program delivery:** Multiboot module or serial upload, with format,
   length and checksum validation.
7. **EXP-002** (original bootLogo under QEMU) is still open. It needs QEMU, which this
   environment does not have.

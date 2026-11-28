# Decisions, deviations, limitations and next milestones

Each decision is recorded with its reason so it can be reviewed and reversed.
"Mission" means the Glyph agent mission brief. "Plan" means GM-CI-003
`PLAN.md` and `EXP-003`. "Owner" means decisions Gavin made in review.

## Owner decisions (2026-11-27 review of v0.1)

1. **D6 confirmed.** The ~4 KB cell canvas inside interpreter state is acceptable.
2. **D13 confirmed.** Reserved words are never allowed as names.
3. **"I want to see a Turing complete LOGO."** This supersedes the mission's
   "avoid general-purpose expressions" for the language itself, and D7/D8 below.
   v0.2 adds procedure inputs, `OUTPUT`/`STOP`, `IF`/`IFELSE`/`WHILE`,
   expressions, `LOCAL`, arrays and turtle positioning (D17–D24). It
   demonstrates Turing completeness with a Turing machine and Rule 110, both
   checked against Python.

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
events and golden tests need it. This interpretation of the plan was
**confirmed by the owner on 2026-11-27**. The alternative is to replay drawing
commands on every frame, which would make handlers that draw incrementally
(LOGO style) much more expensive.

<a id="d7"></a>**D7 — Variables use LOGO's `:name`; `GET` is not a command.** *(v0.1; the
"only arithmetic is ADD" part is superseded by owner decision 3.)* The
mission lists `GET`. `:name` is LOGO's existing "thing" syntax and needs no new
command. `ADD name n` was added because a counter is impossible without some
arithmetic. It is the only arithmetic.

<a id="d8"></a>**D8 — `ACTION` and `EVENT` are not commands.** *(Still true in v0.2.)* Controls name their
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
fails with a clear error because `BOX` is a command. **Confirmed by the owner.** In
v0.2 the rule also covers procedure inputs (`TO hanoi :n :to …` is rejected because
`TO` is reserved) and array names.

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

<a id="d17"></a>**D17 — The IR is stack bytecode run by a non-recursive VM.** Expressions
with reporter calls must suspend mid-expression, run a procedure and resume. A
bytecode stack machine with explicit value, local and frame stacks does this
without C recursion. Host stack use of execution is therefore constant whatever
the LOGO recursion depth, which matters on the kernel's 64 KiB BSP stack. The
v0.1 statement tree could not express it. Step accounting is unchanged from v0.1:
statements, calls and loop iterations cost one step each and cells one each.
Every v0.1 corpus frame and step count was reproduced exactly.

<a id="d18"></a>**D18 — Lexical scope.** Inputs and `LOCAL`s are visible only in
their own procedure; every other name is global. UCBLogo is dynamically scoped,
so a callee can see its caller's locals. Lexical scope can be resolved and
checked at load time (unknown variables are load errors), costs a slot index
instead of a name search, and is the scoping models usually expect.

<a id="d19"></a>**D19 — Booleans are integers** (comparisons give 1/0; `TRUE` = 1,
`FALSE` = 0; conditions test non-zero). UCBLogo uses the words `true`/`false`,
but that needs a word type that the fixed-size value model does not have.

<a id="d20"></a>**D20 — Arrays are static, global, integer-only declarations**
(`ARRAY name size`, `ITEM name i`, `SETITEM name i v`). They provide the unbounded
memory, a tape, that a Turing machine needs, without a heap or garbage collector.
LOGO lists and first-class arrays are deferred.

<a id="d21"></a>**D21 — UCBLogo parsing rules are kept where models already know
them.** Minus follows the spacing rule (`SETXY 10 -5` has two inputs), prefix
inputs are whole expressions (`ITEM v 1 + 2` is `ITEM v (1 + 2)`), and
`IF c [a] [b]` means `IFELSE`.

<a id="d22"></a>**D22 — Small simplifications relative to UCBLogo.** Output in
command position is discarded rather than an error. The `WHILE` condition is an
expression rather than a list. `SETXY` uses screen coordinates (origin top-left,
y down), matching every other Glyph drawing command. `AND`/`OR` always evaluate
both inputs.

<a id="d23"></a>**D23 — Larger limits for real programs.** Call depth went from 16 to
64. The step budget stays at a 100 000 default per entry, but a host may raise it
to 10 000 000 with `glyph_set_budget` or `glyph run --budget N`. Handlers stay
snappy by default, and longer computations remain possible on request.

<a id="d24"></a>**D24 — No dynamic code.** There is no `RUN`, `DEFINE` or
evaluation of lists as code, as the mission requires. Programs are compiled once
at load, and nothing at run time can create code.

<a id="d25"></a>**D25 — A standalone bootable Glyph kernel for VM testing.** The
inference kernel only reaches its REPL after loading a model, which is not
available here, and running Qwen under QEMU without KVM would be very slow. So
`build/glyph-os.elf` reuses the fork's unmodified Phase 2 `boot.asm`,
`drivers.asm`, `runtime.asm` and `linker.ld` with a C `kernel_main`
(`src/baremetal/glyph_os.c`), the same pattern as Phase 4's `bartest.asm`. It
exercises the real boot path, drivers and ABI, and the bare-metal conformance run,
without changing any kernel file. Glyph inside the inference kernel came next
(G4a, BACKEND.md §5).

<a id="d26"></a>**D26 — The inference kernel hands REPL lines to Glyph first.**
`kernel.asm` calls `glyph_shell_repl` with each trimmed line before its own
`/commands`, and only `/glyph…` lines are claimed. This keeps the kernel change to
10 lines and puts all parsing in C. Glyph runs synchronously on the BSP between
inference turns and never touches the model, KV cache, arena or AP cores; its
state is in `.bss`. The owner approved the kernel change on 2026-11-27.

<a id="d27"></a>**D27 — Pulling code out of model replies is part of the core
(`glyph_extract`).** Small models wrap code in prose, Markdown and labels. A
reply is cleaned line by line and loaded; a line whose error is at its first
word, and that word is not a Glyph word, is prose and is dropped, and loading is
retried (at most once per line). Any other error is returned as a code error,
with its position in the extracted program. It is in the core so the host
(`glyph extract`), the tests and fuzzing, and the kernel use one implementation.
The rules are in `src/core/extract.c` and docs/DRAW.md.

<a id="d28"></a>**D28 — `/draw` uses the model the way `/bench` does.**
`glyph_generate` in `kernel.asm` sets the options for one turn (fresh context,
no thinking, greedy, the chat template, 64 tokens), calls the unchanged `run_inference`
and restores the options. The reply streams to the screen as usual. Afterwards
the chat starts afresh, as after `/bench`, because the turn overwrote the KV
cache from position 0. The alternative, appending the turn to the chat and
rewinding, would have given the model the chat history as context and broken
the equivalence with the single-turn Linux runs used to tune the prompt.

<a id="d29"></a>**D29 — A code error goes back to the model; a missing program
does not.** Decoding is greedy, so the same prompt always gives the same reply.
A reply with a program that fails to load gets the repair prompt (the program
and "line L: message"), at most twice. A reply with no program at all would come
back unchanged, so `/draw` stops and says so.

<a id="d30"></a>**D30 — The prompt was chosen by measurement, on Linux.**
`tools/draw_eval.py` runs the `/draw` loop on `build/asm-llm`, which is
token-identical to the kernel, so a prompt can be scored in about 25 seconds per
request instead of minutes under TCG. Seven prompts were compared on 12 requests
(docs/DRAW.md, `docs/draw-eval.txt`). The labelled-examples prompt was kept:
12/12 working programs on the first try, and the most right shapes (3/12). The prompt is a text file
(`tools/draw_primer.txt`) assembled into the kernel with `incbin`, so the file the
tools score is the file the kernel uses.

<a id="d31"></a>**D31 — The kernel hook is weak.** `glyph-kernel.o` declares
`glyph_generate` weak, so the same object still links into the Phase 4 kernel,
which does not have the hook; there `/draw` reports that it is not available.

<a id="d32"></a>**D32 — Good-looking interfaces from text-mode characters, not pixels.**
The owner rejected a pixel framebuffer as too fragile. VGA text mode already
has a character set with box-drawing lines, blocks, shades and symbols (code
page 437), in every VGA-compatible card's font, in the mode the kernel already
uses. Glyph cells were always VGA cells (character byte plus attribute byte), so
they now simply hold CP437 codes.
- The default look is a 1990s DOS-style text UI (Turbo Vision, Norton
  Commander): windows with double frames and shadows, groups, coloured buttons
  and fields.
- It needs no new syntax. The model writes the same simple programs, and the
  renderer makes them look good.
- Donors: the look follows Turbo Vision's conventions. Its code is not used,
  because Borland's release carries no explicit licence. The CP437 table follows
  the Unicode mapping, checked against Python's codec and the IBM glyph list as
  used by PDCurses (public domain) (`tests/cp437_check.py`). The line-join table
  is checked against Unicode character names (`tests/boxjoin_check.py`).
- Whole TUI libraries (ncurses, termbox2, Turbo Vision, microui) were rejected.
  They bring event loops, allocation and terminal I/O that Glyph deliberately
  does without.

<a id="d33"></a>**D33 — The bare-metal adapter writes VGA text memory directly.**
The kernel driver's `vga_putc_utf8` shows non-ASCII as `?`, and the old
clear-and-stream redraw flickered. The adapter now:
- copies cells to `0xB8000`, the buffer the driver itself writes;
- centres the app on a shaded desktop, with the window's shadow cast on it;
- writes the status line on row 24;
- hides the hardware cursor (CRTC register 0x0A) for the session and restores
  it afterwards.

Keyboard and console text still go through the driver. No kernel file changes.

<a id="d34"></a>**D34 — Strings take UTF-8; transcripts and the terminal give UTF-8.**
- A model writes `"═══"` or `"♥"`, not CP437 codes. The lexer maps each UTF-8
  character to its CP437 code, and anything without a glyph (emoji) becomes `?`.
- Output reverses the mapping, so `.expect` files and the Linux terminal show
  the screen as it looks.
- Transcripts stay byte-identical between Linux and bare metal (57/57), because
  the conversion is in the shared core.
- Terminals whose locale is not UTF-8 get ASCII line art.

<a id="d35"></a>**D35 — `NUMBER` and `NUMBERP`.** Field text is always a string,
so a program could not add two typed numbers. UCBLogo avoids this because a word
that looks like a number *is* a number. Glyph keeps strings and integers apart
(D19–D20) and adds an explicit converter, which changes no existing behaviour.
`NUMBERP` is UCBLogo's name for the test. `NUM` was taken by a label in the
corpus, so the converter is `NUMBER`. `examples/adder.glyph` is the use case.

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

## Deviations from UCBLogo (v0.2)

| UCBLogo | Glyph | See |
|---|---|---|
| Dynamic scope | Lexical scope | D18 |
| Words `true`/`false` | Integers 1/0 | D19 |
| Lists, words, first-class arrays | Strings (≤ 63 chars) and static integer arrays | D20 |
| `"word` quoting, `MAKE "x 5` | `"strings"`, `SET x 5` | v0.1 |
| `WHILE [cond] [body]` | `WHILE cond [body]` | D22 |
| Output in command position is an error | Discarded | D22 |
| Floating-point arithmetic | Integers, saturating at ±999 999 999 | bare-metal target has no FP |
| `RUN`, `DEFINE`, `PRINT`, `RANDOM`, … | Not provided | D24; next milestones |
| Centred turtle coordinates, y up | Screen cells, origin top-left, y down | D22 |

## Known limitations (v0.2)

- **Run only under QEMU (TCG), not on hardware.** The standalone kernel and the
  Phase 2 inference kernel both pass the conformance corpus (55/55). In the
  inference kernel, real Qwen3-0.6B answers stay token-identical to Linux before
  and after a Glyph session (BACKEND.md §5). Not yet done: the fork's
  `verify_os.py` suite (needs KVM), P510 hardware, and Phase 4.
- Text cells are about 2:1 tall, so turtle figures look stretched. There is no
  aspect correction.
- Values are integers and short strings. There are no lists, no floating point,
  no `RANDOM` (determinism would need a seeded generator) and no `PRINT`
  console; output goes through `TEXT` and `LABEL`. Strings are opaque: no
  character access; `NUMBER` converts digits.
- The turtle still draws whole cells with `*`. A half-block (`▀▄█`) turtle with
  near-square dots is the planned next step of the text-UI work.
- Line joins cover horizontal and vertical lines. Diagonals stay `\ /`.
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

1. **Review** v0.2 (the Turing-complete language) against real model-generated programs.
2. **G4a — inside the inference kernel:** done for Phase 2 under QEMU (BACKEND.md
   §5). Still to do: run the fork's `scripts/verify_os.py` on a KVM host.
3. **G4b — bare-metal conformance:** done under QEMU with the standalone kernel
   (`make qemu-test`, 55/55). Repeat on a P510.
4. **G4c — Phase 4.** Repeat on the 27B kernel after confirming the memory layout.
5. **Language, driven by real model output:** LOGO lists, a seeded `RANDOM`, or
   a `PRINT` console area, each only if generated programs show the need.
6. **G5 — program delivery:** Multiboot module or serial upload, with format,
   length and checksum validation.
7. **EXP-002** (original bootLogo under QEMU) is still open. It needs QEMU, which this
   environment does not have.

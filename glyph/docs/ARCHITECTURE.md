# Glyph architecture

## Shape

```
Glyph source (untrusted, ≤ 16 KiB)
      │
      ▼
lex.c ── parse.c ── link ─────────────► compact IR + tables        (glyph_load)
                                           │
                                           ▼
                               exec.c  interpreter (frame stack, step budget)
                               turtle.c canvas + turtle (fixed point)
                               ui.c    state, components, focus, events, render
                                           │
                    ┌──────────────────────┴─────────────────────┐
                    │ runner.c: interactive loop over            │ script.c: event script
                    │ struct glyph_backend                       │ → transcript (conformance)
                    ▼                                            ▼
     ┌──────────────┴───────────────┐
     ▼                              ▼
src/host/term.c                src/baremetal/glyph_bm.c
ANSI terminal, termios         existing VGA/PS/2/COM1 drivers
(Linux executable)             (fork's drivers.asm, via SysV calls)
```

Everything above the backends, `src/core/*.c`, is the **shared core**. It is one
set of source files compiled unchanged for both targets. It is freestanding
C99, which means:

- It includes only `<stddef.h>` and `<stdint.h>`.
- It calls no libc, OS or hardware function. `make freestanding` compiles it with the
  kernel's flags and proves `nm -u` is empty.
- It performs no I/O. Output leaves through a row callback; input enters through
  `glyph_key` / `glyph_click` / `glyph_action`.
- It allocates nothing. All state lives in one caller-owned `struct glyph_vm`
  (47 416 bytes on x86-64; `glyph limits` prints it).
- It uses no floating point (the bare-metal build uses `-mgeneral-regs-only`).
- It never recurses without a bound. The parser's recursion is capped by the nesting
  limit (16). The interpreter does not recurse at all: it runs on an explicit frame stack.

## Modules (mapping to the required list)

| # | Module | File | Notes |
|---|---|---|---|
| 1 | Lexer | `src/core/lex.c` | Pull-based, one token of state, line/column tracking, token limit. |
| 2 | Parser | `src/core/parse.c` | Recursive descent with depth checks; declarations handled at load. |
| 3 | IR | `glyph.h` `glyph_node` | 28-byte nodes in a fixed array; sequences linked by `next`; bodies by `body`. Operands carry 2-bit kinds (int / string-pool ref / variable symbol). |
| 4 | Interpreter | `src/core/exec.c` | Explicit frame stack (`GLYPH_MAX_FRAMES` = 17 × 17), step budget, saturating arithmetic. |
| 5 | Turtle / control state | `src/core/turtle.c` | 16.16 fixed-point position, integer degree sine table, clamp at ±4096 cells. |
| 6 | Application state store | `glyph_state[]` in `glyph_vm`, `exec.c` | Slots allocated at load time; dirty flags record changes per input. |
| 7 | UI component model | `src/core/ui.c` | PANEL / LABEL / BUTTON / FIELD; focus order = declaration order. |
| 8 | Event dispatcher | `src/core/ui.c` | Key, click and named-action inputs; one handler per input, run to completion. |
| 9 | Rendering interface | `glyph_render()` + `glyph_row_fn` | Composes canvas + components one row at a time into `vm->row` (80 cells) and hands each row to the caller. |
| 10 | Platform/backend interface | `include/glyph_backend.h`, `src/core/runner.c` | `struct glyph_backend` (poll_key, idle, begin/end_frame, draw_row, status); the shared runner loop. |

## Data flow and lifetime

1. `glyph_load(vm, src, len, diag)` resets the vm and lexes, parses and links.
   On failure the vm is left empty and one diagnostic is returned; the
   earliest-positioned link error wins. The source buffer is not retained.
2. `glyph_start(vm, result)` runs the main sequence with a fresh budget. Errors
   fault the session (later inputs return `E_FAULTED`); rendering still works.
3. Each input clears the per-variable dirty flags, runs at most one handler with
   a fresh budget, and reports `status`, `action`, `steps`, `changed`, `focus`
   and a diagnostic.
4. `glyph_render(vm, fn, user)` may be called at any time; it does not
   modify program state (it uses `vm->row` as scratch).

## Memory (sizes on x86-64)

| Item | Size |
|---|---|
| IR nodes: 1024 × 28 B | 28 672 |
| Symbols: 128 × 28 B | 3 584 |
| String pool | 4 096 |
| Components: 32 × 28 B | 896 |
| State: 32 × 76 B | 2 432 |
| Canvas: 80 × 24 cells × 2 B | 3 840 |
| Frame stack: 289 × 12 B | 3 468 |
| Lexer, row scratch, misc. | ≈ 500 |
| **`sizeof(glyph_vm)`** | **47 416** |

Code size (freestanding, x86-64): 37.6 KB at `-O2`, 26.7 KB at `-Os`.
Stack, from `gcc -fstack-usage`, is about 320 bytes per nesting level of
parser recursion. The worst case is about 16 × 320 B plus about 1 KB of fixed
frames, under 8 KiB in total (the kernel's BSP stack is 64 KiB).

The canvas is the program's own drawing surface. It is part of the
application state and is needed for deterministic semantics and golden tests.
Backends receive composed rows and add no frame buffer of their own; see
[DECISIONS.md](DECISIONS.md#d6).

## Determinism

There are no clocks, randomness, floating point, pointers in output, or
unordered containers. Given the same source and the same input sequence, every
target produces the same state, frames and transcript. The conformance
harness (`script.c`) is itself in the shared core so that transcripts are
compared byte for byte across targets.

## Untrusted input

Model-generated programs and event scripts are treated as hostile:

- Every read is bounds-checked against the source length. NUL and other control
  bytes are lexical errors.
- Every table has a hard limit with its own error code (SPEC §9).
- Execution is bounded three ways: the step budget covers statements, loop
  iterations and every drawn cell; REPEAT and FD have range limits; call depth is
  capped.
- No program value is ever used as a pointer or as an unchecked index. Symbol,
  slot and component indices come from the linker, not from program text.
- Integer arithmetic saturates and is computed in 64 bits before clamping.
- Fuzzing: `make fuzz` (libFuzzer + ASan + UBSan) over program + event script.
  `make sanitize` and `make valgrind` run the full test suite instrumented.

## Embedding / model-integration interface

An external caller, such as a future model adapter, needs only `glyph.h`:

| Need | Call |
|---|---|
| Submit a bounded program; parse success or failure; diagnostics | `glyph_load` → `glyph_diag {code, line, col, msg}` |
| Execution status | `glyph_start` → `glyph_result` |
| Event results | `glyph_key` / `glyph_click` / `glyph_action` → `glyph_result` (`status`, `action`, `steps`, `changed`, `focus`, `diag`) |
| Application state changes | `glyph_state_count/name/type/int/str/dirty/find` |
| Renderable output | `glyph_render` (rows of VGA-format cells) |
| All of the above in one call, as text | `glyph_transcript(vm, src, len, script, slen, buf)` |

Glyph knows nothing about Qwen, tokenisation or the inference engine. A caller
that wants to "ask the model for an app" passes the model's text to `glyph_load`
and feeds the diagnostic back as text. `glyph check` prints the same diagnostic
in `file:line:col: error CODE: message` form.

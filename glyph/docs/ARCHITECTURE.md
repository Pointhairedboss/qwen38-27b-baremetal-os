# Glyph architecture

## Shape

```
Glyph source (untrusted, ≤ 16 KiB)
      │
      ▼
lex.c ── parse.c (pre-scan arities, compile, link) ──► bytecode + tables   (glyph_load)
                                                          │
                                                          ▼
                                     exec.c   stack VM: value / local / frame stacks, step budget
                                     turtle.c canvas + turtle (fixed point)
                                     ui.c     globals, components, focus, events, render
                                     cp437.c  VGA character set <-> Unicode (UTF-8 in and out)
                                                          │
                    ┌─────────────────────────────────────┴───────────────────┐
                    │ runner.c: interactive loop over                         │ script.c: event script
                    │ struct glyph_backend                                    │ → transcript (conformance)
                    ▼                                                         ▼
     ┌──────────────┴───────────────┐
     ▼                              ▼
src/host/term.c                src/baremetal/glyph_bm.c
ANSI terminal, termios,        cells copied to VGA text memory;
UTF-8 (ASCII if not)           PS/2/COM1 input via the fork's drivers
```

Everything above the backends, `src/core/*.c`, is the **shared core**: one set
of source files compiled unchanged for both targets. It is freestanding C99:

- It includes only `<stddef.h>` and `<stdint.h>`.
- It calls no libc, OS or hardware function. `make freestanding` compiles it with
  the kernel's flags and checks that `nm -u` is empty.
- It performs no I/O. Output leaves through a row callback; input enters through
  `glyph_key`, `glyph_click` and `glyph_action`.
- It allocates nothing. All state lives in one caller-owned `struct glyph_vm`
  (85 128 bytes on x86-64; `glyph limits` prints the size).
- It uses no floating point (the bare-metal build uses `-mgeneral-regs-only`).
- **Execution never recurses in C.** LOGO recursion uses the VM's own frame,
  local and value stacks, so the host stack use of execution is constant,
  about 0.6 KiB. Only the compiler recurses, and that recursion is bounded by the
  nesting limits.

## Modules (mapping to the required list)

| # | Module | File | Notes |
|---|---|---|---|
| 1 | Lexer | `src/core/lex.c` | Pull-based, one token of state, line/column, token limit, UCBLogo minus rule. |
| 2 | Parser / compiler | `src/core/parse.c` | Pass 1 pre-scans `TO` headers for arities. Pass 2 uses recursive descent for statements and precedence climbing for expressions, emitting bytecode directly. The link step resolves arrays and components and reports the earliest error. |
| 3 | IR | `glyph_insn` in `glyph.h`, opcodes in `glyph_internal.h` | 8-byte stack-machine instructions `{op, col, line, arg}`, at most 4 095. Units (main, one per `TO`, one per `ON`) record entry point, input count and local-slot count. |
| 4 | Interpreter | `src/core/exec.c` | Bytecode loop with explicit stacks: 128 values, 256 local slots, 64 call frames. Step budget per entry. |
| 5 | Turtle / control state | `src/core/turtle.c` | 16.16 fixed-point position, integer sine table, clamp at ±4096 cells, `SETXY` Bresenham. |
| 6 | Application state store | globals `glyph_state[]`, arrays `cells[]` | Global slots and array storage are allocated at load. Dirty flags record which globals change per input. |
| 7 | UI component model | `src/core/ui.c` | PANEL / LABEL / BUTTON / FIELD; focus order = declaration order. |
| 8 | Event dispatcher | `src/core/ui.c` | Key, click and named-action inputs; one handler unit per input, run to completion. |
| 9 | Rendering interface | `glyph_render()` + `glyph_row_fn` | Composes the canvas and components row by row into `vm->row` and hands each row to the caller. |
| 10 | Platform/backend interface | `include/glyph_backend.h`, `src/core/runner.c` | `struct glyph_backend` (poll_key, idle, begin/end_frame, draw_row, status) and the shared runner loop. |
| 11 | Characters | `src/core/cp437.c` | Cells hold VGA (code page 437) codes. UTF-8 string literals map to them, and transcripts and the terminal get UTF-8 back (SPEC §8.1, D32–D34). Line joins are in `turtle.c`. |

## Compilation

1. **Pre-scan.** One lexing pass records `TO name :a :b …` arities, so a call
   can be parsed (LOGO parses calls by arity) before its definition appears.
2. **Compile.** Statements become stack code:
   - Command inputs are pushed, then the command instruction consumes them.
   - Prefix reporters and procedure calls take whole expressions as inputs.
   - Infix operators use precedence climbing.
   - `REPEAT` uses a counter in a per-call local slot, one per loop-nesting level.
   - `IF`/`IFELSE`/`WHILE` become conditional jumps.
   - `TO` and `ON` bodies are emitted inline behind a jump.
3. **Scope resolution.** Outside procedures, names resolve to globals as they are
   compiled. Inside a `TO` body, names resolve when its `END` is reached (`LOCAL`
   may appear anywhere): an input or `LOCAL` of that procedure becomes a local
   slot, anything else a global.
4. **Link.** Array references are resolved. Variables read but never assigned,
   unknown arrays and components, missing action handlers, components off screen
   and array/variable name clashes are reported. The earliest one in source
   order wins.

`glyph dump FILE` prints the bytecode, units, components, globals and arrays.

## Execution

`exec_entry(vm, unit)` resets the three stacks and the step counter, enters the
unit's frame, and loops over instructions:

- **Expression** instructions (push, load, arithmetic, comparison, `ITEM`, `WORD`,
  turtle queries) are free.
- **Statement** instructions cost 1 step each, and so do calls. Loop instructions
  cost 1 per iteration or condition test. Drawing adds 1 per cell.
- A call moves its arguments from the value stack into fresh local slots and
  pushes a frame. `OUTPUT` and `RET` pop the frame and, for reporter calls,
  push the result.
- Every cycle in the control flow passes a charged instruction, so the budget
  bounds total work.

## Data flow and lifetime

1. `glyph_load(vm, src, len, diag)` resets the vm, then pre-scans, compiles and
   links. On failure the vm is left empty and one diagnostic is returned. The
   source buffer is not retained.
2. `glyph_start(vm, result)` runs unit 0 (main) with a fresh budget. An error
   faults the session (later inputs return `E_FAULTED`); rendering still works.
3. Each input clears the dirty flags and runs at most one handler unit with a fresh
   budget. It reports `status`, `action`, `steps`, `changed`, `focus` and a
   diagnostic.
4. `glyph_render(vm, fn, user)` may be called at any time. It does not modify
   program state (it uses `vm->row` as scratch).

## Memory (sizes on x86-64)

| Item | Size |
|---|---|
| Bytecode: 4096 × 8 B | 32 768 |
| Local slots: 256 × 72 B | 18 432 |
| Value stack: 128 × 72 B | 9 216 |
| Array cells: 2048 × 4 B | 8 192 |
| Symbols: 128 × 28 B | 3 584 |
| String pool | 4 096 |
| Canvas: 80 × 24 cells × 2 B | 3 840 |
| Globals: 32 × 76 B | 2 432 |
| Components, units, frames, arrays, lexer, parser, misc. | ≈ 2 600 |
| **`sizeof(glyph_vm)`** | **85 128** |

A value is 72 bytes: a type, an integer and an inline 63-character string. Values
never point at other storage, so no garbage collection is needed.

Code size (freestanding, x86-64): 53.7 KB at `-O2`, 38.3 KB at `-Os`.

Host stack (from `gcc -fstack-usage`):
- **Execution** is constant: `exec_entry` with its helpers is about 0.6 KiB,
  independent of LOGO recursion depth.
- **Loading** recurses for nesting. Each statement level costs about 370 B, and
  each expression level about 690 B (precedence climbing adds up to four frames
  per parenthesis level). The worst case of 16 nested lists holding a 16-deep
  expression is about 18 KiB. The kernel's BSP stack is 64 KiB.

The canvas is the program's own drawing surface. It is part of the application
state, and the owner confirmed it (DECISIONS D6). Backends receive composed rows
and add no frame buffer of their own.

## Determinism

There are no clocks, randomness, floating point, pointers in output, or
unordered containers. Given the same source and the same input sequence, every
target produces the same state, frames and transcript. The conformance harness
(`script.c`) is itself part of the shared core, so transcripts can be compared
byte for byte across targets.

## Untrusted input

Model-generated programs and event scripts are treated as hostile:

- Every source read is bounds-checked. NUL and control bytes are lexical errors.
- Every table and stack has a hard limit with its own error code (SPEC §11).
- Execution is bounded by the step budget, which covers statements, calls, loop
  iterations and drawn cells. REPEAT, FD and coordinates are range-checked;
  call depth, local space and value stack are capped.
- Bytecode is produced only by the compiler. Jump targets, slot indices and
  array indices come from it, never from program values. Array element access is
  range-checked at run time.
- Integer arithmetic is computed in 64 bits and saturated. Division and remainder
  by zero are errors.
- `make fuzz` runs libFuzzer + ASan + UBSan over program + event script.
  `make sanitize` and `make valgrind` run the full suite instrumented.

## Embedding / model-integration interface

An external caller, such as a future model adapter, needs only `glyph.h`:

| Need | Call |
|---|---|
| Submit a bounded program; parse success/failure; diagnostics | `glyph_load` → `glyph_diag {code, line, col, msg}` |
| Execution status | `glyph_start` → `glyph_result` |
| Event results | `glyph_key` / `glyph_click` / `glyph_action` → `glyph_result` (`status`, `action`, `steps`, `changed`, `focus`, `diag`) |
| Application state changes | `glyph_state_*`, `glyph_array_*` |
| Renderable output | `glyph_render` (rows of VGA-format cells) |
| Budget for heavier computation | `glyph_set_budget` (up to 10 000 000 steps per entry) |
| All of the above in one call, as text | `glyph_transcript(vm, src, len, script, slen, buf)` |
| Pull a program out of a model's reply (prose, Markdown, labels) | `glyph_extract(vm, text, len, out, cap, &outlen, &diag)` (D27) |

The core knows nothing about Qwen, tokenisation or the inference engine. A caller
passes the model's text to `glyph_extract` (or `glyph_load`) and feeds the
diagnostic back to the model as text. `glyph check` prints the same diagnostic as
`file:line:col: error CODE: message`; `glyph extract` does the same for a reply.
The inference kernel's `/draw` does exactly this (`src/baremetal/glyph_draw.c`,
docs/DRAW.md).

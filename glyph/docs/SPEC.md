# Glyph language specification — v0.2 (text-UI look)

Glyph is a small, deterministic, **Turing-complete LOGO** for text-cell interactive
applications. It keeps the LOGO of [nanochess/bootLogo](https://github.com/nanochess/bootLogo):
turtle commands, `REPEAT`, `TO … END` and bracketed lists. It follows UCBLogo for
procedure inputs, `OUTPUT`/`STOP`, `IF`/`IFELSE`/`WHILE`, infix arithmetic and
prefix reporters. On top of that it adds text-cell drawing, a small component
model (panels, labels, buttons, fields) and event handlers.

Everything is bounded. Each resource has a documented limit with its own error.
Every execution entry has a step budget. Untrusted programs fail with a
diagnostic and never crash or hang the host.

This document is normative for the shared interpreter in `src/core/`. Differences
from bootLogo and UCBLogo are marked **(deviation)** and recorded in
[DECISIONS.md](DECISIONS.md).

## 1. Execution model

A program is loaded in three steps, then becomes interactive:

1. **Load.** Lex, parse and compile the whole program to bytecode. Declarations
   (`SCREEN`, `PANEL`, `LABEL`, `BUTTON`, `FIELD`, `ARRAY`, `TO`, `ON`) are
   processed here. Load either succeeds or fails with one diagnostic, and nothing
   runs.
2. **Start.** The top-level statements (the *main* sequence) run once, in order.
3. **Interactive.** Each input (key, click or named action) is dispatched to at most
   one handler, which runs to completion. The frame is then re-rendered.

Every execution entry (start, or one input) has its own step budget. Execution is
synchronous; there are no timers, threads or queued events.

The output is a **frame** of `W × H` text cells (default 80×24). Each cell has an
ASCII character and a VGA colour attribute. The frame is the persistent **canvas**,
written by drawing commands, with **components** composited on top.

## 2. Lexical structure

- Source is ASCII. Allowed bytes are printable `0x20–0x7E`, tab, CR and LF;
  anything else is `E_CHAR`. There are two exceptions:
  - Comments may also contain `0x80–0xFF`, so UTF-8 in comments is accepted and
    never rendered.
  - String literals may contain UTF-8 (see Strings).
- Whitespace separates tokens; line breaks have no meaning. `;` starts a comment
  that runs to the end of the line.
- **Words**: `[A-Za-z][A-Za-z0-9_]*`, at most 15 characters, case-insensitive
  (folded to upper case).
- **Numbers**: decimal integers, −999 999 999 … 999 999 999.
- **Strings**: `"…"` on one line, at most 63 characters, no escapes. Case is
  preserved. **(deviation)** UCBLogo's unterminated `"word` quoting is not used.
  - A string may hold printable ASCII except `"`, and UTF-8.
  - Each UTF-8 character becomes one screen character: the VGA text-mode
    character set (code page 437, §8.1).
  - A character VGA has no glyph for, such as an emoji, becomes `?`. So
    `"═══ ♥ café ░▒▓█"` shows as written, and the 63-character limit counts
    screen characters.
  - Malformed UTF-8 (truncated, overlong, a surrogate, or above U+10FFFF) is
    `E_CHAR`.
- **Variables**: `:name` reads a variable.
- **Punctuation**: `[ ] ( )` and the operators `+ - * / = <> < > <= >=`.
- **Minus** follows UCBLogo's spacing rule. A `-` that follows whitespace, `(`, `[`
  or the start of the text, and is immediately followed by a non-space character,
  begins a new input: `SETXY 10 -5` has two inputs, and `-5` there is a negative
  literal. Any other `-` subtracts: `:a - 5`, `:a-5`, `10-4`.
- **Reserved words** (all command, reporter and declaration names) can never be
  used as names of procedures, inputs, variables, arrays, components or actions.

## 3. Values, variables and scope

A value is an **integer** (−999 999 999 … 999 999 999) or a **string** (≤ 63
printable characters). Integer arithmetic saturates at the range limits.
Booleans are integers: comparisons report `1` or `0`, `TRUE` is 1, `FALSE` is 0,
and conditions treat any non-zero integer as true. **(deviation)** UCBLogo uses
the words `true`/`false`.

**Global variables** exist when the program contains `SET name …` anywhere (or
`FIELD name …`). They persist across inputs. A global assigned only by `SET`
starts *unset*, and reading an unset variable is `E_UNSET`. Reading a global that
is never assigned anywhere is a load error (`E_UNDEFINED`).

**Local variables** are a procedure's inputs and the names it declares with
`LOCAL`. Each call has its own copies. Inputs start with the call's arguments;
`LOCAL`s start unset.

**Scope is lexical** **(deviation**; UCBLogo is dynamically scoped**)**. Inside a
`TO` body, a name that is an input or a `LOCAL` of that procedure refers to the
local. Every other name, in any procedure, handler or main, refers to the global.
`LOCAL` applies to the whole procedure body, wherever it appears.

**Arrays** are declared at the top level with `ARRAY name size` (1–2048 integers,
initially 0). They are global, persist across inputs, are indexed from 1, and
hold integers only. A name cannot be both an array and a variable.
**(deviation)** UCBLogo arrays are first-class values.

## 4. Grammar

```
program   := { decl | stmt }
decl      := "SCREEN" INT INT
           | "PANEL"  id INT INT INT INT STRING
           | "LABEL"  id INT INT ( INT | STRING | VAR )
           | "BUTTON" id INT INT STRING action
           | "FIELD"  id INT INT INT action
           | "ARRAY"  name INT
           | "TO" name { VAR } { stmt } "END"
           | "ON" action body
body      := "[" { stmt } "]" | stmt
stmt      := "[" { stmt } "]"
           | command { input }                  ; fixed number of inputs, see §5–§8
           | procname { input }                 ; as many inputs as the TO lists
           | "REPEAT" input body | "WHILE" input body
           | "IF" input body [ "[" { stmt } "]" ] | "IFELSE" input body body
           | "STOP" | "OUTPUT" input | "LOCAL" name
input     := expr
expr      := operand { binop operand }          ; precedence below
operand   := INT | STRING | VAR | "(" expr ")" | "-" operand
           | reporter { input } | procname { input }
```

Declarations are only allowed at the top level (`E_TOPLEVEL`). `INT`s in
declarations must be literals. `OUTPUT` and `LOCAL` are only allowed inside `TO`.

**Precedence** (loosest first): comparisons `= <> < > <= >=`, then `+ -`, then
`* /`, then unary minus. All binary operators are left associative.

**Prefix inputs are whole expressions** (UCBLogo rule). A command, reporter or
procedure reads each input as a complete expression. `FD :x + 1` moves `:x + 1`,
`sq :n - 1` calls `sq` with `:n - 1`, and `ITEM v 1 + ITEM v 5` means
`ITEM v (1 + ITEM v 5)`. Use parentheses to end an input early:
`(ITEM v 1) + ITEM v 5`.

## 5. Procedures and control

| Form | Meaning |
|---|---|
| `TO name :a :b … END` | Define a procedure with inputs. Calls may appear before the definition. |
| `name x y …` | Call it. Exactly as many inputs as it declares (`not enough inputs` / `too many inputs` at load). |
| `OUTPUT x` / `OP x` | Return `x` from the current procedure. A call used as a value must `OUTPUT` (`E_TYPE` otherwise). In command position the output is discarded **(deviation**: UCBLogo errors**)**. |
| `STOP` | Return without a value. In main or a handler, it ends that execution entry. |
| `LOCAL name` | Make `name` local to the procedure (see §3). |
| `REPEAT n body` | Run `body` `n` times, 0 ≤ n ≤ 10 000. Nestable; each call has its own counters. |
| `WHILE cond body` | Run `body` while `cond` is non-zero. The condition is an expression, not a list **(deviation)**. |
| `IF cond body` | Run `body` if `cond` is non-zero. `IF cond [a] [b]` (two lists) is `IFELSE` (UCBLogo). |
| `IFELSE cond body1 body2` | Run one of two bodies. |

Calls nest at most 64 deep. Recursion is the normal way to write LOGO.

## 6. Reporters (expression functions)

| Reporter | Value |
|---|---|
| `TRUE`, `FALSE` | 1, 0 |
| `AND a b`, `OR a b`, `NOT a` | 1 or 0. Both inputs are always evaluated. |
| `REMAINDER a b` | `a` mod `b` with the sign of `a`; `E_RANGE` if `b` = 0 |
| `a / b` | Integer division truncating toward zero; `E_RANGE` if `b` = 0 |
| `WORD a b` | String concatenation (numbers in decimal); `E_RANGE` if over 63 characters |
| `NUMBER v` | `v` as an integer. `v` is an integer, or a string holding one: optional spaces, an optional sign, decimal digits, optional spaces. Saturates at ±999 999 999. Anything else is `E_TYPE`. Field text is a string, so `(NUMBER :a) + (NUMBER :b)` adds two typed numbers. |
| `NUMBERP v` | 1 if `NUMBER v` would succeed, else 0 (UCBLogo's name). |
| `ITEM name i` | Element `i` of an array (1-based); `E_RANGE` outside 1..size |
| `XCOR`, `YCOR`, `HEADING` | Turtle cell column, cell row, heading in degrees |
| procedure call | The value the procedure `OUTPUT`s |

`=` and `<>` compare any two values: equal only if same type and same value.
`< > <= >=` and arithmetic need integers (`E_TYPE`).

## 7. Turtle commands

| Command | Meaning |
|---|---|
| `FD n` / `BK n` | Move `n` unit steps forward or back, −1000 ≤ n ≤ 1000. Aliases `FORWARD`, `BACK`. |
| `RT n` / `LT n` | Turn clockwise or anticlockwise by `n` degrees (mod 360). Aliases `RIGHT`, `LEFT`. |
| `PU` / `PD` | Pen up or down. Aliases `PENUP`, `PENDOWN`. |
| `HOME` | Move to the home cell (`W/2`, `H/2`) and face north. Draws if the pen is down. |
| `SETXY x y` | Move to cell (x, y), −1000 ≤ x, y ≤ 1000. Draws a line if the pen is down. **(deviation)** Screen coordinates (origin top-left, y down), not UCBLogo's centred y-up system. |
| `SETHEADING h` / `SETH h` | Face `h` degrees (0 = north, clockwise). |
| `CLEARSCREEN` / `CS` | `CLEAR`, then home with heading 0 and pen down. |
| `SETCOLOR n` | Foreground colour 0–15; background unchanged. |

**Geometry.** One turtle step is one cell. Position is kept in 16.16 fixed point,
clamped to ±4096 cells. Directions come from an integer sine table (one entry per
degree); there is no floating point. `FD n` performs `|n|` steps. On each step, if
the pen is down, it plots the nearest cell (`floor(p + 0.5)`) with `*` in the
current colour, then moves one unit. `SETXY` and `HOME` plot the Bresenham line
from the current cell up to, but not including, the target cell. Neither command
plots the final position, matching bootLogo's draw-then-advance loop. Text cells
are about twice as tall as they are wide, so figures look vertically stretched.

## 8. Drawing, layout, state and events

Drawing commands take expression inputs. Coordinates must be within ±1000
(`E_RANGE`), and output is clipped to the screen.

| Command | Meaning |
|---|---|
| `CLEAR` | Fill the canvas with spaces in the current colour; the turtle is unchanged. |
| `COLOR fg bg` | Current colour: `fg` 0–15, `bg` 0–7 (VGA palette). Initially 7 on 0. |
| `TEXT x y v` | Write a value at (x, y), with no wrapping. |
| `LINE x1 y1 x2 y2` | Bresenham line including both ends: `─` horizontal, `│` vertical, `\` or `/` exact diagonal, `*` otherwise. Horizontal and vertical lines join the lines they meet (§8.1). |
| `RECT x y w h` | Filled area of spaces in the current colour (0 ≤ w, h ≤ 1000). |
| `BOX x y w h` | Outline with `┌ ─ ┐ │ └ ┘` (2 ≤ w, h ≤ 1000), joining the lines it meets (§8.1). |
| `SET name v` | Assign a local or global variable (see §3). |
| `ADD name n` | Add to an integer variable (saturating). `E_UNSET` if unset, `E_TYPE` if it holds a string. |
| `SETITEM name i v` | Store integer `v` at index `i` of an array. |
| `ENABLE id` / `DISABLE id` | Enable or disable a `BUTTON` or `FIELD`; other kinds are `E_KIND` at load. |

VGA palette: 0 black, 1 blue, 2 green, 3 cyan, 4 red, 5 magenta, 6 brown, 7 light grey,
8 dark grey, 9 light blue, 10 light green, 11 light cyan, 12 light red, 13 light magenta,
14 yellow, 15 white.

**Components** are declared at the top level with literal positions:

| Declaration | Meaning |
|---|---|
| `SCREEN w h` | Screen size, 1 ≤ w ≤ 80, 1 ≤ h ≤ 24, at most once. |
| `PANEL id x y w h "title"` | A framed area with the title at (x+2, y). Its interior takes the panel colour; drawings inside it stay visible. A top-level panel is a window: double frame `╔═╗` and a drop shadow. A panel inside another panel is a group: single frame `┌─┐`, no shadow. |
| `LABEL id x y value` | Text. A `:var` value shows that global's current value at every render. |
| `BUTTON id x y "caption" action` | Activating it fires `action`. |
| `FIELD id x y width action` | One-line text input (width 1–63). Its text is the global string variable `id`. ENTER fires `action`. |
| `ON action body` | Handler for a named action, at most one per action. Every action named by a control must have one (`E_UNDEFINED`). |

Component origins must lie on the screen (`E_RANGE`); the rest is clipped.
Components are drawn over the canvas in declaration order. Control state shows in
the characters as well as the colours, so it is visible in a transcript:

| | Button | Field |
|---|---|---|
| normal | `▐ OK ▌` | `▐text····▌` |
| focused | `▐►OK◄▌` | `►text_···◄` (`_` is the cursor) |
| disabled | `░ OK ░` | `░text····░` |

Labels take the background of whatever is under them (a panel, or the canvas).

**Focus and inputs.** Focusable components are enabled buttons and fields, in
declaration order. After start, focus is on the first one. `DISABLE` on the
focused component moves focus to the next focusable one, or to none.

| Input | Effect |
|---|---|
| key TAB (9) | Focus the next focusable component (wrapping). |
| key ENTER (10 or 13) | Focused button or field fires its action. |
| key SPACE (32) | Focused button fires its action; a focused field inserts a space. |
| key BACKSPACE (8 or 127) | Focused field: delete the last character. |
| key printable (33–126) | Focused field: append, unless the field is full. |
| click `id` | Enabled button: focus it and fire it. Enabled field: focus it. |
| action `name` | Fire the handler for `name`; ignored if there is none. |

Every input reports `ok`, `ignored` or an error. The result also gives the fired
action, the steps used, the globals whose values changed, and the focus.

### 8.1 Characters and the default look

A screen cell holds one character of the **VGA text-mode character set (code
page 437)** and a colour byte, exactly as VGA text memory does.
- **Bare metal:** cells are copied to the screen unchanged.
- **Linux:** cells are written as UTF-8; terminals whose locale is not UTF-8
  get ASCII line art instead.
- **Transcripts** are UTF-8, so they read the same as the screen.

The set includes single, double and mixed box-drawing lines, blocks
`█ ▀ ▄ ▌ ▐`, shades `░ ▒ ▓`, arrows, card suits and smileys.

**Lines join.** A horizontal or vertical `LINE`, or a `BOX` edge, drawn over
another line merges with it:
- crossing lines make `┼`;
- a line ending on a box edge makes `├`;
- boxes sharing an edge make `┬` and `┴`.

Each cell keeps one style per axis. A new stroke's style wins on its own axis,
and every combination is in the character set.

**The default look** is a blue-desktop text UI in the style of 1990s DOS
applications (Turbo Vision, Norton Commander), and needs nothing from the
program:

| Part | Look |
|---|---|
| Window | white double frame on blue |
| Group | single frame |
| Title | yellow |
| Labels | white |
| Buttons | cyan faces; focused green |
| Fields | grey faces with a blinking cursor |
| Status line | cyan |

Bare metal centres the app on a shaded desktop, with the window's shadow cast
on it. The VGA attribute values are the `GLYPH_ATTR_*` constants in `glyph.h`.

## 9. Errors

Errors carry a stable code, a line and column, and a message. A load error stops
the load. A runtime error stops the current execution entry, and changes made
before it are kept (no rollback). An error during start faults the session:
inputs then return `E_FAULTED`. An error in a handler affects only that input.

Codes: `E_ARG E_SOURCE_SIZE E_CHAR E_STRING E_STR_LEN E_NAME_LEN E_NUMBER E_TOKENS
E_SYNTAX E_UNCLOSED E_NEST E_CODE E_NAMES E_STRPOOL E_UNKNOWN E_REDEFINE E_TOPLEVEL
E_UNDEFINED E_KIND E_LIMIT E_RANGE E_TYPE E_UNSET E_BUDGET E_DEPTH E_FAULTED E_STATE`.

## 10. Turing completeness

Glyph has:

- conditional branching (`IF`, `IFELSE`);
- unbounded iteration and recursion (`WHILE`, recursive procedures with inputs and
  `OUTPUT`);
- read/write memory (variables and arrays).

That makes the language Turing complete in the usual sense. Like every real
implementation, it runs on a finite machine: the limits in §11 are that machine,
and exceeding one gives a deterministic error. The repository demonstrates this
rather than just asserting it:

- `examples/turing.glyph` is a table-driven Turing machine simulator. It runs the
  2-, 3- and 4-state busy beavers.
- `examples/rule110.glyph` computes Rule 110, a cellular automaton proven Turing
  complete (Cook, 2004).

Both are checked cell for cell against independent Python implementations by
`tests/reference/check_tc.py` (`make reference`).

## 11. Limits

| Limit | Value | Error |
|---|---|---|
| Program size | 16 384 bytes | `E_SOURCE_SIZE` |
| Tokens | 4 096 | `E_TOKENS` |
| Nesting of lists and bodies | 16 | `E_NEST` |
| Expression nesting (parentheses, inputs, unary minus) | 16 | `E_NEST` |
| Bytecode instructions | 4 095 | `E_CODE` |
| Distinct names / name length / string length | 128 / 15 / 63 | `E_NAMES` / `E_NAME_LEN` / `E_STR_LEN` |
| String literal pool | 4 096 bytes | `E_STRPOOL` |
| Procedures / handlers / components | 32 / 32 / 32 | `E_LIMIT` |
| Global variables | 32 | `E_LIMIT` |
| Arrays / array cells in total | 16 / 2 048 | `E_LIMIT` |
| Inputs + LOCALs + REPEAT counters per procedure | 32 | `E_LIMIT` |
| Procedure call depth | 64 | `E_DEPTH` |
| Local slots for all active calls | 256 | `E_DEPTH` |
| Values in flight (expression stack) | 128 | `E_DEPTH` |
| REPEAT count | 0 … 10 000 | `E_RANGE` |
| FD/BK distance; drawing coordinates | ±1000; ±1000 | `E_RANGE` |
| Steps per execution entry | 100 000 by default; the host may set up to 10 000 000 | `E_BUDGET` |

**Step accounting.** Each executed statement costs 1 step, and so does each
procedure call (command or reporter). Each `REPEAT` iteration and each `WHILE`
condition test costs 1. Each turtle step, line cell, rectangle or box cell, text
character and cleared cell costs 1. Expression operators are free; each
expression is bounded by the program text. Every cycle in the control flow passes
a charged instruction, so the budget bounds total work.

## 12. Example

```
SCREEN 40 10
SET count 0
PANEL main 0 0 40 10 "Counter"
LABEL value 2 2 :count
BUTTON inc 2 4 "+1" up
BUTTON dec 9 4 "-1" down
ON up ADD count 1
ON down IF :count > 0 [ADD count -1]

TO star :size
  REPEAT 5 [FD :size RT 144]
END
PU SETXY 30 6 PD
star 4
```

## Appendix A — Event scripts and transcripts (conformance)

Conformance tests drive the interpreter with an **event script** and compare the
**transcript** it produces. Both formats are implemented in the shared core
(`src/core/script.c`), so every target produces byte-identical transcripts.

Event script: one command per line; `#` starts a comment.

```
key TAB | ENTER | CR | SPACE | BS | DEL | ESC | <decimal code> | 'c'
type "text"          ; one key per character
click ID
action NAME
frame                ; dump the frame characters
attrs                ; dump the frame attributes (hex)
state                ; dump global variables and arrays
turtle               ; turtle position, heading, pen
```

Transcript: `glyph 0.2`, then `load:`, `main:` and one `@` line per script command,
then `end`. See `tests/corpus/*.expect`.

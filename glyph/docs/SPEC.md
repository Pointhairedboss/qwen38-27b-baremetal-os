# Glyph language specification — v0.1

Glyph is a small, deterministic language for text-cell interactive applications.
It keeps the LOGO control model of [nanochess/bootLogo](https://github.com/nanochess/bootLogo)
(turtle commands, `REPEAT`, `TO … END`, bracketed lists) and adds a minimal set of
drawing, layout, state and event commands. There are no general expressions,
no dynamic code execution and no unbounded constructs.

This document is normative for the shared interpreter in `src/core/`. Where Glyph
differs from bootLogo the difference is marked **(deviation)** and recorded in
[DECISIONS.md](DECISIONS.md).

## 1. Execution model

A Glyph program is loaded in three steps, then becomes interactive:

1. **Load.** Lex, parse and link the whole program. Declarations (`SCREEN`, `PANEL`,
   `LABEL`, `BUTTON`, `FIELD`, `TO`, `ON`) are processed here. Load either succeeds
   or fails with one diagnostic. Nothing is executed during load.
2. **Start.** The top-level statements (the *main* sequence) run once, in order.
3. **Interactive.** Each input (key, click or named action) is dispatched to at most
   one handler, which runs to completion. The frame is then re-rendered.

Every execution entry (start, or one input) has its own step budget. All
execution is synchronous; there are no timers, threads or queued events.

The output of a program is a **frame**: a grid of `W × H` text cells (default 80×24),
each with an ASCII character and a VGA colour attribute. The frame is the persistent
**canvas** (written by drawing commands) with **components** composited on top.

## 2. Lexical structure

- Source is ASCII. Allowed bytes: printable `0x20–0x7E`, tab, CR, LF. Anything else
  is error `E_CHAR`, except inside comments, which may also contain bytes
  `0x80–0xFF` (so UTF-8 punctuation in comments is accepted; it is never rendered).
- Whitespace (space, tab, CR, LF) separates tokens. Line breaks have no meaning.
- `;` starts a comment that runs to the end of the line.
- **Words**: `[A-Za-z][A-Za-z0-9_]*`, at most 15 characters. Words are
  case-insensitive (folded to upper case), like bootLogo's input.
- **Numbers**: decimal integers with an optional leading `-` touching the first
  digit (`-5`). Range −999 999 999 … 999 999 999.
- **Strings**: `"…"` on one line, printable ASCII except `"`, at most 63 characters.
  No escapes. Case is preserved.
- **Variable references**: `:name` reads state variable `name` (LOGO's "thing" syntax).
- **Lists**: `[` and `]`.

## 3. Values and state

A value is an **integer** (−999 999 999 … 999 999 999) or a **string** (≤ 63
printable characters). Integer arithmetic saturates at the range limits.

State is a fixed table of named variables. A variable exists if the program contains
`SET name …` anywhere, or declares `FIELD name …`. Variables declared only by `SET`
start *unset*; reading an unset variable is runtime error `E_UNSET`. A `FIELD`
variable starts as the empty string. Variables are global.

Reading a variable that is never assigned anywhere in the program is a load error
(`E_UNDEFINED`).

## 4. Grammar

```
program   := { decl | stmt }
decl      := "SCREEN" INT INT
           | "PANEL"  id INT INT INT INT STRING
           | "LABEL"  id INT INT value
           | "BUTTON" id INT INT STRING action
           | "FIELD"  id INT INT INT action
           | "TO" name { stmt } "END"
           | "ON" action body
body      := "[" { stmt } "]" | stmt
stmt      := "[" { stmt } "]"
           | "FD" num | "BK" num | "RT" num | "LT" num | "PU" | "PD"
           | "REPEAT" num body
           | "CLEARSCREEN" | "CLEAR" | "COLOR" num num | "SETCOLOR" num
           | "TEXT" num num value
           | "LINE" num num num num | "RECT" num num num num | "BOX" num num num num
           | "SET" name value | "ADD" name num
           | "ENABLE" id | "DISABLE" id
           | name                                   ; procedure call
num       := INT | VAR
value     := INT | STRING | VAR
```

`INT` in a declaration must be a literal. Declarations are only allowed at the top
level (not inside lists, bodies or procedures): `E_TOPLEVEL`. Reserved words cannot
be used as names.

Aliases: `FORWARD`=`FD`, `BACK`=`BK`, `RIGHT`=`RT`, `LEFT`=`LT`, `PENUP`=`PU`,
`PENDOWN`=`PD`, `CS`=`CLEARSCREEN`.

## 5. LOGO commands

| Command | Meaning |
|---|---|
| `FD n` / `BK n` | Move the turtle `n` unit steps forward / back. −1000 ≤ n ≤ 1000. |
| `RT n` / `LT n` | Turn clockwise / anticlockwise by `n` degrees (any integer, taken mod 360). |
| `PU` / `PD` | Pen up / pen down. |
| `REPEAT n body` | Run `body` `n` times, 0 ≤ n ≤ 10 000. Nestable. |
| `TO name … END` | Define a procedure with no parameters. Call it by writing `name`. |
| `CLEARSCREEN` | `CLEAR`, then home the turtle: centre, heading 0 (north), pen down. |
| `SETCOLOR n` | Set the foreground colour (0–15); background unchanged. |

**Turtle geometry.** One turtle step is one cell. The home position is cell
(`W/2`, `H/2`) (integer division). Heading is degrees clockwise from north; north is
toward row 0. Position is kept in 16.16 fixed point and is clamped to ±4096 cells.
`FD n` performs `|n|` steps; on each step, if the pen is down, the cell nearest the
current position is plotted with `*` in the current colour, then the turtle moves one
unit along its heading (backwards when `n < 0` or for `BK`). The final position is
not plotted (this matches bootLogo's draw-then-advance loop). Directions come from an
integer sine table with one entry per degree; there is no floating point.
Cell rounding is `floor(p + 0.5)`.

Text cells are about twice as tall as they are wide, so turtle figures look
vertically stretched. This is accepted in v0.1.

## 6. Drawing commands

All coordinates are cells, column `x` then row `y`, origin top-left. Coordinates must
be within ±1000 (`E_RANGE`); output is clipped to the screen.

| Command | Meaning |
|---|---|
| `CLEAR` | Fill the canvas with spaces in the current colour. The turtle is unchanged. |
| `COLOR fg bg` | Set the current colour: `fg` 0–15, `bg` 0–7 (VGA palette). Initial: 7 on 0. |
| `TEXT x y v` | Write value `v` starting at (x, y). No wrapping. |
| `LINE x1 y1 x2 y2` | Bresenham line including both ends. Character: `-` horizontal, `\|` vertical, `\` or `/` exact diagonal, otherwise `*`. |
| `RECT x y w h` | Fill a `w × h` area with spaces in the current colour (0 ≤ w, h ≤ 1000). |
| `BOX x y w h` | Outline: `+` corners, `-` and `\|` edges (2 ≤ w, h ≤ 1000). |

VGA palette: 0 black, 1 blue, 2 green, 3 cyan, 4 red, 5 magenta, 6 brown, 7 light grey,
8 dark grey, 9 light blue, 10 light green, 11 light cyan, 12 light red, 13 light magenta,
14 yellow, 15 white.

## 7. Layout and components

| Declaration | Meaning |
|---|---|
| `SCREEN w h` | Screen size, 1 ≤ w ≤ 80, 1 ≤ h ≤ 24. At most once. Default 80 × 24. |
| `PANEL id x y w h "title"` | Bordered box (like `BOX`) with the title drawn at (x+2, y). Interior is transparent. 2 ≤ w ≤ 80, 2 ≤ h ≤ 24. |
| `LABEL id x y value` | Text. If `value` is `:var`, the label shows the variable's *current* value at every render (empty while unset). |
| `BUTTON id x y "caption" action` | A button. Activating it fires `action`. |
| `FIELD id x y width action` | A one-line text input, 1 ≤ width ≤ 63. Its text is the string variable `id`. ENTER in the field fires `action`. |

Component positions are absolute. A component's origin must lie on the screen
(`E_RANGE`); the rest is clipped. Components are drawn over the canvas in
declaration order. Component ids are unique (`E_REDEFINE`).

**Rendering of controls.** Outer markers show the control state; this is visible in
plain text as well as in colour:

| State | Button | Field (width 6, text `bob`) |
|---|---|---|
| normal | `[ OK ]` | `[bob___]` |
| focused | `> OK <` | `>bob___<` |
| disabled | `( OK )` | `(bob___)` |

Theme attributes: panel border 0x0B, panel title 0x0F, label 0x0F, control 0x1F,
focused control 0x70, disabled control 0x08.

## 8. State and events

| Command | Meaning |
|---|---|
| `SET name value` | Assign an integer, string or another variable's value. |
| `ADD name n` | Add an integer to an integer variable (saturating). `E_TYPE` if the variable holds a string, `E_UNSET` if unset. |
| `ENABLE id` / `DISABLE id` | Enable / disable a `BUTTON` or `FIELD`. Other kinds: load error `E_KIND`. |
| `ON action body` | Define the handler for a named action. At most one handler per action. |

Every action named by a `BUTTON` or `FIELD` must have an `ON` handler (`E_UNDEFINED`).
Handlers may exist for actions no control names; an external caller can fire them.

**Focus.** Focusable components are enabled buttons and fields, in declaration
order. After *start*, focus goes to the first focusable component (or none).
`DISABLE` on the focused component moves focus to the next focusable one (wrapping),
or to none.

**Inputs.**

| Input | Effect |
|---|---|
| key TAB (9) | Focus the next focusable component (wrapping; from none, the first). |
| key ENTER (10 or 13) | Focused button or field: fire its action. |
| key SPACE (32) | Focused button: fire its action. Focused field: insert a space. |
| key BACKSPACE (8 or 127) | Focused field: delete the last character. |
| key printable (33–126) | Focused field: append, unless the text is already `width` long. |
| other keys | Ignored. (ESC is handled by the host runner as "exit".) |
| click `id` | Enabled button: focus it and fire its action. Enabled field: focus it. Otherwise ignored. |
| action `name` | Fire the handler for `name`; ignored if there is none. |

A result is reported for every input: `ok`, `ignored`, or an error with diagnostic.
The result also reports the fired action, steps used, the variables whose values
changed, and the focused component.

## 9. Errors and limits

Errors carry a stable code, a line and column, and a message. Load errors stop the
load. A runtime error stops the current execution entry; state changes made before
the error are kept (no rollback). A runtime error during *start* faults the session:
later inputs return `E_FAULTED`. A runtime error in a handler affects only that input.

| Limit | Value | Error |
|---|---|---|
| Program size | 16 384 bytes | `E_SOURCE_SIZE` |
| Tokens | 4 096 | `E_TOKENS` |
| Nesting depth (lists and bodies) | 16 | `E_NEST` |
| IR nodes (statements) | 1 023 | `E_NODES` |
| Distinct names | 128 | `E_NAMES` |
| Name length / string length | 15 / 63 | `E_NAME_LEN` / `E_STR_LEN` |
| String literal pool | 4 096 bytes | `E_STRPOOL` |
| Procedures / handlers | 32 / 32 | `E_LIMIT` |
| Components / state variables | 32 / 32 | `E_LIMIT` |
| Procedure call depth | 16 | `E_DEPTH` |
| REPEAT count | 0 … 10 000 | `E_RANGE` |
| FD/BK distance | −1000 … 1000 | `E_RANGE` |
| Steps per execution entry | 100 000 (caller may lower) | `E_BUDGET` |

**Step accounting.** Each executed statement costs 1 step; each REPEAT iteration
costs 1; each turtle step, line cell, rectangle cell, box cell, text character and
cleared cell costs 1. Budgets therefore bound wall-clock time as well as loop counts.

Error codes: `E_ARG E_SOURCE_SIZE E_CHAR E_STRING E_STR_LEN E_NAME_LEN E_NUMBER
E_TOKENS E_SYNTAX E_UNCLOSED E_NEST E_NODES E_NAMES E_STRPOOL E_UNKNOWN E_REDEFINE
E_TOPLEVEL E_UNDEFINED E_KIND E_LIMIT E_RANGE E_TYPE E_UNSET E_BUDGET E_DEPTH
E_FAULTED E_STATE`.

## 10. Example

```
SCREEN 40 10
SET count 0
PANEL main 0 0 40 10 "Counter"
LABEL value 2 2 :count
BUTTON inc 2 4 "+1" up
BUTTON dec 9 4 "-1" down
ON up ADD count 1
ON down ADD count -1
```

## Appendix A — Event scripts and transcripts (conformance)

Conformance tests drive the interpreter with an **event script** and compare the
**transcript** it produces. Both formats are implemented in the shared core
(`src/core/script.c`), so every target produces byte-identical transcripts.

Event script: one command per line; `#` starts a comment.

```
key TAB | ENTER | SPACE | BS | ESC | <decimal code> | 'c'
type "text"          ; one key per character
click ID
action NAME
frame                ; dump the frame characters
attrs                ; dump the frame attributes (hex)
state                ; dump all state variables
```

Transcript: `glyph 0.1`, then `load:`, `main:` and one `@` line per script command,
then `end`. See `tests/corpus/*.expect` for examples.

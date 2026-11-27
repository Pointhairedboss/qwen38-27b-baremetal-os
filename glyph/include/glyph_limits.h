/* glyph_limits.h — every resource bound of the Glyph shared core.
 *
 * All storage is static inside struct glyph_vm; nothing is allocated at run
 * time. Changing a value here changes sizeof(glyph_vm) and the documented
 * limits in docs/SPEC.md section 11; keep both in step.
 *
 * The language is Turing complete; these limits are the finite machine it
 * runs on (as every real implementation is), and each one fails with its own
 * deterministic error instead of crashing or hanging.
 */
#ifndef GLYPH_LIMITS_H
#define GLYPH_LIMITS_H

#define GLYPH_VERSION         "0.2"

/* Source and compilation */
#define GLYPH_MAX_SOURCE      16384   /* bytes of program text */
#define GLYPH_MAX_TOKENS      4096    /* tokens per program */
#define GLYPH_MAX_NEST        16      /* nested lists/bodies */
#define GLYPH_MAX_EXPR_DEPTH  16      /* nested parentheses, reporter inputs, unary minus */
#define GLYPH_MAX_CODE        4096    /* bytecode instructions; index 0 is reserved */
#define GLYPH_MAX_NAMES       128     /* distinct identifiers */
#define GLYPH_MAX_NAME_LEN    15
#define GLYPH_MAX_STR_LEN     63
#define GLYPH_MAX_STRPOOL     4096    /* bytes for string literals */

/* Program objects */
#define GLYPH_MAX_PROCS       32
#define GLYPH_MAX_HANDLERS    32
#define GLYPH_MAX_UNITS       (1 + GLYPH_MAX_PROCS + GLYPH_MAX_HANDLERS)
#define GLYPH_MAX_UNIT_LOCALS 32      /* inputs + LOCALs + loop counters per procedure */
#define GLYPH_MAX_COMPONENTS  32
#define GLYPH_MAX_STATE       32      /* global variables */
#define GLYPH_MAX_ARRAYS      16
#define GLYPH_MAX_ARRAY_CELLS 2048    /* integers shared by all arrays */

/* Execution */
#define GLYPH_MAX_CALL_DEPTH  64      /* nested procedure calls */
#define GLYPH_MAX_LOCALS      256     /* local variable slots for all active calls */
#define GLYPH_MAX_VSTACK      128     /* expression values in flight */
#define GLYPH_MAX_REPEAT      10000
#define GLYPH_DEFAULT_STEPS   100000  /* per execution entry */
#define GLYPH_MAX_STEPS       10000000 /* ceiling for glyph_set_budget */
#define GLYPH_MAX_DIST        1000    /* FD/BK magnitude */
#define GLYPH_MAX_COORD       1000    /* drawing coordinates and sizes */
#define GLYPH_TURTLE_LIMIT    4096    /* turtle position clamp, cells */
#define GLYPH_INT_MAX         999999999

/* Screen */
#define GLYPH_MAX_W           80
#define GLYPH_MAX_H           24      /* row 25 of an 80x25 VGA is the status line */
#define GLYPH_DEFAULT_W       80
#define GLYPH_DEFAULT_H       24

/* Conformance scripts */
#define GLYPH_MAX_SCRIPT      16384   /* bytes of event script */

#endif

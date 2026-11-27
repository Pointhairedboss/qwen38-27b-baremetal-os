/* glyph_internal.h — definitions shared between the core modules only. */
#ifndef GLYPH_INTERNAL_H
#define GLYPH_INTERNAL_H

#include "glyph.h"
#include "glyph_backend.h"

/* ---- session phases ------------------------------------------------------- */
enum { GLYPH_PHASE_EMPTY = 0, GLYPH_PHASE_LOADED, GLYPH_PHASE_RUNNING, GLYPH_PHASE_FAULTED };

/* ---- tokens --------------------------------------------------------------- */
enum { TOK_EOF = 0, TOK_WORD, TOK_INT, TOK_STR, TOK_VAR, TOK_LBRACK, TOK_RBRACK,
       TOK_LPAREN, TOK_RPAREN, TOK_OP };
/* TOK_OP operator codes */
enum { TOP_PLUS = 1, TOP_MINUS, TOP_MUL, TOP_DIV, TOP_EQ, TOP_NE, TOP_LT, TOP_GT, TOP_LE, TOP_GE };

/* ---- LABEL binding kinds ---------------------------------------------------- */
enum { K_NONE = 0, K_INT = 1, K_STR = 2, K_VAR = 3 };
/* string pool reference packed into an int32: (offset << 8) | length */
#define STR_OFF(v)          ((uint32_t)(v) >> 8)
#define STR_LEN(v)          ((uint32_t)(v) & 0xFFu)

/* ---- bytecode ----------------------------------------------------------------
 * A stack machine. Expression instructions are free; statement instructions
 * cost one step each; loops cost one step per iteration (docs/SPEC.md §11).
 * Operand encodings are given per opcode. "N" opcodes are unresolved names
 * that the compiler rewrites before the program can run. */
enum {
    OP_INVALID = 0,
    /* expressions (free) */
    OP_PUSH_INT,   /* arg = value */
    OP_PUSH_STR,   /* arg = string pool ref */
    OP_LOAD_G,     /* arg = global slot */
    OP_LOAD_L,     /* arg = (sym << 8) | local index */
    OP_LOADN,      /* arg = sym (unresolved) */
    OP_ITEM,       /* arg = array index; pops index */
    OP_ITEMN,      /* arg = sym (unresolved) */
    OP_NEG, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_REM,
    OP_EQ, OP_NE, OP_LESS, OP_GREATER, OP_LESSEQ, OP_GREATEREQ,
    OP_AND, OP_OR, OP_NOT, OP_WORD,
    OP_XCOR, OP_YCOR, OP_HEADING,
    OP_CALLV,      /* arg = procedure sym; reporter call (1 step) */
    /* control (free) */
    OP_JMP,        /* arg = target */
    OP_RET,        /* end of a unit */
    /* control (1 step) */
    OP_IF_JZ,      /* arg = target; pops condition */
    OP_WHILE_JZ,   /* arg = target; pops condition; 1 step per test */
    OP_REP_INIT,   /* arg = counter slot; pops count */
    OP_REP_NEXT,   /* arg = (counter slot << 16) | exit target; 1 step per iteration */
    /* statements (1 step) */
    OP_BLOCK, OP_LOCAL,
    OP_CALL,       /* arg = procedure sym */
    OP_STOP, OP_OUTPUT,
    OP_FD, OP_BK, OP_RT, OP_LT, OP_PU, OP_PD, OP_HOME, OP_SETXY, OP_SETH,
    OP_CS, OP_CLEAR, OP_COLOR, OP_SETCOLOR, OP_TEXT, OP_LINE, OP_RECT, OP_BOX,
    OP_STORE_G, OP_STORE_L, OP_STOREN,  /* SET */
    OP_ADDV_G, OP_ADDV_L, OP_ADDVN,     /* ADD */
    OP_SETITEM, OP_SETITEMN,            /* arg = array index / sym; pops index, value */
    OP_ENABLE, OP_DISABLE,              /* arg = component sym */
    OP_COUNT
};
const char *glyph_op_name(int op);

/* ---- symbols ---------------------------------------------------------------- */
#define GLYPH_SYM_PROC      0x01  /* defined by TO */
#define GLYPH_SYM_HANDLER   0x02  /* defined by ON */
#define GLYPH_SYM_ASSIGNED  0x04  /* global assigned by SET, or a FIELD id */
#define GLYPH_SYM_READ      0x08  /* global read by :name or ADD */
#define GLYPH_SYM_PROCDECL  0x10  /* seen as `TO name` by the pre-scan (arity known) */
#define GLYPH_SYM_COMPREF   0x20  /* ENABLE/DISABLE target */
#define GLYPH_SYM_ACTIONREF 0x40  /* named as an action by a control */

/* ---- components ---------------------------------------------------------- */
enum { GLYPH_COMP_PANEL = 1, GLYPH_COMP_LABEL, GLYPH_COMP_BUTTON, GLYPH_COMP_FIELD };

/* ---- util.c --------------------------------------------------------------- */
void   g_zero(void *p, size_t n);
void   g_copy(void *dst, const void *src, size_t n);
size_t g_strlen(const char *s);
int    g_streq(const char *a, const char *b);
int    g_ieq(const char *a, const char *b);   /* ASCII case-insensitive */
void   g_diag(glyph_diag *d, int code, int line, int col, const char *m1, const char *m2, const char *m3);
void   g_diag_num(glyph_diag *d, int code, int line, int col, const char *m1, long long v, const char *m2);
void   g_copy_diag(glyph_diag *dst, const glyph_diag *src);
void   g_result_reset(glyph_vm *vm, glyph_result *r);
void   val_copy(glyph_value *dst, const glyph_value *src);
void   val_int(glyph_value *v, int32_t i);
void   val_str(glyph_value *v, const char *s, int n);
int    format_int(char *out, int32_t v);              /* returns length; out >= 12 bytes */

/* ---- lex.c ---------------------------------------------------------------- */
void lex_begin(glyph_lexer *lx, const char *src, uint32_t len);
int  lex_next(glyph_lexer *lx, glyph_diag *d);    /* GLYPH_OK or error */

/* ---- parse.c -------------------------------------------------------------- */
int  parse_program(glyph_vm *vm, glyph_diag *d);
int  sym_find(const glyph_vm *vm, const char *upper_name); /* -1 if absent */
int  is_reserved(const char *upper_word);

/* ---- turtle.c (canvas and turtle) ----------------------------------------- */
void    canvas_reset(glyph_vm *vm);
void    turtle_home(glyph_vm *vm);
int32_t turtle_sin(int32_t deg);                   /* 16.16 */
int     canvas_round(int32_t fixed);               /* 16.16 -> nearest cell */
/* Drawing primitives take `avail` (remaining budget) and return their cost
 * in steps; when the cost exceeds `avail` they draw nothing. */
uint32_t turtle_move(glyph_vm *vm, int32_t dist, uint32_t avail);
uint32_t turtle_goto(glyph_vm *vm, int x, int y, uint32_t avail);   /* SETXY, HOME */
void     canvas_put(glyph_vm *vm, int x, int y, char ch, uint8_t attr);
uint32_t canvas_text(glyph_vm *vm, int x, int y, const char *s, int n);
uint32_t canvas_line(glyph_vm *vm, int x1, int y1, int x2, int y2, uint32_t avail);
uint32_t canvas_rect(glyph_vm *vm, int x, int y, int w, int h, uint32_t avail);
uint32_t canvas_box(glyph_vm *vm, int x, int y, int w, int h, uint32_t avail);
uint32_t canvas_clear(glyph_vm *vm);

/* ---- exec.c --------------------------------------------------------------- */
int  exec_entry(glyph_vm *vm, int unit, glyph_diag *d);  /* runs a unit with a fresh budget */
int  state_set(glyph_vm *vm, int slot, const glyph_value *v);

/* ---- ui.c ----------------------------------------------------------------- */
void ui_focus_first(glyph_vm *vm);
void ui_on_disable(glyph_vm *vm, int comp);

#endif

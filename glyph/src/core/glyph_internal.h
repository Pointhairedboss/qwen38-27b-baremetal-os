/* glyph_internal.h — definitions shared between the core modules only. */
#ifndef GLYPH_INTERNAL_H
#define GLYPH_INTERNAL_H

#include "glyph.h"
#include "glyph_backend.h"

/* ---- session phases ------------------------------------------------------- */
enum { GLYPH_PHASE_EMPTY = 0, GLYPH_PHASE_LOADED, GLYPH_PHASE_RUNNING, GLYPH_PHASE_FAULTED };

/* ---- tokens --------------------------------------------------------------- */
enum { TOK_EOF = 0, TOK_WORD, TOK_INT, TOK_STR, TOK_VAR, TOK_LBRACK, TOK_RBRACK };

/* ---- operand kinds (2 bits each in glyph_node.kinds) ------------------------ */
enum { K_NONE = 0, K_INT = 1, K_STR = 2, K_VAR = 3 };
#define KIND(n, i)          (((n)->kinds >> ((i) * 2)) & 3u)
#define SET_KIND(n, i, k)   ((n)->kinds = (uint8_t)(((n)->kinds & ~(3u << ((i) * 2))) | ((unsigned)(k) << ((i) * 2))))
/* K_STR operand value: (pool offset << 8) | length */
#define STR_OFF(v)          ((uint32_t)(v) >> 8)
#define STR_LEN(v)          ((uint32_t)(v) & 0xFFu)

/* ---- opcodes ---------------------------------------------------------------- */
enum {
    OP_NONE = 0,
    OP_BLOCK,    /* body = list head */
    OP_CALL,     /* body = procedure symbol */
    OP_FD, OP_BK, OP_RT, OP_LT, OP_PU, OP_PD,
    OP_REPEAT,   /* a0 = count, body = head */
    OP_CS, OP_CLEAR, OP_COLOR, OP_SETCOLOR,
    OP_TEXT, OP_LINE, OP_RECT, OP_BOX,
    OP_SET,      /* a0 = name symbol, a1 = value */
    OP_ADD,      /* a0 = name symbol, a1 = num */
    OP_ENABLE, OP_DISABLE, /* a0 = component symbol */
    OP_COUNT
};
const char *glyph_op_name(int op);

/* ---- symbols ---------------------------------------------------------------- */
#define GLYPH_SYM_PROC      0x01  /* defined by TO */
#define GLYPH_SYM_HANDLER   0x02  /* defined by ON */
#define GLYPH_SYM_ASSIGNED  0x04  /* SET target or FIELD id: has a state slot */
#define GLYPH_SYM_READ      0x08  /* read by :name or ADD */
#define GLYPH_SYM_CALLED    0x10  /* called as a procedure */
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
/* Each drawing primitive returns the number of steps it consumed, or
 * a negative value when the budget would be exceeded (nothing partial is
 * guaranteed in that case; the entry stops with E_BUDGET). */
int     turtle_move(glyph_vm *vm, int32_t dist, uint32_t avail, uint32_t *used);
void    canvas_put(glyph_vm *vm, int x, int y, char ch, uint8_t attr);
uint32_t canvas_text(glyph_vm *vm, int x, int y, const char *s, int n);
uint32_t canvas_line(glyph_vm *vm, int x1, int y1, int x2, int y2, uint32_t avail);
uint32_t canvas_rect(glyph_vm *vm, int x, int y, int w, int h, uint32_t avail);
uint32_t canvas_box(glyph_vm *vm, int x, int y, int w, int h, uint32_t avail);
uint32_t canvas_clear(glyph_vm *vm);

/* ---- exec.c --------------------------------------------------------------- */
int  exec_entry(glyph_vm *vm, uint16_t head, glyph_diag *d); /* runs a body with a fresh budget */
int  state_set_int(glyph_vm *vm, int slot, int32_t v);
int  state_set_str(glyph_vm *vm, int slot, const char *s, int n);
int  format_int(char *out, int32_t v);              /* returns length; out >= 12 bytes */

/* ---- ui.c ----------------------------------------------------------------- */
void ui_focus_first(glyph_vm *vm);
void ui_on_disable(glyph_vm *vm, int comp);

#endif

/* glyph.h — public API of the Glyph shared interpreter core.
 *
 * The core is freestanding C99: it uses only <stddef.h> and <stdint.h>, calls
 * no libc or OS function, performs no I/O and allocates nothing. The caller
 * owns one struct glyph_vm (static storage is fine; see glyph_vm_size()).
 *
 * Lifecycle:   glyph_init -> glyph_load -> glyph_start -> glyph_key/click/action...
 * Output:      glyph_render (row callback), glyph_state_* (state), results.
 * Platform I/O lives outside the core; see glyph_backend.h.
 */
#ifndef GLYPH_H
#define GLYPH_H

#include <stddef.h>
#include <stdint.h>
#include "glyph_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- status codes ------------------------------------------------------ */
enum {
    GLYPH_OK = 0,
    GLYPH_IGNORED,        /* input had no effect (not an error) */
    GLYPH_E_ARG,          /* API misuse: bad argument */
    GLYPH_E_SOURCE_SIZE,
    GLYPH_E_CHAR,
    GLYPH_E_STRING,
    GLYPH_E_STR_LEN,
    GLYPH_E_NAME_LEN,
    GLYPH_E_NUMBER,
    GLYPH_E_TOKENS,
    GLYPH_E_SYNTAX,
    GLYPH_E_UNCLOSED,
    GLYPH_E_NEST,
    GLYPH_E_NODES,
    GLYPH_E_NAMES,
    GLYPH_E_STRPOOL,
    GLYPH_E_UNKNOWN,
    GLYPH_E_REDEFINE,
    GLYPH_E_TOPLEVEL,
    GLYPH_E_UNDEFINED,
    GLYPH_E_KIND,
    GLYPH_E_LIMIT,
    GLYPH_E_RANGE,
    GLYPH_E_TYPE,
    GLYPH_E_UNSET,
    GLYPH_E_BUDGET,
    GLYPH_E_DEPTH,
    GLYPH_E_FAULTED,
    GLYPH_E_STATE,        /* API called in the wrong phase */
    GLYPH_CODE_COUNT
};

/* Stable name for a status code, e.g. "E_SYNTAX". Never NULL. */
const char *glyph_code_name(int code);

/* ---- keys (ASCII plus the codes the PS/2 and serial drivers produce) ---- */
#define GLYPH_KEY_BS     8
#define GLYPH_KEY_TAB    9
#define GLYPH_KEY_LF     10
#define GLYPH_KEY_CR     13
#define GLYPH_KEY_ESC    27
#define GLYPH_KEY_SPACE  32
#define GLYPH_KEY_DEL    127

/* ---- cells --------------------------------------------------------------- */
/* A cell is (attribute << 8) | character, the VGA text-mode layout. */
typedef uint16_t glyph_cell;
#define GLYPH_CELL(ch, attr) ((glyph_cell)((((unsigned)(attr) & 0xFFu) << 8) | ((unsigned)(ch) & 0xFFu)))
#define GLYPH_CELL_CH(c)     ((unsigned char)((c) & 0xFFu))
#define GLYPH_CELL_ATTR(c)   ((unsigned char)(((c) >> 8) & 0xFFu))

#define GLYPH_ATTR_DEFAULT   0x07
#define GLYPH_ATTR_PANEL     0x0B
#define GLYPH_ATTR_TITLE     0x0F
#define GLYPH_ATTR_LABEL     0x0F
#define GLYPH_ATTR_CONTROL   0x1F
#define GLYPH_ATTR_FOCUS     0x70
#define GLYPH_ATTR_DISABLED  0x08

/* ---- diagnostics and results -------------------------------------------- */
typedef struct glyph_diag {
    int  code;            /* GLYPH_OK or GLYPH_E_* */
    int  line, col;       /* 1-based source position; 0 when not applicable */
    char msg[96];         /* NUL-terminated, ASCII */
} glyph_diag;

typedef struct glyph_result {
    int        status;    /* GLYPH_OK, GLYPH_IGNORED or GLYPH_E_* */
    int        action;    /* symbol index of the fired action, or -1 */
    int        focus;     /* focused component index after the input, or -1 */
    uint32_t   steps;     /* steps used by this execution entry */
    int        changed;   /* number of state variables whose value changed */
    glyph_diag diag;      /* filled when status is an error */
} glyph_result;

/* ---- internal representation (exposed only so callers can allocate) ----- */
typedef struct glyph_node {
    uint8_t  op;          /* GLYPH_OP_* (glyph_internal.h) */
    uint8_t  kinds;       /* 2 bits per operand: none/int/str/var */
    uint16_t line, col;
    uint16_t body;        /* first node of a body, or procedure symbol for CALL */
    uint16_t next;        /* next node in the sequence; 0 ends it */
    int32_t  a[4];        /* operands */
} glyph_node;

typedef struct glyph_sym {
    char     name[GLYPH_MAX_NAME_LEN + 1];
    uint8_t  flags;       /* GLYPH_SYM_* */
    int8_t   comp;        /* component index or -1 */
    int8_t   state;       /* state slot or -1 */
    uint8_t  pad;
    uint16_t proc_body;   /* TO body head (valid if GLYPH_SYM_PROC) */
    uint16_t handler_body;/* ON body head (valid if GLYPH_SYM_HANDLER) */
    uint16_t line, col;   /* first reference, for link diagnostics */
} glyph_sym;

typedef struct glyph_comp {
    uint8_t  type;        /* GLYPH_COMP_* */
    uint8_t  enabled;
    uint16_t sym;         /* id */
    int16_t  x, y, w, h;
    uint16_t text;        /* string pool offset of title/caption/literal */
    uint8_t  text_len;
    uint8_t  bind_kind;   /* LABEL value kind: int/str/var */
    int32_t  bind;        /* LABEL int literal or variable symbol */
    uint16_t action;      /* BUTTON/FIELD action symbol */
    uint16_t line, col;
} glyph_comp;

typedef struct glyph_state {
    uint16_t sym;
    uint8_t  type;        /* GLYPH_T_UNSET / INT / STR */
    uint8_t  dirty;       /* value changed during the current entry */
    int32_t  i;
    uint8_t  len;
    char     s[GLYPH_MAX_STR_LEN + 1];
} glyph_state;

typedef struct glyph_frame {
    uint16_t pc;          /* next node to run; 0 = end of sequence */
    uint16_t body;        /* sequence head to restart for REPEAT */
    int32_t  remaining;   /* REPEAT iterations still to start */
    uint8_t  is_call;
    uint8_t  pad[3];
} glyph_frame;

typedef struct glyph_lexer {
    const char *src;
    uint32_t    len, pos;
    uint16_t    line, col;
    uint32_t    ntokens;
    /* current token */
    uint8_t     type;
    uint16_t    tline, tcol;
    int32_t     ival;
    uint8_t     tlen;
    char        text[GLYPH_MAX_STR_LEN + 1];
} glyph_lexer;

typedef struct glyph_vm {
    uint8_t     phase;               /* GLYPH_PHASE_* */
    uint8_t     w, h, screen_set;
    /* program (built by glyph_load) */
    glyph_node  nodes[GLYPH_MAX_NODES];
    uint16_t    n_nodes;
    uint16_t    main_head, main_tail;
    glyph_sym   syms[GLYPH_MAX_NAMES];
    uint16_t    n_syms;
    uint8_t     n_procs, n_handlers;
    char        strpool[GLYPH_MAX_STRPOOL];
    uint16_t    strpool_len;
    glyph_comp  comps[GLYPH_MAX_COMPONENTS];
    uint8_t     n_comps;
    glyph_state state[GLYPH_MAX_STATE];
    uint8_t     n_state;
    /* canvas and turtle */
    glyph_cell  canvas[GLYPH_MAX_W * GLYPH_MAX_H];
    glyph_cell  row[GLYPH_MAX_W];    /* render scratch */
    int32_t     tx, ty;              /* 16.16 fixed point, cells */
    int32_t     heading;             /* degrees, 0..359, clockwise from north */
    uint8_t     pen, attr;
    /* execution */
    int8_t      focus;
    uint8_t     depth;
    glyph_frame frames[GLYPH_MAX_FRAMES];
    uint16_t    sp;
    uint32_t    steps, budget;
    /* load-time */
    glyph_lexer lex;
    glyph_diag  diag;                /* last load or start error */
} glyph_vm;

/* ---- lifecycle ------------------------------------------------------------ */
size_t glyph_vm_size(void);

/* Reset vm to the empty state. Always succeeds. */
void glyph_init(glyph_vm *vm);

/* Lex, parse and link a program. On failure vm is left empty and d holds the
 * diagnostic. d may be NULL. Returns GLYPH_OK or an error code. */
int glyph_load(glyph_vm *vm, const char *src, size_t len, glyph_diag *d);

/* Run the main sequence once. r may be NULL. A runtime error faults the
 * session. Returns the status. */
int glyph_start(glyph_vm *vm, glyph_result *r);

/* Inputs. Each returns the status and fills r (may be NULL). */
int glyph_key(glyph_vm *vm, int key, glyph_result *r);
int glyph_click(glyph_vm *vm, const char *id, glyph_result *r);
int glyph_action(glyph_vm *vm, const char *name, glyph_result *r);

/* Diagnostic of the last failed glyph_load or glyph_start (code GLYPH_OK if none). */
const glyph_diag *glyph_last_diag(const glyph_vm *vm);

/* Lower (never raise above GLYPH_MAX_STEPS) the per-entry step budget. */
void glyph_set_budget(glyph_vm *vm, uint32_t steps);

/* ---- output ------------------------------------------------------------- */
/* Called once per row, top to bottom, with w composed cells. */
typedef void (*glyph_row_fn)(void *user, int y, const glyph_cell *cells, int w);
void glyph_render(glyph_vm *vm, glyph_row_fn fn, void *user);
int  glyph_width(const glyph_vm *vm);
int  glyph_height(const glyph_vm *vm);

/* ---- introspection -------------------------------------------------------- */
enum { GLYPH_T_UNSET = 0, GLYPH_T_INT, GLYPH_T_STR };

int         glyph_state_count(const glyph_vm *vm);
const char *glyph_state_name(const glyph_vm *vm, int i);
int         glyph_state_type(const glyph_vm *vm, int i);
int32_t     glyph_state_int(const glyph_vm *vm, int i);
const char *glyph_state_str(const glyph_vm *vm, int i);
int         glyph_state_dirty(const glyph_vm *vm, int i);
int         glyph_state_find(const glyph_vm *vm, const char *name); /* -1 if none */

int         glyph_comp_count(const glyph_vm *vm);
const char *glyph_comp_name(const glyph_vm *vm, int i);
int         glyph_comp_enabled(const glyph_vm *vm, int i);
int         glyph_focus(const glyph_vm *vm);                       /* -1 if none */

const char *glyph_sym_name(const glyph_vm *vm, int sym);           /* "" if invalid */

/* Turtle position in whole cells (rounded) and heading in degrees. */
void glyph_turtle(const glyph_vm *vm, int *x, int *y, int *heading, int *pen);
/* Canvas cell (before components are composited); 0 if out of range. */
glyph_cell glyph_canvas_cell(const glyph_vm *vm, int x, int y);

#ifdef __cplusplus
}
#endif
#endif

/* parse.c — recursive-descent parser and linker.
 *
 * Produces the compact IR (glyph_node sequences) plus the symbol, component
 * and state tables. Recursion depth is bounded by GLYPH_MAX_NEST, checked
 * before each descent. Linking resolves procedure calls, variable reads,
 * component references and action handlers, reporting the earliest error in
 * source order.
 */
#include "glyph_internal.h"

/* ---- reserved words ---------------------------------------------------------- */
enum { W_CMD = 1, W_DECL, W_END, W_REPEAT };
enum { D_SCREEN = 1, D_PANEL, D_LABEL, D_BUTTON, D_FIELD, D_TO, D_ON };

typedef struct kw {
    const char *word;
    uint8_t     kind;   /* W_* */
    uint8_t     op;     /* OP_* or D_* */
    const char *sig;    /* operands: n=num v=value S=assign R=read-target C=component */
} kw;

static const kw keywords[] = {
    { "FD", W_CMD, OP_FD, "n" },          { "FORWARD", W_CMD, OP_FD, "n" },
    { "BK", W_CMD, OP_BK, "n" },          { "BACK", W_CMD, OP_BK, "n" },
    { "RT", W_CMD, OP_RT, "n" },          { "RIGHT", W_CMD, OP_RT, "n" },
    { "LT", W_CMD, OP_LT, "n" },          { "LEFT", W_CMD, OP_LT, "n" },
    { "PU", W_CMD, OP_PU, "" },           { "PENUP", W_CMD, OP_PU, "" },
    { "PD", W_CMD, OP_PD, "" },           { "PENDOWN", W_CMD, OP_PD, "" },
    { "REPEAT", W_REPEAT, OP_REPEAT, "n" },
    { "CLEARSCREEN", W_CMD, OP_CS, "" },  { "CS", W_CMD, OP_CS, "" },
    { "CLEAR", W_CMD, OP_CLEAR, "" },
    { "COLOR", W_CMD, OP_COLOR, "nn" },   { "SETCOLOR", W_CMD, OP_SETCOLOR, "n" },
    { "TEXT", W_CMD, OP_TEXT, "nnv" },
    { "LINE", W_CMD, OP_LINE, "nnnn" },
    { "RECT", W_CMD, OP_RECT, "nnnn" },
    { "BOX", W_CMD, OP_BOX, "nnnn" },
    { "SET", W_CMD, OP_SET, "Sv" },
    { "ADD", W_CMD, OP_ADD, "Rn" },
    { "ENABLE", W_CMD, OP_ENABLE, "C" },  { "DISABLE", W_CMD, OP_DISABLE, "C" },
    { "SCREEN", W_DECL, D_SCREEN, 0 },    { "PANEL", W_DECL, D_PANEL, 0 },
    { "LABEL", W_DECL, D_LABEL, 0 },      { "BUTTON", W_DECL, D_BUTTON, 0 },
    { "FIELD", W_DECL, D_FIELD, 0 },      { "TO", W_DECL, D_TO, 0 },
    { "ON", W_DECL, D_ON, 0 },
    { "END", W_END, 0, 0 },
};
#define N_KEYWORDS ((int)(sizeof keywords / sizeof keywords[0]))

static const kw *lookup_kw(const char *w)
{
    int i;
    for (i = 0; i < N_KEYWORDS; i++)
        if (g_streq(keywords[i].word, w)) return &keywords[i];
    return 0;
}

int is_reserved(const char *upper_word) { return lookup_kw(upper_word) != 0; }

const char *glyph_op_name(int op)
{
    static const char *const names[OP_COUNT] = {
        "NONE", "BLOCK", "CALL", "FD", "BK", "RT", "LT", "PU", "PD", "REPEAT",
        "CLEARSCREEN", "CLEAR", "COLOR", "SETCOLOR", "TEXT", "LINE", "RECT", "BOX",
        "SET", "ADD", "ENABLE", "DISABLE"
    };
    return (op >= 0 && op < OP_COUNT) ? names[op] : "?";
}

/* ---- tables ------------------------------------------------------------- */
int sym_find(const glyph_vm *vm, const char *name)
{
    int i;
    for (i = 0; i < vm->n_syms; i++)
        if (g_streq(vm->syms[i].name, name)) return i;
    return -1;
}

static int sym_intern(glyph_vm *vm, const char *name, int line, int col, glyph_diag *d)
{
    int i = sym_find(vm, name);
    glyph_sym *s;
    if (i >= 0) return i;
    if (vm->n_syms >= GLYPH_MAX_NAMES) {
        g_diag_num(d, GLYPH_E_NAMES, line, col, "more than ", GLYPH_MAX_NAMES, " distinct names");
        return -GLYPH_E_NAMES;
    }
    i = vm->n_syms++;
    s = &vm->syms[i];
    g_copy(s->name, name, g_strlen(name) + 1);
    s->flags = 0; s->comp = -1; s->state = -1; s->pad = 0;
    s->proc_body = 0; s->handler_body = 0;
    s->line = (uint16_t)line; s->col = (uint16_t)col;
    return i;
}

static int new_node(glyph_vm *vm, int op, int line, int col, glyph_diag *d)
{
    glyph_node *n;
    int i;
    if (vm->n_nodes >= GLYPH_MAX_NODES) {
        g_diag_num(d, GLYPH_E_NODES, line, col, "program has more than ", GLYPH_MAX_NODES - 1, " statements");
        return -GLYPH_E_NODES;
    }
    i = vm->n_nodes++;
    n = &vm->nodes[i];
    g_zero(n, sizeof *n);
    n->op = (uint8_t)op; n->line = (uint16_t)line; n->col = (uint16_t)col;
    return i;
}

static int pool_add(glyph_vm *vm, const char *s, int len, int line, int col, glyph_diag *d)
{
    int off = vm->strpool_len;
    if (off + len + 1 > GLYPH_MAX_STRPOOL) {
        g_diag_num(d, GLYPH_E_STRPOOL, line, col, "string literals exceed ", GLYPH_MAX_STRPOOL, " bytes");
        return -GLYPH_E_STRPOOL;
    }
    g_copy(&vm->strpool[off], s, (size_t)len);
    vm->strpool[off + len] = 0;
    vm->strpool_len = (uint16_t)(off + len + 1);
    return off;
}

static int ensure_state(glyph_vm *vm, int sym, int line, int col, glyph_diag *d)
{
    glyph_sym *s = &vm->syms[sym];
    s->flags |= GLYPH_SYM_ASSIGNED;
    if (s->state < 0) {
        glyph_state *st;
        if (vm->n_state >= GLYPH_MAX_STATE) {
            g_diag_num(d, GLYPH_E_LIMIT, line, col, "more than ", GLYPH_MAX_STATE, " state variables");
            return GLYPH_E_LIMIT;
        }
        s->state = (int8_t)vm->n_state;
        st = &vm->state[vm->n_state++];
        g_zero(st, sizeof *st);
        st->sym = (uint16_t)sym;
        st->type = GLYPH_T_UNSET;
    }
    return GLYPH_OK;
}

/* ---- token helpers ---------------------------------------------------------- */
#define LX (&vm->lex)
#define NEXT() do { int e_ = lex_next(LX, d); if (e_) return e_; } while (0)
#define TRY(x) do { int e_ = (x); if (e_) return e_; } while (0)

static int err_at_tok(glyph_vm *vm, glyph_diag *d, int code, const char *a, const char *b, const char *c)
{
    g_diag(d, code, LX->tline, LX->tcol, a, b, c);
    return code;
}

static const char *tok_desc(int t)
{
    switch (t) {
    case TOK_EOF: return "end of program";
    case TOK_INT: return "a number";
    case TOK_STR: return "a string";
    case TOK_VAR: return "a :variable";
    case TOK_LBRACK: return "'['";
    case TOK_RBRACK: return "']'";
    default: return "a word";
    }
}

/* Current token must be a non-reserved word; returns its symbol in *sym. */
static int expect_name(glyph_vm *vm, glyph_diag *d, const char *who, int *sym)
{
    int s;
    if (LX->type != TOK_WORD)
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, who, " expects a name, found ", tok_desc(LX->type));
    if (is_reserved(LX->text))
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, "'", LX->text, "' is a reserved word and cannot be a name");
    s = sym_intern(vm, LX->text, LX->tline, LX->tcol, d);
    if (s < 0) return -s;
    *sym = s;
    return GLYPH_OK;
}

static int expect_lit(glyph_vm *vm, glyph_diag *d, const char *who, int32_t *v)
{
    if (LX->type != TOK_INT)
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, who, " expects a number literal, found ", tok_desc(LX->type));
    *v = LX->ival;
    return GLYPH_OK;
}

static int expect_str(glyph_vm *vm, glyph_diag *d, const char *who, uint16_t *off, uint8_t *len)
{
    int o;
    if (LX->type != TOK_STR)
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, who, " expects a \"string\", found ", tok_desc(LX->type));
    o = pool_add(vm, LX->text, LX->tlen, LX->tline, LX->tcol, d);
    if (o < 0) return -o;
    *off = (uint16_t)o; *len = LX->tlen;
    return GLYPH_OK;
}

/* Parse one operand of kind 'n' (num) or 'v' (value) into node slot i. */
static int parse_operand(glyph_vm *vm, glyph_diag *d, glyph_node *n, int i, char kind, const char *who)
{
    if (LX->type == TOK_INT) {
        SET_KIND(n, i, K_INT);
        n->a[i] = LX->ival;
    } else if (LX->type == TOK_VAR) {
        int s = sym_intern(vm, LX->text, LX->tline, LX->tcol, d);
        if (s < 0) return -s;
        vm->syms[s].flags |= GLYPH_SYM_READ;
        SET_KIND(n, i, K_VAR);
        n->a[i] = s;
    } else if (LX->type == TOK_STR && kind == 'v') {
        int o = pool_add(vm, LX->text, LX->tlen, LX->tline, LX->tcol, d);
        if (o < 0) return -o;
        SET_KIND(n, i, K_STR);
        n->a[i] = (int32_t)(((uint32_t)o << 8) | LX->tlen);
    } else {
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, who,
                          kind == 'v' ? " expects a number, \"string\" or :variable, found "
                                      : " expects a number or :variable, found ",
                          tok_desc(LX->type));
    }
    return GLYPH_OK;
}

/* ---- statements ------------------------------------------------------------- */
typedef struct seq { uint16_t head, tail; } seq;

static void seq_add(glyph_vm *vm, seq *s, int idx)
{
    if (!s->head) s->head = (uint16_t)idx;
    else vm->nodes[s->tail].next = (uint16_t)idx;
    s->tail = (uint16_t)idx;
}

static int parse_stmt(glyph_vm *vm, int depth, int *out, glyph_diag *d);

/* Current token is '['. The list itself sits at nesting level `depth`. */
static int parse_list(glyph_vm *vm, int depth, uint16_t *head, glyph_diag *d)
{
    seq s = { 0, 0 };
    int line = LX->tline, col = LX->tcol;
    if (depth > GLYPH_MAX_NEST)
        return err_at_tok(vm, d, GLYPH_E_NEST, "lists nested deeper than 16", 0, 0);
    NEXT();
    while (LX->type != TOK_RBRACK) {
        int n;
        if (LX->type == TOK_EOF) {
            g_diag(d, GLYPH_E_UNCLOSED, line, col, "'[' opened here has no matching ']'", 0, 0);
            return GLYPH_E_UNCLOSED;
        }
        TRY(parse_stmt(vm, depth, &n, d));
        seq_add(vm, &s, n);
    }
    NEXT();
    *head = s.head;
    return GLYPH_OK;
}

/* A body: a list, or one statement. The body sits at nesting level depth+1. */
static int parse_body(glyph_vm *vm, int depth, uint16_t *head, glyph_diag *d)
{
    int n;
    if (LX->type == TOK_LBRACK) return parse_list(vm, depth + 1, head, d);
    if (depth + 1 > GLYPH_MAX_NEST)
        return err_at_tok(vm, d, GLYPH_E_NEST, "bodies nested deeper than 16", 0, 0);
    TRY(parse_stmt(vm, depth + 1, &n, d));
    *head = (uint16_t)n;
    return GLYPH_OK;
}

static int parse_stmt(glyph_vm *vm, int depth, int *out, glyph_diag *d)
{
    const kw *k;
    glyph_node *n;
    int idx, line = LX->tline, col = LX->tcol, i;

    switch (LX->type) {
    case TOK_LBRACK: {
        uint16_t head;
        idx = new_node(vm, OP_BLOCK, line, col, d);
        if (idx < 0) return -idx;
        TRY(parse_list(vm, depth + 1, &head, d));
        vm->nodes[idx].body = head;
        *out = idx;
        return GLYPH_OK;
    }
    case TOK_RBRACK:
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, "unexpected ']'", 0, 0);
    case TOK_WORD:
        break;
    default:
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, "expected a command, found ", tok_desc(LX->type), 0);
    }

    k = lookup_kw(LX->text);
    if (!k) {                                   /* procedure call */
        int s = sym_intern(vm, LX->text, line, col, d);
        if (s < 0) return -s;
        idx = new_node(vm, OP_CALL, line, col, d);
        if (idx < 0) return -idx;
        vm->syms[s].flags |= GLYPH_SYM_CALLED;
        vm->nodes[idx].body = (uint16_t)s;
        NEXT();
        /* No command takes a value here, so `name 5` is a misspelt command
         * or an argument to a parameterless procedure. */
        if (LX->type == TOK_INT || LX->type == TOK_STR || LX->type == TOK_VAR) {
            if (vm->syms[s].flags & GLYPH_SYM_PROC)
                g_diag(d, GLYPH_E_SYNTAX, line, col, "procedure ", vm->syms[s].name, " takes no inputs");
            else
                g_diag(d, GLYPH_E_UNKNOWN, line, col, "unknown command ", vm->syms[s].name, 0);
            return d->code;
        }
        *out = idx;
        return GLYPH_OK;
    }
    if (k->kind == W_DECL)
        return err_at_tok(vm, d, GLYPH_E_TOPLEVEL, k->word, " is only allowed at the top level", 0);
    if (k->kind == W_END)
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, "END without TO", 0, 0);

    idx = new_node(vm, k->op, line, col, d);
    if (idx < 0) return -idx;
    NEXT();
    n = &vm->nodes[idx];
    for (i = 0; k->sig[i]; i++) {
        char c = k->sig[i];
        if (c == 'n' || c == 'v') {
            TRY(parse_operand(vm, d, n, i, c, k->word));
        } else {
            int s;
            TRY(expect_name(vm, d, k->word, &s));
            n->a[i] = s;
            if (c == 'S') TRY(ensure_state(vm, s, LX->tline, LX->tcol, d));
            else if (c == 'R') vm->syms[s].flags |= GLYPH_SYM_READ;
            else if (c == 'C') vm->syms[s].flags |= GLYPH_SYM_COMPREF;
        }
        NEXT();
    }
    if (k->kind == W_REPEAT) {
        uint16_t head;
        TRY(parse_body(vm, depth, &head, d));
        vm->nodes[idx].body = head;
    }
    *out = idx;
    return GLYPH_OK;
}

/* ---- declarations ---------------------------------------------------------- */
static int new_comp(glyph_vm *vm, int type, const char *who, int line, int col, glyph_diag *d, glyph_comp **out)
{
    int s;
    glyph_comp *c;
    TRY(expect_name(vm, d, who, &s));
    if (vm->syms[s].comp >= 0)
        return err_at_tok(vm, d, GLYPH_E_REDEFINE, "component ", LX->text, " is already declared");
    if (vm->n_comps >= GLYPH_MAX_COMPONENTS) {
        g_diag_num(d, GLYPH_E_LIMIT, LX->tline, LX->tcol, "more than ", GLYPH_MAX_COMPONENTS, " components");
        return GLYPH_E_LIMIT;
    }
    vm->syms[s].comp = (int8_t)vm->n_comps;
    c = &vm->comps[vm->n_comps++];
    g_zero(c, sizeof *c);
    c->type = (uint8_t)type; c->enabled = 1; c->sym = (uint16_t)s;
    c->line = (uint16_t)line; c->col = (uint16_t)col;
    NEXT();
    *out = c;
    return GLYPH_OK;
}

static int parse_xy(glyph_vm *vm, glyph_diag *d, const char *who, glyph_comp *c)
{
    int32_t v;
    TRY(expect_lit(vm, d, who, &v));
    if (v < -GLYPH_MAX_COORD || v > GLYPH_MAX_COORD)
        return err_at_tok(vm, d, GLYPH_E_RANGE, who, " coordinate out of range", 0);
    c->x = (int16_t)v;
    NEXT();
    TRY(expect_lit(vm, d, who, &v));
    if (v < -GLYPH_MAX_COORD || v > GLYPH_MAX_COORD)
        return err_at_tok(vm, d, GLYPH_E_RANGE, who, " coordinate out of range", 0);
    c->y = (int16_t)v;
    NEXT();
    return GLYPH_OK;
}

static int parse_size(glyph_vm *vm, glyph_diag *d, const char *who, int lo, int hi, int16_t *out)
{
    int32_t v;
    TRY(expect_lit(vm, d, who, &v));
    if (v < lo || v > hi) {
        g_diag_num(d, GLYPH_E_RANGE, LX->tline, LX->tcol, "size ", v, " out of range");
        return GLYPH_E_RANGE;
    }
    *out = (int16_t)v;
    NEXT();
    return GLYPH_OK;
}

static int parse_action_ref(glyph_vm *vm, glyph_diag *d, const char *who, glyph_comp *c)
{
    int s;
    TRY(expect_name(vm, d, who, &s));
    vm->syms[s].flags |= GLYPH_SYM_ACTIONREF;
    c->action = (uint16_t)s;
    NEXT();
    return GLYPH_OK;
}

static int parse_decl(glyph_vm *vm, const kw *k, glyph_diag *d)
{
    glyph_comp *c;
    int line = LX->tline, col = LX->tcol;
    NEXT();
    switch (k->op) {
    case D_SCREEN: {
        int32_t w, h;
        if (vm->screen_set) return err_at_tok(vm, d, GLYPH_E_REDEFINE, "SCREEN declared twice", 0, 0);
        TRY(expect_lit(vm, d, "SCREEN", &w));
        if (w < 1 || w > GLYPH_MAX_W) return err_at_tok(vm, d, GLYPH_E_RANGE, "SCREEN width must be 1..80", 0, 0);
        NEXT();
        TRY(expect_lit(vm, d, "SCREEN", &h));
        if (h < 1 || h > GLYPH_MAX_H) return err_at_tok(vm, d, GLYPH_E_RANGE, "SCREEN height must be 1..24", 0, 0);
        NEXT();
        vm->w = (uint8_t)w; vm->h = (uint8_t)h; vm->screen_set = 1;
        return GLYPH_OK;
    }
    case D_PANEL:
        TRY(new_comp(vm, GLYPH_COMP_PANEL, "PANEL", line, col, d, &c));
        TRY(parse_xy(vm, d, "PANEL", c));
        TRY(parse_size(vm, d, "PANEL", 2, GLYPH_MAX_W, &c->w));
        TRY(parse_size(vm, d, "PANEL", 2, GLYPH_MAX_H, &c->h));
        TRY(expect_str(vm, d, "PANEL", &c->text, &c->text_len));
        NEXT();
        return GLYPH_OK;
    case D_LABEL: {
        glyph_node tmp;
        TRY(new_comp(vm, GLYPH_COMP_LABEL, "LABEL", line, col, d, &c));
        TRY(parse_xy(vm, d, "LABEL", c));
        g_zero(&tmp, sizeof tmp);
        TRY(parse_operand(vm, d, &tmp, 0, 'v', "LABEL"));
        c->bind_kind = (uint8_t)KIND(&tmp, 0);
        c->bind = tmp.a[0];
        NEXT();
        return GLYPH_OK;
    }
    case D_BUTTON:
        TRY(new_comp(vm, GLYPH_COMP_BUTTON, "BUTTON", line, col, d, &c));
        TRY(parse_xy(vm, d, "BUTTON", c));
        TRY(expect_str(vm, d, "BUTTON", &c->text, &c->text_len));
        NEXT();
        c->w = (int16_t)(c->text_len + 4); c->h = 1;
        return parse_action_ref(vm, d, "BUTTON", c);
    case D_FIELD: {
        int16_t width;
        TRY(new_comp(vm, GLYPH_COMP_FIELD, "FIELD", line, col, d, &c));
        TRY(ensure_state(vm, c->sym, c->line, c->col, d));
        vm->state[vm->syms[c->sym].state].type = GLYPH_T_STR;
        TRY(parse_xy(vm, d, "FIELD", c));
        TRY(parse_size(vm, d, "FIELD", 1, GLYPH_MAX_STR_LEN, &width));
        c->w = (int16_t)(width + 2); c->h = 1;
        return parse_action_ref(vm, d, "FIELD", c);
    }
    case D_TO: {
        int s;
        seq body = { 0, 0 };
        if (LX->type == TOK_WORD && is_reserved(LX->text))
            return err_at_tok(vm, d, GLYPH_E_REDEFINE, "cannot redefine built-in word ", LX->text, 0);
        TRY(expect_name(vm, d, "TO", &s));
        if (vm->syms[s].flags & GLYPH_SYM_PROC)
            return err_at_tok(vm, d, GLYPH_E_REDEFINE, "procedure ", LX->text, " is already defined");
        if (vm->n_procs >= GLYPH_MAX_PROCS) {
            g_diag_num(d, GLYPH_E_LIMIT, LX->tline, LX->tcol, "more than ", GLYPH_MAX_PROCS, " procedures");
            return GLYPH_E_LIMIT;
        }
        vm->n_procs++;
        vm->syms[s].flags |= GLYPH_SYM_PROC;
        NEXT();
        for (;;) {
            int n;
            if (LX->type == TOK_WORD && g_streq(LX->text, "END")) break;
            if (LX->type == TOK_EOF) {
                g_diag(d, GLYPH_E_UNCLOSED, line, col, "TO ", vm->syms[s].name, " has no matching END");
                return GLYPH_E_UNCLOSED;
            }
            TRY(parse_stmt(vm, 1, &n, d));
            seq_add(vm, &body, n);
        }
        NEXT();
        vm->syms[s].proc_body = body.head;
        return GLYPH_OK;
    }
    case D_ON: {
        int s;
        uint16_t head;
        TRY(expect_name(vm, d, "ON", &s));
        if (vm->syms[s].flags & GLYPH_SYM_HANDLER)
            return err_at_tok(vm, d, GLYPH_E_REDEFINE, "handler for ", LX->text, " is already defined");
        if (vm->n_handlers >= GLYPH_MAX_HANDLERS) {
            g_diag_num(d, GLYPH_E_LIMIT, LX->tline, LX->tcol, "more than ", GLYPH_MAX_HANDLERS, " handlers");
            return GLYPH_E_LIMIT;
        }
        vm->n_handlers++;
        vm->syms[s].flags |= GLYPH_SYM_HANDLER;
        NEXT();
        TRY(parse_body(vm, 0, &head, d));
        vm->syms[s].handler_body = head;
        return GLYPH_OK;
    }
    }
    return GLYPH_E_SYNTAX;
}

/* ---- linking -------------------------------------------------------------- */
typedef struct link_err { glyph_diag *d; int found; } link_err;

/* Record an error if it is earlier in the source than the one already held. */
static void link_report(link_err *le, int code, int line, int col, const char *a, const char *b, const char *c)
{
    if (le->found && (line > le->d->line || (line == le->d->line && col >= le->d->col))) return;
    g_diag(le->d, code, line, col, a, b, c);
    le->found = 1;
}

static void link_var(glyph_vm *vm, link_err *le, int sym, int line, int col)
{
    if (!(vm->syms[sym].flags & GLYPH_SYM_ASSIGNED))
        link_report(le, GLYPH_E_UNDEFINED, line, col, "variable ", vm->syms[sym].name, " is never SET");
}

static int link_program(glyph_vm *vm, glyph_diag *d)
{
    link_err le;
    int i, j;
    le.d = d; le.found = 0;

    for (i = 1; i < vm->n_nodes; i++) {
        glyph_node *n = &vm->nodes[i];
        for (j = 0; j < 4; j++)
            if (KIND(n, j) == K_VAR) link_var(vm, &le, n->a[j], n->line, n->col);
        switch (n->op) {
        case OP_CALL:
            if (!(vm->syms[n->body].flags & GLYPH_SYM_PROC))
                link_report(&le, GLYPH_E_UNKNOWN, n->line, n->col,
                            "unknown command or procedure ", vm->syms[n->body].name, 0);
            break;
        case OP_ADD:
            link_var(vm, &le, n->a[0], n->line, n->col);
            break;
        case OP_ENABLE:
        case OP_DISABLE: {
            const glyph_sym *s = &vm->syms[n->a[0]];
            if (s->comp < 0)
                link_report(&le, GLYPH_E_UNDEFINED, n->line, n->col, "no component named ", s->name, 0);
            else if (vm->comps[s->comp].type != GLYPH_COMP_BUTTON && vm->comps[s->comp].type != GLYPH_COMP_FIELD)
                link_report(&le, GLYPH_E_KIND, n->line, n->col, glyph_op_name(n->op), " applies only to a BUTTON or FIELD: ", s->name);
            break;
        }
        }
    }
    for (i = 0; i < vm->n_comps; i++) {
        glyph_comp *c = &vm->comps[i];
        if (c->x < 0 || c->x >= vm->w || c->y < 0 || c->y >= vm->h)
            link_report(&le, GLYPH_E_RANGE, c->line, c->col, "component ", vm->syms[c->sym].name, " starts outside the screen");
        if (c->type == GLYPH_COMP_LABEL && c->bind_kind == K_VAR)
            link_var(vm, &le, c->bind, c->line, c->col);
        if ((c->type == GLYPH_COMP_BUTTON || c->type == GLYPH_COMP_FIELD) &&
            !(vm->syms[c->action].flags & GLYPH_SYM_HANDLER))
            link_report(&le, GLYPH_E_UNDEFINED, c->line, c->col, "no ON handler for action ", vm->syms[c->action].name, 0);
    }
    return le.found ? d->code : GLYPH_OK;
}

/* ---- program ---------------------------------------------------------------- */
int parse_program(glyph_vm *vm, glyph_diag *d)
{
    seq main = { 0, 0 };
    NEXT();
    while (LX->type != TOK_EOF) {
        const kw *k = (LX->type == TOK_WORD) ? lookup_kw(LX->text) : 0;
        if (k && k->kind == W_DECL) {
            TRY(parse_decl(vm, k, d));
        } else {
            int n;
            TRY(parse_stmt(vm, 0, &n, d));
            seq_add(vm, &main, n);
        }
    }
    vm->main_head = main.head;
    vm->main_tail = main.tail;
    return link_program(vm, d);
}

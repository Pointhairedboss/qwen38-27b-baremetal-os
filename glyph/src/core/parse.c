/* parse.c — the compiler: source text -> stack bytecode + tables.
 *
 * Two passes over the token stream:
 *   1. pre-scan: record the input count of every `TO name :a :b` so calls can
 *      be parsed before the definition (LOGO parses calls by arity);
 *   2. compile: recursive descent for statements, precedence climbing for
 *      expressions, emitting bytecode directly.
 * Recursion is bounded by GLYPH_MAX_NEST (lists/bodies) and
 * GLYPH_MAX_EXPR_DEPTH (expressions), each checked before descending.
 *
 * Scoping is lexical: inside a TO body, a name that is an input or a LOCAL
 * of that procedure is a local slot; every other name is a global. Names in a
 * TO body are resolved when its END is reached (LOCAL may appear anywhere in
 * the body); names elsewhere are resolved as they are compiled.
 */
#include "glyph_internal.h"

/* ---- reserved words ---------------------------------------------------------- */
enum { W_CMD = 1, W_DECL, W_END, W_REPEAT, W_WHILE, W_IF, W_IFELSE, W_STOP, W_OUTPUT,
       W_LOCAL, W_REPORTER };
enum { D_SCREEN = 1, D_PANEL, D_LABEL, D_BUTTON, D_FIELD, D_TO, D_ON, D_ARRAY };

typedef struct kw {
    const char *word;
    uint8_t     kind;   /* W_* */
    uint8_t     op;     /* OP_* or D_* */
    const char *sig;    /* inputs: e=expression S=SET target R=ADD target C=component A=array */
} kw;

static const kw keywords[] = {
    /* turtle */
    { "FD", W_CMD, OP_FD, "e" },          { "FORWARD", W_CMD, OP_FD, "e" },
    { "BK", W_CMD, OP_BK, "e" },          { "BACK", W_CMD, OP_BK, "e" },
    { "RT", W_CMD, OP_RT, "e" },          { "RIGHT", W_CMD, OP_RT, "e" },
    { "LT", W_CMD, OP_LT, "e" },          { "LEFT", W_CMD, OP_LT, "e" },
    { "PU", W_CMD, OP_PU, "" },           { "PENUP", W_CMD, OP_PU, "" },
    { "PD", W_CMD, OP_PD, "" },           { "PENDOWN", W_CMD, OP_PD, "" },
    { "HOME", W_CMD, OP_HOME, "" },       { "SETXY", W_CMD, OP_SETXY, "ee" },
    { "SETHEADING", W_CMD, OP_SETH, "e" },{ "SETH", W_CMD, OP_SETH, "e" },
    /* control */
    { "REPEAT", W_REPEAT, 0, 0 },         { "WHILE", W_WHILE, 0, 0 },
    { "IF", W_IF, 0, 0 },                 { "IFELSE", W_IFELSE, 0, 0 },
    { "STOP", W_STOP, 0, 0 },
    { "OUTPUT", W_OUTPUT, 0, 0 },         { "OP", W_OUTPUT, 0, 0 },
    { "LOCAL", W_LOCAL, 0, 0 },
    /* drawing */
    { "CLEARSCREEN", W_CMD, OP_CS, "" },  { "CS", W_CMD, OP_CS, "" },
    { "CLEAR", W_CMD, OP_CLEAR, "" },
    { "COLOR", W_CMD, OP_COLOR, "ee" },   { "SETCOLOR", W_CMD, OP_SETCOLOR, "e" },
    { "TEXT", W_CMD, OP_TEXT, "eee" },
    { "LINE", W_CMD, OP_LINE, "eeee" },
    { "RECT", W_CMD, OP_RECT, "eeee" },
    { "BOX", W_CMD, OP_BOX, "eeee" },
    /* state */
    { "SET", W_CMD, OP_STOREN, "Se" },
    { "ADD", W_CMD, OP_ADDVN, "Re" },
    { "SETITEM", W_CMD, OP_SETITEMN, "Aee" },
    { "ENABLE", W_CMD, OP_ENABLE, "C" },  { "DISABLE", W_CMD, OP_DISABLE, "C" },
    /* reporters */
    { "TRUE", W_REPORTER, OP_PUSH_INT, "" },  { "FALSE", W_REPORTER, OP_PUSH_INT, "" },
    { "AND", W_REPORTER, OP_AND, "ee" },  { "OR", W_REPORTER, OP_OR, "ee" },
    { "NOT", W_REPORTER, OP_NOT, "e" },
    { "REMAINDER", W_REPORTER, OP_REM, "ee" },
    { "WORD", W_REPORTER, OP_WORD, "ee" },
    { "ITEM", W_REPORTER, OP_ITEMN, "Ae" },
    { "XCOR", W_REPORTER, OP_XCOR, "" },  { "YCOR", W_REPORTER, OP_YCOR, "" },
    { "HEADING", W_REPORTER, OP_HEADING, "" },
    /* declarations */
    { "SCREEN", W_DECL, D_SCREEN, 0 },    { "PANEL", W_DECL, D_PANEL, 0 },
    { "LABEL", W_DECL, D_LABEL, 0 },      { "BUTTON", W_DECL, D_BUTTON, 0 },
    { "FIELD", W_DECL, D_FIELD, 0 },      { "TO", W_DECL, D_TO, 0 },
    { "ON", W_DECL, D_ON, 0 },            { "ARRAY", W_DECL, D_ARRAY, 0 },
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
        "INVALID", "PUSH", "PUSH", "LOAD", "LOAD", "LOAD", "ITEM", "ITEM",
        "NEGATE", "+", "-", "*", "/", "REMAINDER",
        "=", "<>", "<", ">", "<=", ">=",
        "AND", "OR", "NOT", "WORD", "XCOR", "YCOR", "HEADING",
        "CALL", "JMP", "RET", "IF", "WHILE", "REPEAT", "REPEAT-NEXT",
        "BLOCK", "LOCAL", "CALL", "STOP", "OUTPUT",
        "FD", "BK", "RT", "LT", "PU", "PD", "HOME", "SETXY", "SETHEADING",
        "CLEARSCREEN", "CLEAR", "COLOR", "SETCOLOR", "TEXT", "LINE", "RECT", "BOX",
        "SET", "SET", "SET", "ADD", "ADD", "ADD", "SETITEM", "SETITEM",
        "ENABLE", "DISABLE"
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
    g_zero(s, sizeof *s);
    g_copy(s->name, name, g_strlen(name) + 1);
    s->comp = -1; s->state = -1; s->array = -1;
    s->line = (uint16_t)line; s->col = (uint16_t)col;
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

/* Global variable slot for sym, allocated on first use. */
static int global_slot(glyph_vm *vm, int sym, int line, int col, glyph_diag *d)
{
    glyph_sym *s = &vm->syms[sym];
    if (s->state < 0) {
        glyph_state *st;
        if (vm->n_state >= GLYPH_MAX_STATE) {
            g_diag_num(d, GLYPH_E_LIMIT, line, col, "more than ", GLYPH_MAX_STATE, " global variables");
            return -GLYPH_E_LIMIT;
        }
        s->state = (int8_t)vm->n_state;
        st = &vm->state[vm->n_state++];
        g_zero(st, sizeof *st);
        st->sym = (uint16_t)sym;
    }
    return s->state;
}

static int emit(glyph_vm *vm, int op, int32_t arg, int line, int col, glyph_diag *d)
{
    glyph_insn *in;
    if (vm->n_code >= GLYPH_MAX_CODE) {
        g_diag_num(d, GLYPH_E_CODE, line, col, "program needs more than ", GLYPH_MAX_CODE - 1, " instructions");
        return -GLYPH_E_CODE;
    }
    in = &vm->code[vm->n_code];
    in->op = (uint8_t)op;
    in->col = (uint8_t)(col > 255 ? 255 : col);
    in->line = (uint16_t)line;
    in->arg = arg;
    return vm->n_code++;
}

/* ---- token helpers ---------------------------------------------------------- */
#define LX (&vm->lex)
#define PS (&vm->ps)
#define NEXT() do { int e_ = lex_next(LX, d); if (e_) return e_; } while (0)
#define TRY(x) do { int e_ = (x); if (e_) return e_; } while (0)
#define EMIT(op, arg, line, col) do { int i_ = emit(vm, (op), (arg), (line), (col), d); if (i_ < 0) return -i_; } while (0)

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
    case TOK_LPAREN: return "'('";
    case TOK_RPAREN: return "')'";
    case TOK_OP: return "an operator";
    default: return "a word";
    }
}

static int check_name(glyph_vm *vm, glyph_diag *d)
{
    if (is_reserved(LX->text))
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, "'", LX->text, "' is a reserved word and cannot be a name");
    return GLYPH_OK;
}

/* Current token must be a non-reserved word; returns its symbol in *sym. */
static int expect_name(glyph_vm *vm, glyph_diag *d, const char *who, int *sym)
{
    int s;
    if (LX->type != TOK_WORD)
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, who, " expects a name, found ", tok_desc(LX->type));
    TRY(check_name(vm, d));
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

/* ---- variables ----------------------------------------------------------------- */
enum { V_LOAD, V_STORE, V_ADD };

/* Resolve a global variable access and emit it. */
static int emit_global(glyph_vm *vm, int kind, int sym, int line, int col, glyph_diag *d)
{
    static const uint8_t ops[3] = { OP_LOAD_G, OP_STORE_G, OP_ADDV_G };
    int slot = global_slot(vm, sym, line, col, d);
    if (slot < 0) return -slot;
    vm->syms[sym].flags |= (kind == V_STORE) ? GLYPH_SYM_ASSIGNED : GLYPH_SYM_READ;
    EMIT(ops[kind], slot, line, col);
    return GLYPH_OK;
}

/* Inside a TO body the decision local/global waits for END. */
static int emit_var(glyph_vm *vm, int kind, int sym, int line, int col, glyph_diag *d)
{
    static const uint8_t nops[3] = { OP_LOADN, OP_STOREN, OP_ADDVN };
    if (PS->in_proc) { EMIT(nops[kind], sym, line, col); return GLYPH_OK; }
    return emit_global(vm, kind, sym, line, col, d);
}

/* Local slot of sym in the procedure being compiled, or -1. */
static int local_index(const glyph_vm *vm, int sym)
{
    int j;
    for (j = 0; j < PS->nlocal_names; j++) {
        if (PS->local_names[j] != sym) continue;
        if (j < PS->nparams) return j;
        return PS->nparams + PS->max_loop + (j - PS->nparams);
    }
    return -1;
}

/* Rewrite the unresolved name instructions of a finished TO body. */
static int resolve_proc(glyph_vm *vm, int from, glyph_diag *d)
{
    int i;
    for (i = from; i < vm->n_code; i++) {
        glyph_insn *in = &vm->code[i];
        int kind, idx, slot;
        if (in->op == OP_LOADN) kind = V_LOAD;
        else if (in->op == OP_STOREN) kind = V_STORE;
        else if (in->op == OP_ADDVN) kind = V_ADD;
        else continue;
        idx = local_index(vm, in->arg);
        if (idx >= 0) {
            in->op = (uint8_t)(kind == V_LOAD ? OP_LOAD_L : kind == V_STORE ? OP_STORE_L : OP_ADDV_L);
            in->arg = (in->arg << 8) | idx;
            continue;
        }
        slot = global_slot(vm, in->arg, in->line, in->col, d);
        if (slot < 0) return -slot;
        vm->syms[in->arg].flags |= (kind == V_STORE) ? GLYPH_SYM_ASSIGNED : GLYPH_SYM_READ;
        in->op = (uint8_t)(kind == V_LOAD ? OP_LOAD_G : kind == V_STORE ? OP_STORE_G : OP_ADDV_G);
        in->arg = slot;
    }
    return GLYPH_OK;
}

/* ---- expressions ----------------------------------------------------------------- */
static int parse_expr(glyph_vm *vm, const char *who, glyph_diag *d);

static int depth_in(glyph_vm *vm, glyph_diag *d)
{
    if (++PS->expr_depth > GLYPH_MAX_EXPR_DEPTH)
        return err_at_tok(vm, d, GLYPH_E_NEST, "expression nested deeper than 16", 0, 0);
    return GLYPH_OK;
}

static int not_enough(glyph_vm *vm, glyph_diag *d, const char *who)
{
    return err_at_tok(vm, d, GLYPH_E_SYNTAX, "not enough inputs to ", who, 0);
}

/* Parse the inputs described by sig; a name input is returned in *named. */
static int parse_inputs(glyph_vm *vm, const char *sig, const char *who, int *named, glyph_diag *d)
{
    int i;
    for (i = 0; sig[i]; i++) {
        if (sig[i] == 'e') { TRY(parse_expr(vm, who, d)); continue; }
        if (LX->type != TOK_WORD) {
            if (LX->type == TOK_VAR)
                return err_at_tok(vm, d, GLYPH_E_SYNTAX, who, " takes a bare name here, not :", LX->text);
            return err_at_tok(vm, d, GLYPH_E_SYNTAX, who, " expects a name, found ", tok_desc(LX->type));
        }
        TRY(expect_name(vm, d, who, named));
        if (sig[i] == 'C') vm->syms[*named].flags |= GLYPH_SYM_COMPREF;
        NEXT();
    }
    return GLYPH_OK;
}

static int parse_call_args(glyph_vm *vm, int sym, glyph_diag *d)
{
    int i;
    for (i = 0; i < vm->syms[sym].arity; i++) TRY(parse_expr(vm, vm->syms[sym].name, d));
    return GLYPH_OK;
}

static int parse_primary(glyph_vm *vm, const char *who, glyph_diag *d)
{
    int line = LX->tline, col = LX->tcol;
    switch (LX->type) {
    case TOK_INT:
        EMIT(OP_PUSH_INT, LX->ival, line, col);
        NEXT();
        return GLYPH_OK;
    case TOK_STR: {
        int o = pool_add(vm, LX->text, LX->tlen, line, col, d);
        if (o < 0) return -o;
        EMIT(OP_PUSH_STR, (int32_t)(((uint32_t)o << 8) | LX->tlen), line, col);
        NEXT();
        return GLYPH_OK;
    }
    case TOK_VAR: {
        int s;
        TRY(check_name(vm, d));
        s = sym_intern(vm, LX->text, line, col, d);
        if (s < 0) return -s;
        TRY(emit_var(vm, V_LOAD, s, line, col, d));
        NEXT();
        return GLYPH_OK;
    }
    case TOK_LPAREN:
        NEXT();
        TRY(parse_expr(vm, who, d));
        if (LX->type != TOK_RPAREN)
            return err_at_tok(vm, d, GLYPH_E_SYNTAX, "missing ')', found ", tok_desc(LX->type), 0);
        NEXT();
        return GLYPH_OK;
    case TOK_WORD: {
        const kw *k = lookup_kw(LX->text);
        int s, named = 0;
        if (k && k->kind == W_REPORTER) {
            NEXT();
            if (k->op == OP_PUSH_INT) { EMIT(OP_PUSH_INT, k->word[0] == 'T' ? 1 : 0, line, col); return GLYPH_OK; }
            TRY(parse_inputs(vm, k->sig, k->word, &named, d));
            EMIT(k->op, named, line, col);
            return GLYPH_OK;
        }
        if (k) return not_enough(vm, d, who);
        s = sym_find(vm, LX->text);
        if (s < 0 || !(vm->syms[s].flags & GLYPH_SYM_PROCDECL))
            return err_at_tok(vm, d, GLYPH_E_UNKNOWN, "unknown procedure ", LX->text, 0);
        NEXT();
        TRY(parse_call_args(vm, s, d));
        EMIT(OP_CALLV, s, line, col);
        return GLYPH_OK;
    }
    case TOK_OP:
        if (LX->op == TOP_MINUS) {
            NEXT();
            TRY(depth_in(vm, d));
            TRY(parse_primary(vm, who, d));
            PS->expr_depth--;
            EMIT(OP_NEG, 0, line, col);
            return GLYPH_OK;
        }
        return not_enough(vm, d, who);
    default:
        return not_enough(vm, d, who);
    }
}

/* Binary operator precedence of the current token, 0 if it ends the input. */
static int binop_prec(const glyph_lexer *lx)
{
    if (lx->type != TOK_OP) return 0;
    switch (lx->op) {
    case TOP_MUL: case TOP_DIV: return 3;
    case TOP_PLUS: return 2;
    case TOP_MINUS: return lx->unary ? 0 : 2;
    default: return 1;                       /* comparisons */
    }
}

static int parse_binary(glyph_vm *vm, int minp, const char *who, glyph_diag *d)
{
    static const uint8_t ops[] = { 0, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_EQ, OP_NE, OP_LESS, OP_GREATER, OP_LESSEQ, OP_GREATEREQ };
    TRY(parse_primary(vm, who, d));
    for (;;) {
        int p = binop_prec(LX), op = LX->op, line = LX->tline, col = LX->tcol;
        if (p == 0 || p < minp) return GLYPH_OK;
        NEXT();
        TRY(parse_binary(vm, p + 1, who, d));
        EMIT(ops[op], 0, line, col);
    }
}

/* One input: a full expression (comparisons bind loosest). */
static int parse_expr(glyph_vm *vm, const char *who, glyph_diag *d)
{
    TRY(depth_in(vm, d));
    TRY(parse_binary(vm, 1, who, d));
    PS->expr_depth--;
    return GLYPH_OK;
}

/* ---- statements ------------------------------------------------------------- */
static int parse_stmt(glyph_vm *vm, int depth, glyph_diag *d);

/* Current token is '['. The list itself sits at nesting level `depth`. */
static int parse_list(glyph_vm *vm, int depth, glyph_diag *d)
{
    int line = LX->tline, col = LX->tcol;
    if (depth > GLYPH_MAX_NEST)
        return err_at_tok(vm, d, GLYPH_E_NEST, "lists nested deeper than 16", 0, 0);
    NEXT();
    while (LX->type != TOK_RBRACK) {
        if (LX->type == TOK_EOF) {
            g_diag(d, GLYPH_E_UNCLOSED, line, col, "'[' opened here has no matching ']'", 0, 0);
            return GLYPH_E_UNCLOSED;
        }
        TRY(parse_stmt(vm, depth, d));
    }
    NEXT();
    return GLYPH_OK;
}

/* A body: a list, or one statement. The body sits at nesting level depth+1. */
static int parse_body(glyph_vm *vm, int depth, glyph_diag *d)
{
    if (LX->type == TOK_LBRACK) return parse_list(vm, depth + 1, d);
    if (depth + 1 > GLYPH_MAX_NEST)
        return err_at_tok(vm, d, GLYPH_E_NEST, "bodies nested deeper than 16", 0, 0);
    return parse_stmt(vm, depth + 1, d);
}

static int parse_repeat(glyph_vm *vm, int depth, int line, int col, glyph_diag *d)
{
    int slot = PS->nparams + PS->loop_depth, top;
    TRY(parse_expr(vm, "REPEAT", d));
    if (slot >= GLYPH_MAX_UNIT_LOCALS)
        return err_at_tok(vm, d, GLYPH_E_LIMIT, "too many nested REPEATs in one procedure", 0, 0);
    EMIT(OP_REP_INIT, slot, line, col);
    top = emit(vm, OP_REP_NEXT, 0, line, col, d);
    if (top < 0) return -top;
    PS->loop_depth++;
    if (PS->loop_depth > PS->max_loop) PS->max_loop = PS->loop_depth;
    TRY(parse_body(vm, depth, d));
    PS->loop_depth--;
    EMIT(OP_JMP, top, line, col);
    vm->code[top].arg = (slot << 16) | vm->n_code;
    return GLYPH_OK;
}

static int parse_while(glyph_vm *vm, int depth, int line, int col, glyph_diag *d)
{
    int top = vm->n_code, jz;
    TRY(parse_expr(vm, "WHILE", d));
    jz = emit(vm, OP_WHILE_JZ, 0, line, col, d);
    if (jz < 0) return -jz;
    TRY(parse_body(vm, depth, d));
    EMIT(OP_JMP, top, line, col);
    vm->code[jz].arg = vm->n_code;
    return GLYPH_OK;
}

/* IF cond body [else-list]   and   IFELSE cond body body */
static int parse_if(glyph_vm *vm, int depth, int is_ifelse, int line, int col, glyph_diag *d)
{
    int jz, j;
    TRY(parse_expr(vm, is_ifelse ? "IFELSE" : "IF", d));
    jz = emit(vm, OP_IF_JZ, 0, line, col, d);
    if (jz < 0) return -jz;
    TRY(parse_body(vm, depth, d));
    if (!is_ifelse && LX->type != TOK_LBRACK) {     /* UCBLogo: IF c [a] [b] means IFELSE */
        vm->code[jz].arg = vm->n_code;
        return GLYPH_OK;
    }
    j = emit(vm, OP_JMP, 0, line, col, d);
    if (j < 0) return -j;
    vm->code[jz].arg = vm->n_code;
    TRY(parse_body(vm, depth, d));
    vm->code[j].arg = vm->n_code;
    return GLYPH_OK;
}

static int parse_call(glyph_vm *vm, int line, int col, glyph_diag *d)
{
    int s = sym_find(vm, LX->text);
    if (s < 0 || !(vm->syms[s].flags & GLYPH_SYM_PROCDECL))
        return err_at_tok(vm, d, GLYPH_E_UNKNOWN, "unknown command ", LX->text, 0);
    NEXT();
    TRY(parse_call_args(vm, s, d));
    EMIT(OP_CALL, s, line, col);
    if (LX->type == TOK_INT || LX->type == TOK_STR || LX->type == TOK_VAR || LX->type == TOK_LPAREN) {
        glyph_buf b;
        d->code = GLYPH_E_SYNTAX; d->line = line; d->col = col;
        glyph_buf_init(&b, d->msg, sizeof d->msg);
        glyph_buf_str(&b, "too many inputs to ");
        glyph_buf_str(&b, vm->syms[s].name);
        glyph_buf_str(&b, " (it takes ");
        glyph_buf_int(&b, vm->syms[s].arity);
        glyph_buf_ch(&b, ')');
        return GLYPH_E_SYNTAX;
    }
    return GLYPH_OK;
}

static int parse_stmt(glyph_vm *vm, int depth, glyph_diag *d)
{
    const kw *k;
    int line = LX->tline, col = LX->tcol, named = 0;

    switch (LX->type) {
    case TOK_LBRACK:
        EMIT(OP_BLOCK, 0, line, col);
        return parse_list(vm, depth + 1, d);
    case TOK_RBRACK:
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, "unexpected ']'", 0, 0);
    case TOK_WORD:
        break;
    default:
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, "expected a command, found ", tok_desc(LX->type), 0);
    }

    k = lookup_kw(LX->text);
    if (!k) return parse_call(vm, line, col, d);
    switch (k->kind) {
    case W_DECL:
        return err_at_tok(vm, d, GLYPH_E_TOPLEVEL, k->word, " is only allowed at the top level", 0);
    case W_END:
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, "END without TO", 0, 0);
    case W_REPORTER:
        return err_at_tok(vm, d, GLYPH_E_SYNTAX, k->word, " reports a value; say what to do with it", 0);
    case W_REPEAT: NEXT(); return parse_repeat(vm, depth, line, col, d);
    case W_WHILE:  NEXT(); return parse_while(vm, depth, line, col, d);
    case W_IF:     NEXT(); return parse_if(vm, depth, 0, line, col, d);
    case W_IFELSE: NEXT(); return parse_if(vm, depth, 1, line, col, d);
    case W_STOP:
        NEXT();
        EMIT(OP_STOP, 0, line, col);
        return GLYPH_OK;
    case W_OUTPUT:
        if (!PS->in_proc) return err_at_tok(vm, d, GLYPH_E_SYNTAX, "OUTPUT is only allowed inside TO", 0, 0);
        NEXT();
        TRY(parse_expr(vm, "OUTPUT", d));
        EMIT(OP_OUTPUT, 0, line, col);
        return GLYPH_OK;
    case W_LOCAL: {
        int s, j;
        if (!PS->in_proc) return err_at_tok(vm, d, GLYPH_E_SYNTAX, "LOCAL is only allowed inside TO", 0, 0);
        NEXT();
        TRY(expect_name(vm, d, "LOCAL", &s));
        for (j = 0; j < PS->nlocal_names && PS->local_names[j] != s; j++) {}
        if (j == PS->nlocal_names) {
            if (PS->nlocal_names >= GLYPH_MAX_UNIT_LOCALS)
                return err_at_tok(vm, d, GLYPH_E_LIMIT, "more than 32 inputs and LOCALs", 0, 0);
            PS->local_names[PS->nlocal_names++] = (uint16_t)s;
        }
        NEXT();
        EMIT(OP_LOCAL, 0, line, col);
        return GLYPH_OK;
    }
    }

    /* fixed-signature command */
    NEXT();
    TRY(parse_inputs(vm, k->sig, k->word, &named, d));
    if (k->op == OP_STOREN) return emit_var(vm, V_STORE, named, line, col, d);
    if (k->op == OP_ADDVN)  return emit_var(vm, V_ADD, named, line, col, d);
    EMIT(k->op, named, line, col);
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

static int parse_size(glyph_vm *vm, glyph_diag *d, int lo, int hi, int16_t *out)
{
    int32_t v;
    TRY(expect_lit(vm, d, "size", &v));
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

static int new_unit(glyph_vm *vm, int sym, int nparams, glyph_diag *d)
{
    glyph_unit *u = &vm->units[vm->n_units];
    (void)d;
    g_zero(u, sizeof *u);
    u->sym = (uint16_t)sym;
    u->nparams = (uint8_t)nparams;
    return vm->n_units++;
}

/* TO name :in1 :in2 ... statements END */
static int parse_to(glyph_vm *vm, int line, int col, glyph_diag *d)
{
    int s, jmp, unit, nlocals;
    uint8_t main_max = PS->max_loop;
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
    g_zero(PS, sizeof *PS);
    PS->in_proc = 1;
    NEXT();
    while (LX->type == TOK_VAR) {                    /* inputs */
        int p, j;
        TRY(check_name(vm, d));
        p = sym_intern(vm, LX->text, LX->tline, LX->tcol, d);
        if (p < 0) return -p;
        for (j = 0; j < PS->nlocal_names; j++)
            if (PS->local_names[j] == p)
                return err_at_tok(vm, d, GLYPH_E_REDEFINE, "input :", LX->text, " is listed twice");
        if (PS->nlocal_names >= GLYPH_MAX_UNIT_LOCALS)
            return err_at_tok(vm, d, GLYPH_E_LIMIT, "more than 32 inputs", 0, 0);
        PS->local_names[PS->nlocal_names++] = (uint16_t)p;
        NEXT();
    }
    PS->nparams = PS->nlocal_names;
    unit = new_unit(vm, s, PS->nparams, d);
    PS->unit = (uint16_t)unit;
    vm->syms[s].proc_unit = (uint8_t)unit;
    jmp = emit(vm, OP_JMP, 0, line, col, d);
    if (jmp < 0) return -jmp;
    vm->units[unit].entry = vm->n_code;
    for (;;) {
        if (LX->type == TOK_WORD && g_streq(LX->text, "END")) break;
        if (LX->type == TOK_EOF) {
            g_diag(d, GLYPH_E_UNCLOSED, line, col, "TO ", vm->syms[s].name, " has no matching END");
            return GLYPH_E_UNCLOSED;
        }
        TRY(parse_stmt(vm, 1, d));
    }
    EMIT(OP_RET, 0, LX->tline, LX->tcol);
    NEXT();
    TRY(resolve_proc(vm, vm->units[unit].entry, d));
    nlocals = PS->nparams + PS->max_loop + (PS->nlocal_names - PS->nparams);
    if (nlocals > GLYPH_MAX_UNIT_LOCALS) {
        g_diag(d, GLYPH_E_LIMIT, line, col, "procedure ", vm->syms[s].name, " needs more than 32 local slots");
        return GLYPH_E_LIMIT;
    }
    vm->units[unit].nlocals = (uint8_t)nlocals;
    vm->code[jmp].arg = vm->n_code;
    g_zero(PS, sizeof *PS);
    PS->max_loop = main_max;
    return GLYPH_OK;
}

/* ON action body */
static int parse_on(glyph_vm *vm, int line, int col, glyph_diag *d)
{
    int s, jmp, unit;
    uint8_t main_max = PS->max_loop;
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
    g_zero(PS, sizeof *PS);
    unit = new_unit(vm, s, 0, d);
    vm->syms[s].handler_unit = (uint8_t)unit;
    jmp = emit(vm, OP_JMP, 0, line, col, d);
    if (jmp < 0) return -jmp;
    vm->units[unit].entry = vm->n_code;
    TRY(parse_body(vm, 0, d));
    EMIT(OP_RET, 0, line, col);
    vm->units[unit].nlocals = PS->max_loop;
    vm->code[jmp].arg = vm->n_code;
    g_zero(PS, sizeof *PS);
    PS->max_loop = main_max;
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
        TRY(parse_size(vm, d, 2, GLYPH_MAX_W, &c->w));
        TRY(parse_size(vm, d, 2, GLYPH_MAX_H, &c->h));
        TRY(expect_str(vm, d, "PANEL", &c->text, &c->text_len));
        NEXT();
        return GLYPH_OK;
    case D_LABEL:
        TRY(new_comp(vm, GLYPH_COMP_LABEL, "LABEL", line, col, d, &c));
        TRY(parse_xy(vm, d, "LABEL", c));
        if (LX->type == TOK_INT) {
            c->bind_kind = K_INT; c->bind = LX->ival;
        } else if (LX->type == TOK_STR) {
            uint16_t off; uint8_t len;
            TRY(expect_str(vm, d, "LABEL", &off, &len));
            c->bind_kind = K_STR; c->bind = (int32_t)(((uint32_t)off << 8) | len);
        } else if (LX->type == TOK_VAR) {
            int s;
            TRY(check_name(vm, d));
            s = sym_intern(vm, LX->text, LX->tline, LX->tcol, d);
            if (s < 0) return -s;
            if (global_slot(vm, s, LX->tline, LX->tcol, d) < 0) return d->code;
            vm->syms[s].flags |= GLYPH_SYM_READ;
            c->bind_kind = K_VAR; c->bind = s;
        } else {
            return err_at_tok(vm, d, GLYPH_E_SYNTAX, "LABEL expects a number, \"string\" or :variable, found ",
                              tok_desc(LX->type), 0);
        }
        NEXT();
        return GLYPH_OK;
    case D_BUTTON:
        TRY(new_comp(vm, GLYPH_COMP_BUTTON, "BUTTON", line, col, d, &c));
        TRY(parse_xy(vm, d, "BUTTON", c));
        TRY(expect_str(vm, d, "BUTTON", &c->text, &c->text_len));
        NEXT();
        c->w = (int16_t)(c->text_len + 4); c->h = 1;
        return parse_action_ref(vm, d, "BUTTON", c);
    case D_FIELD: {
        int16_t width;
        int slot;
        TRY(new_comp(vm, GLYPH_COMP_FIELD, "FIELD", line, col, d, &c));
        slot = global_slot(vm, c->sym, c->line, c->col, d);
        if (slot < 0) return -slot;
        vm->syms[c->sym].flags |= GLYPH_SYM_ASSIGNED;
        val_str(&vm->state[slot].v, "", 0);
        TRY(parse_xy(vm, d, "FIELD", c));
        TRY(parse_size(vm, d, 1, GLYPH_MAX_STR_LEN, &width));
        c->w = (int16_t)(width + 2); c->h = 1;
        return parse_action_ref(vm, d, "FIELD", c);
    }
    case D_ARRAY: {
        int s;
        int32_t size;
        glyph_array *a;
        TRY(expect_name(vm, d, "ARRAY", &s));
        if (vm->syms[s].array >= 0)
            return err_at_tok(vm, d, GLYPH_E_REDEFINE, "ARRAY ", LX->text, " is already declared");
        if (vm->n_arrays >= GLYPH_MAX_ARRAYS) {
            g_diag_num(d, GLYPH_E_LIMIT, LX->tline, LX->tcol, "more than ", GLYPH_MAX_ARRAYS, " arrays");
            return GLYPH_E_LIMIT;
        }
        NEXT();
        TRY(expect_lit(vm, d, "ARRAY", &size));
        if (size < 1 || size > GLYPH_MAX_ARRAY_CELLS)
            return err_at_tok(vm, d, GLYPH_E_RANGE, "ARRAY size must be 1..2048", 0, 0);
        if (vm->n_cells + size > GLYPH_MAX_ARRAY_CELLS) {
            g_diag_num(d, GLYPH_E_LIMIT, LX->tline, LX->tcol, "arrays need more than ", GLYPH_MAX_ARRAY_CELLS, " cells");
            return GLYPH_E_LIMIT;
        }
        vm->syms[s].array = (int8_t)vm->n_arrays;
        a = &vm->arrays[vm->n_arrays++];
        a->sym = (uint16_t)s; a->base = vm->n_cells; a->size = (uint16_t)size; a->line = (uint16_t)line;
        vm->n_cells = (uint16_t)(vm->n_cells + size);
        NEXT();
        return GLYPH_OK;
    }
    case D_TO: return parse_to(vm, line, col, d);
    case D_ON: return parse_on(vm, line, col, d);
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

static int link_program(glyph_vm *vm, glyph_diag *d)
{
    link_err le;
    int i;
    le.d = d; le.found = 0;

    for (i = 1; i < vm->n_code; i++) {
        glyph_insn *in = &vm->code[i];
        switch (in->op) {
        case OP_LOAD_G:
        case OP_ADDV_G: {
            const glyph_sym *s = &vm->syms[vm->state[in->arg].sym];
            if (!(s->flags & GLYPH_SYM_ASSIGNED))
                link_report(&le, GLYPH_E_UNDEFINED, in->line, in->col, "variable ", s->name, " is never SET");
            break;
        }
        case OP_ITEMN:
        case OP_SETITEMN: {
            const glyph_sym *s = &vm->syms[in->arg];
            if (s->array < 0) {
                link_report(&le, GLYPH_E_UNDEFINED, in->line, in->col, "no ARRAY named ", s->name, 0);
                break;
            }
            in->op = (uint8_t)(in->op == OP_ITEMN ? OP_ITEM : OP_SETITEM);
            in->arg = s->array;
            break;
        }
        case OP_ENABLE:
        case OP_DISABLE: {
            const glyph_sym *s = &vm->syms[in->arg];
            if (s->comp < 0)
                link_report(&le, GLYPH_E_UNDEFINED, in->line, in->col, "no component named ", s->name, 0);
            else if (vm->comps[s->comp].type != GLYPH_COMP_BUTTON && vm->comps[s->comp].type != GLYPH_COMP_FIELD)
                link_report(&le, GLYPH_E_KIND, in->line, in->col, glyph_op_name(in->op),
                            " applies only to a BUTTON or FIELD: ", s->name);
            break;
        }
        case OP_LOADN: case OP_STOREN: case OP_ADDVN:
            link_report(&le, GLYPH_E_ARG, in->line, in->col, "internal: unresolved name", 0, 0);
            break;
        }
    }
    for (i = 0; i < vm->n_comps; i++) {
        const glyph_comp *c = &vm->comps[i];
        if (c->x < 0 || c->x >= vm->w || c->y < 0 || c->y >= vm->h)
            link_report(&le, GLYPH_E_RANGE, c->line, c->col, "component ", vm->syms[c->sym].name, " starts outside the screen");
        if (c->type == GLYPH_COMP_LABEL && c->bind_kind == K_VAR && !(vm->syms[c->bind].flags & GLYPH_SYM_ASSIGNED))
            link_report(&le, GLYPH_E_UNDEFINED, c->line, c->col, "variable ", vm->syms[c->bind].name, " is never SET");
        if ((c->type == GLYPH_COMP_BUTTON || c->type == GLYPH_COMP_FIELD) &&
            !(vm->syms[c->action].flags & GLYPH_SYM_HANDLER))
            link_report(&le, GLYPH_E_UNDEFINED, c->line, c->col, "no ON handler for action ", vm->syms[c->action].name, 0);
    }
    for (i = 0; i < vm->n_arrays; i++) {
        const glyph_sym *s = &vm->syms[vm->arrays[i].sym];
        if (s->state >= 0)
            link_report(&le, GLYPH_E_REDEFINE, vm->arrays[i].line, 1, s->name, " is both an ARRAY and a variable", 0);
    }
    return le.found ? d->code : GLYPH_OK;
}

/* ---- program ---------------------------------------------------------------- */

/* Pass 1: the input count of every `TO name :a :b ...`. Errors are ignored
 * here; pass 2 reports them at their position. */
static void prescan(glyph_vm *vm, const char *src, uint32_t len)
{
    glyph_diag ignore;
    lex_begin(LX, src, len);
    for (;;) {
        int s, n = 0;
        if (lex_next(LX, &ignore) || LX->type == TOK_EOF) return;
        if (LX->type != TOK_WORD || !g_streq(LX->text, "TO")) continue;
        if (lex_next(LX, &ignore) || LX->type != TOK_WORD || is_reserved(LX->text)) continue;
        s = sym_intern(vm, LX->text, LX->tline, LX->tcol, &ignore);
        if (s < 0) return;
        for (;;) {
            if (lex_next(LX, &ignore)) return;
            if (LX->type != TOK_VAR) break;
            n++;
        }
        if (!(vm->syms[s].flags & GLYPH_SYM_PROCDECL)) {
            vm->syms[s].flags |= GLYPH_SYM_PROCDECL;
            vm->syms[s].arity = (uint8_t)(n > 255 ? 255 : n);
        }
    }
}

int parse_program(glyph_vm *vm, glyph_diag *d)
{
    const char *src = LX->src;
    uint32_t len = LX->len;
    prescan(vm, src, len);
    lex_begin(LX, src, len);
    g_zero(PS, sizeof *PS);
    new_unit(vm, 0xFFFF, 0, d);                      /* unit 0: main */
    vm->units[0].entry = 1;
    NEXT();
    while (LX->type != TOK_EOF) {
        const kw *k = (LX->type == TOK_WORD) ? lookup_kw(LX->text) : 0;
        if (k && k->kind == W_DECL) TRY(parse_decl(vm, k, d));
        else TRY(parse_stmt(vm, 0, d));
    }
    EMIT(OP_RET, 0, LX->tline, LX->tcol);
    vm->units[0].nlocals = PS->max_loop;
    return link_program(vm, d);
}

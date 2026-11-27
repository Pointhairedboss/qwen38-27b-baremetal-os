/* exec.c — the interpreter.
 *
 * Executes IR sequences on an explicit frame stack (no C recursion), so the
 * host stack use is constant whatever the program does. Every entry (start or
 * one input) gets a fresh step budget; statements, loop iterations and drawn
 * cells are charged against it (docs/SPEC.md section 9).
 */
#include "glyph_internal.h"

typedef struct val {
    uint8_t     type;     /* GLYPH_T_INT or GLYPH_T_STR */
    int32_t     i;
    const char *s;
    int         len;
} val;

int format_int(char *out, int32_t v)
{
    char tmp[12];
    int n = 0, len = 0;
    uint32_t u = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    if (v < 0) out[len++] = '-';
    do { tmp[n++] = (char)('0' + (int)(u % 10u)); u /= 10u; } while (u);
    while (n) out[len++] = tmp[--n];
    out[len] = 0;
    return len;
}

static int32_t sat_add(int32_t a, int32_t b)
{
    int64_t s = (int64_t)a + b;
    if (s > GLYPH_INT_MAX) return GLYPH_INT_MAX;
    if (s < -GLYPH_INT_MAX) return -GLYPH_INT_MAX;
    return (int32_t)s;
}

int state_set_int(glyph_vm *vm, int slot, int32_t v)
{
    glyph_state *st = &vm->state[slot];
    if (st->type != GLYPH_T_INT || st->i != v) st->dirty = 1;
    st->type = GLYPH_T_INT;
    st->i = v;
    st->len = 0;
    st->s[0] = 0;
    return GLYPH_OK;
}

int state_set_str(glyph_vm *vm, int slot, const char *s, int n)
{
    glyph_state *st = &vm->state[slot];
    int i, same;
    if (n > GLYPH_MAX_STR_LEN) n = GLYPH_MAX_STR_LEN;
    same = (st->type == GLYPH_T_STR && st->len == n);
    for (i = 0; same && i < n; i++)
        if (st->s[i] != s[i]) same = 0;
    if (!same) {
        st->dirty = 1;
        for (i = 0; i < n; i++) st->s[i] = s[i];
        st->s[n] = 0;
        st->len = (uint8_t)n;
    }
    st->type = GLYPH_T_STR;
    st->i = 0;
    return GLYPH_OK;
}

/* ---- operand evaluation ------------------------------------------------- */
static int rt_err(glyph_diag *d, const glyph_node *n, int code, const char *a, const char *b, const char *c)
{
    g_diag(d, code, n->line, n->col, a, b, c);
    return code;
}

static int eval(glyph_vm *vm, const glyph_node *n, int i, val *out, glyph_diag *d)
{
    switch (KIND(n, i)) {
    case K_INT:
        out->type = GLYPH_T_INT; out->i = n->a[i]; out->s = 0; out->len = 0;
        return GLYPH_OK;
    case K_STR:
        out->type = GLYPH_T_STR; out->i = 0;
        out->s = &vm->strpool[STR_OFF(n->a[i])];
        out->len = (int)STR_LEN(n->a[i]);
        return GLYPH_OK;
    case K_VAR: {
        const glyph_sym *s = &vm->syms[n->a[i]];
        const glyph_state *st = &vm->state[s->state];
        if (st->type == GLYPH_T_UNSET)
            return rt_err(d, n, GLYPH_E_UNSET, "variable ", s->name, " read before SET");
        out->type = st->type; out->i = st->i; out->s = st->s; out->len = st->len;
        return GLYPH_OK;
    }
    }
    return rt_err(d, n, GLYPH_E_ARG, "missing operand", 0, 0);
}

static int eval_int(glyph_vm *vm, const glyph_node *n, int i, int32_t *out, glyph_diag *d)
{
    val v;
    int e = eval(vm, n, i, &v, d);
    if (e) return e;
    if (v.type != GLYPH_T_INT)
        return rt_err(d, n, GLYPH_E_TYPE, glyph_op_name(n->op), " expects a number but got a string from :",
                      KIND(n, i) == K_VAR ? vm->syms[n->a[i]].name : "?");
    *out = v.i;
    return GLYPH_OK;
}

/* Evaluate operand i as an integer within [lo, hi]. */
static int eval_range(glyph_vm *vm, const glyph_node *n, int i, int32_t *out,
                      int32_t lo, int32_t hi, glyph_diag *d)
{
    glyph_buf b;
    int e = eval_int(vm, n, i, out, d);
    if (e) return e;
    if (*out >= lo && *out <= hi) return GLYPH_OK;
    d->code = GLYPH_E_RANGE; d->line = n->line; d->col = n->col;
    glyph_buf_init(&b, d->msg, sizeof d->msg);
    glyph_buf_str(&b, glyph_op_name(n->op));
    glyph_buf_str(&b, " argument ");
    glyph_buf_int(&b, *out);
    glyph_buf_str(&b, " out of range ");
    glyph_buf_int(&b, lo);
    glyph_buf_str(&b, "..");
    glyph_buf_int(&b, hi);
    return GLYPH_E_RANGE;
}

/* Evaluate operands first..first+count-1 into out[first..]. */
static int eval_ints(glyph_vm *vm, const glyph_node *n, int first, int count, int32_t *out,
                     int32_t lo, int32_t hi, glyph_diag *d)
{
    int i;
    for (i = first; i < first + count; i++) {
        int e = eval_range(vm, n, i, &out[i], lo, hi, d);
        if (e) return e;
    }
    return GLYPH_OK;
}

/* ---- execution ------------------------------------------------------------ */
static int charge(glyph_vm *vm, const glyph_node *n, uint32_t cost, glyph_diag *d)
{
    if (cost > vm->budget - vm->steps) {
        vm->steps = vm->budget;
        g_diag_num(d, GLYPH_E_BUDGET, n ? n->line : 0, n ? n->col : 0,
                   "step budget of ", (long long)vm->budget, " exhausted");
        return GLYPH_E_BUDGET;
    }
    vm->steps += cost;
    return GLYPH_OK;
}

static int push(glyph_vm *vm, const glyph_node *n, uint16_t head, int32_t remaining, int is_call, glyph_diag *d)
{
    glyph_frame *f;
    if (vm->sp >= GLYPH_MAX_FRAMES)
        return rt_err(d, n, GLYPH_E_DEPTH, "execution nested too deeply", 0, 0);
    f = &vm->frames[vm->sp++];
    f->pc = head; f->body = head; f->remaining = remaining; f->is_call = (uint8_t)is_call;
    return GLYPH_OK;
}

#define AVAIL() (vm->budget - vm->steps)
#define CHARGE(n, c) do { int e_ = charge(vm, (n), (c), d); if (e_) return e_; } while (0)
#define TRY(x) do { int e_ = (x); if (e_) return e_; } while (0)

static int exec_node(glyph_vm *vm, const glyph_node *n, glyph_diag *d)
{
    int32_t a[4];
    uint32_t cost;
    switch (n->op) {
    case OP_BLOCK:
        return push(vm, n, n->body, 0, 0, d);
    case OP_CALL:
        if (vm->depth >= GLYPH_MAX_CALL_DEPTH)
            return rt_err(d, n, GLYPH_E_DEPTH, "procedure calls nested deeper than 16 (", vm->syms[n->body].name, ")");
        TRY(push(vm, n, vm->syms[n->body].proc_body, 0, 1, d));
        vm->depth++;
        return GLYPH_OK;
    case OP_FD:
    case OP_BK: {
        uint32_t used;
        TRY(eval_ints(vm, n, 0, 1, a, -GLYPH_MAX_DIST, GLYPH_MAX_DIST, d));
        if (turtle_move(vm, n->op == OP_BK ? -a[0] : a[0], AVAIL(), &used))
            return charge(vm, n, used, d);
        vm->steps += used;
        return GLYPH_OK;
    }
    case OP_RT:
    case OP_LT: {
        int32_t t;
        TRY(eval_int(vm, n, 0, &a[0], d));
        t = a[0] % 360;
        if (n->op == OP_LT) t = -t;
        vm->heading = (vm->heading + t + 360) % 360;
        return GLYPH_OK;
    }
    case OP_PU: vm->pen = 0; return GLYPH_OK;
    case OP_PD: vm->pen = 1; return GLYPH_OK;
    case OP_REPEAT:
        TRY(eval_ints(vm, n, 0, 1, a, 0, GLYPH_MAX_REPEAT, d));
        if (a[0] == 0) return GLYPH_OK;
        CHARGE(n, 1);
        return push(vm, n, n->body, a[0] - 1, 0, d);
    case OP_CS:
        CHARGE(n, (uint32_t)vm->w * vm->h);
        canvas_clear(vm);
        turtle_home(vm);
        return GLYPH_OK;
    case OP_CLEAR:
        CHARGE(n, (uint32_t)vm->w * vm->h);
        canvas_clear(vm);
        return GLYPH_OK;
    case OP_COLOR:
        TRY(eval_ints(vm, n, 0, 1, a, 0, 15, d));
        TRY(eval_ints(vm, n, 1, 1, a, 0, 7, d));
        vm->attr = (uint8_t)((a[1] << 4) | a[0]);
        return GLYPH_OK;
    case OP_SETCOLOR:
        TRY(eval_ints(vm, n, 0, 1, a, 0, 15, d));
        vm->attr = (uint8_t)((vm->attr & 0xF0) | a[0]);
        return GLYPH_OK;
    case OP_TEXT: {
        val v;
        char num[12];
        const char *s;
        int len;
        TRY(eval_ints(vm, n, 0, 2, a, -GLYPH_MAX_COORD, GLYPH_MAX_COORD, d));
        TRY(eval(vm, n, 2, &v, d));
        if (v.type == GLYPH_T_INT) { len = format_int(num, v.i); s = num; }
        else { s = v.s; len = v.len; }
        CHARGE(n, (uint32_t)len);
        canvas_text(vm, a[0], a[1], s, len);
        return GLYPH_OK;
    }
    case OP_LINE:
        TRY(eval_ints(vm, n, 0, 4, a, -GLYPH_MAX_COORD, GLYPH_MAX_COORD, d));
        cost = canvas_line(vm, a[0], a[1], a[2], a[3], AVAIL());
        return charge(vm, n, cost, d);
    case OP_RECT:
    case OP_BOX: {
        int32_t minsz = n->op == OP_BOX ? 2 : 0;
        TRY(eval_ints(vm, n, 0, 2, a, -GLYPH_MAX_COORD, GLYPH_MAX_COORD, d));
        TRY(eval_ints(vm, n, 2, 2, a, minsz, GLYPH_MAX_COORD, d));
        if (n->op == OP_RECT) cost = canvas_rect(vm, a[0], a[1], a[2], a[3], AVAIL());
        else cost = canvas_box(vm, a[0], a[1], a[2], a[3], AVAIL());
        return charge(vm, n, cost, d);
    }
    case OP_SET: {
        val v;
        int slot = vm->syms[n->a[0]].state;
        TRY(eval(vm, n, 1, &v, d));
        if (v.type == GLYPH_T_INT) return state_set_int(vm, slot, v.i);
        return state_set_str(vm, slot, v.s, v.len);
    }
    case OP_ADD: {
        const glyph_sym *s = &vm->syms[n->a[0]];
        glyph_state *st = &vm->state[s->state];
        TRY(eval_int(vm, n, 1, &a[1], d));
        if (st->type == GLYPH_T_UNSET)
            return rt_err(d, n, GLYPH_E_UNSET, "ADD to ", s->name, " before it is SET");
        if (st->type != GLYPH_T_INT)
            return rt_err(d, n, GLYPH_E_TYPE, "ADD to ", s->name, " which holds a string");
        return state_set_int(vm, s->state, sat_add(st->i, a[1]));
    }
    case OP_ENABLE:
    case OP_DISABLE: {
        int c = vm->syms[n->a[0]].comp;
        vm->comps[c].enabled = (uint8_t)(n->op == OP_ENABLE);
        if (n->op == OP_DISABLE) ui_on_disable(vm, c);
        return GLYPH_OK;
    }
    }
    return rt_err(d, n, GLYPH_E_ARG, "invalid instruction", 0, 0);
}

int exec_entry(glyph_vm *vm, uint16_t head, glyph_diag *d)
{
    vm->steps = 0;
    vm->sp = 0;
    vm->depth = 0;
    TRY(push(vm, 0, head, 0, 0, d));
    while (vm->sp > 0) {
        glyph_frame *f = &vm->frames[vm->sp - 1];
        const glyph_node *n;
        if (f->pc == 0) {
            if (f->remaining > 0) {          /* next REPEAT iteration */
                CHARGE(0, 1);
                f->remaining--;
                f->pc = f->body;
                continue;
            }
            if (f->is_call) vm->depth--;
            vm->sp--;
            continue;
        }
        n = &vm->nodes[f->pc];
        f->pc = n->next;
        CHARGE(n, 1);
        TRY(exec_node(vm, n, d));
    }
    return GLYPH_OK;
}

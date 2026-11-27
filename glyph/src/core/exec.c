/* exec.c — the bytecode virtual machine.
 *
 * A stack machine with three explicit stacks, all fixed-size arrays in the
 * vm: values (expression temporaries), locals (inputs, LOCALs and REPEAT
 * counters of every active call) and frames (one per active call). There is
 * no C recursion, so host stack use is constant however deep LOGO recursion
 * goes; each stack fails with E_DEPTH when full.
 *
 * Every entry (start, or one input) gets a fresh step budget. Statements,
 * calls and loop iterations cost one step; drawn cells cost one step each.
 * Expression instructions are free but each statement's expression is
 * bounded by the program text, and every cycle in the control flow passes a
 * charged instruction, so the budget bounds total work.
 */
#include "glyph_internal.h"

static int32_t sat(int64_t v)
{
    if (v > GLYPH_INT_MAX) return GLYPH_INT_MAX;
    if (v < -GLYPH_INT_MAX) return -GLYPH_INT_MAX;
    return (int32_t)v;
}

static int val_equal(const glyph_value *a, const glyph_value *b)
{
    int i;
    if (a->type != b->type) return 0;
    if (a->type == GLYPH_T_INT) return a->i == b->i;
    if (a->len != b->len) return 0;
    for (i = 0; i < a->len; i++)
        if (a->s[i] != b->s[i]) return 0;
    return 1;
}

int state_set(glyph_vm *vm, int slot, const glyph_value *v)
{
    glyph_state *st = &vm->state[slot];
    if (!val_equal(&st->v, v)) st->dirty = 1;
    val_copy(&st->v, v);
    return GLYPH_OK;
}

/* ---- errors and stacks -------------------------------------------------------- */
static int rt_err(glyph_diag *d, const glyph_insn *in, int code, const char *a, const char *b, const char *c)
{
    g_diag(d, code, in ? in->line : 0, in ? in->col : 0, a, b, c);
    return code;
}

static int range_err(glyph_diag *d, const glyph_insn *in, int32_t v, int32_t lo, int32_t hi)
{
    glyph_buf b;
    d->code = GLYPH_E_RANGE; d->line = in->line; d->col = in->col;
    glyph_buf_init(&b, d->msg, sizeof d->msg);
    glyph_buf_str(&b, glyph_op_name(in->op));
    glyph_buf_str(&b, " input ");
    glyph_buf_int(&b, v);
    glyph_buf_str(&b, " out of range ");
    glyph_buf_int(&b, lo);
    glyph_buf_str(&b, "..");
    glyph_buf_int(&b, hi);
    return GLYPH_E_RANGE;
}

static int push(glyph_vm *vm, const glyph_insn *in, glyph_value **out, glyph_diag *d)
{
    if (vm->vsp >= GLYPH_MAX_VSTACK)
        return rt_err(d, in, GLYPH_E_DEPTH, "expression stack exhausted (128 values)", 0, 0);
    *out = &vm->vstack[vm->vsp++];
    return GLYPH_OK;
}

static int push_int(glyph_vm *vm, const glyph_insn *in, int32_t i, glyph_diag *d)
{
    glyph_value *v;
    int e = push(vm, in, &v, d);
    if (e) return e;
    val_int(v, i);
    return GLYPH_OK;
}

/* Pop n values; on return vals points at the first (deepest) of them. The
 * slots stay intact until the next push. */
static int pop_n(glyph_vm *vm, const glyph_insn *in, int n, glyph_value **vals, glyph_diag *d)
{
    if (vm->vsp < n) return rt_err(d, in, GLYPH_E_ARG, "internal: value stack underflow", 0, 0);
    vm->vsp = (uint16_t)(vm->vsp - n);
    *vals = &vm->vstack[vm->vsp];
    return GLYPH_OK;
}

static int want_int(const glyph_value *v, const glyph_insn *in, glyph_diag *d)
{
    glyph_buf b;
    if (v->type == GLYPH_T_INT) return GLYPH_OK;
    d->code = GLYPH_E_TYPE; d->line = in->line; d->col = in->col;
    glyph_buf_init(&b, d->msg, sizeof d->msg);
    glyph_buf_str(&b, glyph_op_name(in->op));
    glyph_buf_str(&b, " expects a number but got the string \"");
    glyph_buf_mem(&b, v->s, v->len > 24 ? 24 : v->len);
    glyph_buf_str(&b, v->len > 24 ? "...\"" : "\"");
    return GLYPH_E_TYPE;
}

/* Pop n integers into out[0..n-1] (in source order), checking [lo, hi]. */
static int pop_ints(glyph_vm *vm, const glyph_insn *in, int n, int32_t *out, int32_t lo, int32_t hi, glyph_diag *d)
{
    glyph_value *v;
    int i, e = pop_n(vm, in, n, &v, d);
    if (e) return e;
    for (i = 0; i < n; i++) {
        if ((e = want_int(&v[i], in, d))) return e;
        out[i] = v[i].i;
        if (out[i] < lo || out[i] > hi) return range_err(d, in, out[i], lo, hi);
    }
    return GLYPH_OK;
}

static int charge(glyph_vm *vm, const glyph_insn *in, uint32_t cost, glyph_diag *d)
{
    if (cost > vm->budget - vm->steps) {
        vm->steps = vm->budget;
        g_diag_num(d, GLYPH_E_BUDGET, in ? in->line : 0, in ? in->col : 0,
                   "step budget of ", (long long)vm->budget, " exhausted");
        return GLYPH_E_BUDGET;
    }
    vm->steps += cost;
    return GLYPH_OK;
}

/* ---- calls --------------------------------------------------------------------- */
static const char *unit_name(const glyph_vm *vm, int unit)
{
    uint16_t s = vm->units[unit].sym;
    return s == 0xFFFF ? "main" : vm->syms[s].name;
}

/* Enter a unit: its inputs are the top nparams values. */
static int enter(glyph_vm *vm, const glyph_insn *in, int unit, int want, uint16_t ret_pc, glyph_diag *d)
{
    const glyph_unit *u = &vm->units[unit];
    glyph_frame *f;
    int i;
    if (vm->fp > GLYPH_MAX_CALL_DEPTH)
        return rt_err(d, in, GLYPH_E_DEPTH, "procedure calls nested deeper than 64 (", unit_name(vm, unit), ")");
    if (vm->lsp + u->nlocals > GLYPH_MAX_LOCALS)
        return rt_err(d, in, GLYPH_E_DEPTH, "out of local variable space (", unit_name(vm, unit), ")");
    if (vm->vsp < u->nparams) return rt_err(d, in, GLYPH_E_ARG, "internal: missing inputs", 0, 0);
    vm->vsp = (uint16_t)(vm->vsp - u->nparams);
    for (i = 0; i < u->nparams; i++) val_copy(&vm->locals[vm->lsp + i], &vm->vstack[vm->vsp + i]);
    for (; i < u->nlocals; i++) vm->locals[vm->lsp + i].type = GLYPH_T_UNSET;
    f = &vm->frames[vm->fp++];
    f->ret_pc = ret_pc; f->unit = (uint16_t)unit; f->base = vm->lsp; f->want = (uint8_t)want;
    vm->lsp = (uint16_t)(vm->lsp + u->nlocals);
    return GLYPH_OK;
}

/* Leave the current unit, optionally with a value. *done is set when the
 * entry's base frame returns. */
static int leave(glyph_vm *vm, const glyph_insn *in, const glyph_value *out, uint16_t *pc, int *done, glyph_diag *d)
{
    const glyph_frame *f = &vm->frames[vm->fp - 1];
    glyph_value tmp, *slot;
    int want = f->want, e;
    if (want && !out)
        return rt_err(d, in, GLYPH_E_TYPE, unit_name(vm, f->unit), " did not OUTPUT a value", 0);
    if (out) val_copy(&tmp, out);
    vm->lsp = f->base;
    *pc = f->ret_pc;
    vm->fp--;
    if (vm->fp == 0) { *done = 1; return GLYPH_OK; }
    if (want) {
        if ((e = push(vm, in, &slot, d))) return e;
        val_copy(slot, &tmp);
    }
    return GLYPH_OK;
}

/* ---- the interpreter loop ------------------------------------------------------- */
#define TRY(x) do { int e_ = (x); if (e_) return e_; } while (0)
#define CHARGE(c) TRY(charge(vm, in, (c), d))
#define AVAIL() (vm->budget - vm->steps)
#define LOCAL(arg) (&vm->locals[vm->frames[vm->fp - 1].base + ((arg) & 0xFF)])

static int store_local(glyph_vm *vm, const glyph_insn *in, glyph_diag *d)
{
    glyph_value *v;
    TRY(pop_n(vm, in, 1, &v, d));
    val_copy(LOCAL(in->arg), v);
    return GLYPH_OK;
}

static int add_to(glyph_vm *vm, const glyph_insn *in, glyph_value *target, const char *name, int global_slot, glyph_diag *d)
{
    int32_t delta;
    glyph_value nv;
    TRY(pop_ints(vm, in, 1, &delta, -GLYPH_INT_MAX, GLYPH_INT_MAX, d));
    if (target->type == GLYPH_T_UNSET) return rt_err(d, in, GLYPH_E_UNSET, "ADD to ", name, " before it is SET");
    if (target->type != GLYPH_T_INT) return rt_err(d, in, GLYPH_E_TYPE, "ADD to ", name, " which holds a string");
    val_int(&nv, sat((int64_t)target->i + delta));
    if (global_slot >= 0) return state_set(vm, global_slot, &nv);
    val_copy(target, &nv);
    return GLYPH_OK;
}

static int binary_int(glyph_vm *vm, const glyph_insn *in, glyph_diag *d)
{
    glyph_value *v;
    int32_t a, b, r = 0;
    TRY(pop_n(vm, in, 2, &v, d));
    TRY(want_int(&v[0], in, d));
    TRY(want_int(&v[1], in, d));
    a = v[0].i; b = v[1].i;
    switch (in->op) {
    case OP_ADD: r = sat((int64_t)a + b); break;
    case OP_SUB: r = sat((int64_t)a - b); break;
    case OP_MUL: r = sat((int64_t)a * b); break;
    case OP_DIV:
    case OP_REM:
        if (b == 0) return rt_err(d, in, GLYPH_E_RANGE, "division by zero", 0, 0);
        r = in->op == OP_DIV ? a / b : a % b;
        break;
    case OP_LESS: r = a < b; break;
    case OP_GREATER: r = a > b; break;
    case OP_LESSEQ: r = a <= b; break;
    case OP_GREATEREQ: r = a >= b; break;
    case OP_AND: r = (a != 0) && (b != 0); break;
    case OP_OR:  r = (a != 0) || (b != 0); break;
    }
    return push_int(vm, in, r, d);
}

static int word(glyph_vm *vm, const glyph_insn *in, glyph_diag *d)
{
    glyph_value *v, *out, tmp;
    char buf[2 * (GLYPH_MAX_STR_LEN + 1)];
    int i, n = 0;
    TRY(pop_n(vm, in, 2, &v, d));
    for (i = 0; i < 2; i++) {
        if (v[i].type == GLYPH_T_INT) n += format_int(buf + n, v[i].i);
        else { g_copy(buf + n, v[i].s, v[i].len); n += v[i].len; }
    }
    if (n > GLYPH_MAX_STR_LEN) return rt_err(d, in, GLYPH_E_RANGE, "WORD result longer than 63 characters", 0, 0);
    val_str(&tmp, buf, n);
    TRY(push(vm, in, &out, d));
    val_copy(out, &tmp);
    return GLYPH_OK;
}

static int array_index(glyph_vm *vm, const glyph_insn *in, int32_t idx, int *cell, glyph_diag *d)
{
    const glyph_array *a = &vm->arrays[in->arg];
    if (idx < 1 || idx > a->size) return range_err(d, in, idx, 1, a->size);
    *cell = a->base + idx - 1;
    return GLYPH_OK;
}

int exec_entry(glyph_vm *vm, int unit, glyph_diag *d)
{
    uint16_t pc = vm->units[unit].entry;
    int done = 0;
    vm->steps = 0; vm->vsp = 0; vm->lsp = 0; vm->fp = 0;
    TRY(enter(vm, 0, unit, 0, 0, d));
    for (;;) {
        const glyph_insn *in;
        glyph_value *v;
        int32_t a[4];
        uint32_t cost;
        int cell = 0;
        if (pc == 0 || pc >= vm->n_code) return rt_err(d, 0, GLYPH_E_ARG, "internal: bad instruction address", 0, 0);
        in = &vm->code[pc++];
        switch (in->op) {
        /* ---- expressions ---- */
        case OP_PUSH_INT: TRY(push_int(vm, in, in->arg, d)); break;
        case OP_PUSH_STR:
            TRY(push(vm, in, &v, d));
            val_str(v, &vm->strpool[STR_OFF(in->arg)], (int)STR_LEN(in->arg));
            break;
        case OP_LOAD_G: {
            const glyph_state *st = &vm->state[in->arg];
            if (st->v.type == GLYPH_T_UNSET)
                return rt_err(d, in, GLYPH_E_UNSET, "variable ", vm->syms[st->sym].name, " read before SET");
            TRY(push(vm, in, &v, d));
            val_copy(v, &st->v);
            break;
        }
        case OP_LOAD_L: {
            glyph_value src;
            if (LOCAL(in->arg)->type == GLYPH_T_UNSET)
                return rt_err(d, in, GLYPH_E_UNSET, "variable ", vm->syms[in->arg >> 8].name, " read before SET");
            val_copy(&src, LOCAL(in->arg));
            TRY(push(vm, in, &v, d));
            val_copy(v, &src);
            break;
        }
        case OP_ITEM:
            TRY(pop_ints(vm, in, 1, a, -GLYPH_INT_MAX, GLYPH_INT_MAX, d));
            TRY(array_index(vm, in, a[0], &cell, d));
            TRY(push_int(vm, in, vm->cells[cell], d));
            break;
        case OP_NEG:
            TRY(pop_ints(vm, in, 1, a, -GLYPH_INT_MAX, GLYPH_INT_MAX, d));
            TRY(push_int(vm, in, -a[0], d));
            break;
        case OP_NOT:
            TRY(pop_ints(vm, in, 1, a, -GLYPH_INT_MAX, GLYPH_INT_MAX, d));
            TRY(push_int(vm, in, a[0] == 0, d));
            break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM:
        case OP_LESS: case OP_GREATER: case OP_LESSEQ: case OP_GREATEREQ: case OP_AND: case OP_OR:
            TRY(binary_int(vm, in, d));
            break;
        case OP_EQ: case OP_NE: {
            int eq;
            TRY(pop_n(vm, in, 2, &v, d));
            eq = val_equal(&v[0], &v[1]);
            TRY(push_int(vm, in, in->op == OP_EQ ? eq : !eq, d));
            break;
        }
        case OP_WORD: TRY(word(vm, in, d)); break;
        case OP_XCOR: TRY(push_int(vm, in, canvas_round(vm->tx), d)); break;
        case OP_YCOR: TRY(push_int(vm, in, canvas_round(vm->ty), d)); break;
        case OP_HEADING: TRY(push_int(vm, in, vm->heading, d)); break;
        case OP_CALL:
        case OP_CALLV:
            CHARGE(1);
            TRY(enter(vm, in, vm->syms[in->arg].proc_unit, in->op == OP_CALLV, pc, d));
            pc = vm->units[vm->syms[in->arg].proc_unit].entry;
            break;
        /* ---- control ---- */
        case OP_JMP: pc = (uint16_t)in->arg; break;
        case OP_RET:
            TRY(leave(vm, in, 0, &pc, &done, d));
            if (done) return GLYPH_OK;
            break;
        case OP_STOP:
            CHARGE(1);
            TRY(leave(vm, in, 0, &pc, &done, d));
            if (done) return GLYPH_OK;
            break;
        case OP_OUTPUT:
            CHARGE(1);
            TRY(pop_n(vm, in, 1, &v, d));
            TRY(leave(vm, in, v, &pc, &done, d));
            if (done) return GLYPH_OK;
            break;
        case OP_IF_JZ:
        case OP_WHILE_JZ:
            CHARGE(1);
            TRY(pop_ints(vm, in, 1, a, -GLYPH_INT_MAX, GLYPH_INT_MAX, d));
            if (a[0] == 0) pc = (uint16_t)in->arg;
            break;
        case OP_REP_INIT:
            CHARGE(1);
            TRY(pop_ints(vm, in, 1, a, 0, GLYPH_MAX_REPEAT, d));
            val_int(LOCAL(in->arg), a[0]);
            break;
        case OP_REP_NEXT: {
            glyph_value *c = LOCAL(in->arg >> 16);
            if (c->i <= 0) { pc = (uint16_t)(in->arg & 0xFFFF); break; }
            CHARGE(1);
            c->i--;
            break;
        }
        /* ---- statements ---- */
        case OP_BLOCK:
        case OP_LOCAL:
        case OP_PU:
        case OP_PD:
            CHARGE(1);
            if (in->op == OP_PU) vm->pen = 0;
            if (in->op == OP_PD) vm->pen = 1;
            break;
        case OP_FD:
        case OP_BK:
            CHARGE(1);
            TRY(pop_ints(vm, in, 1, a, -GLYPH_MAX_DIST, GLYPH_MAX_DIST, d));
            cost = turtle_move(vm, in->op == OP_BK ? -a[0] : a[0], AVAIL());
            CHARGE(cost);
            break;
        case OP_RT:
        case OP_LT:
        case OP_SETH: {
            int32_t t;
            CHARGE(1);
            TRY(pop_ints(vm, in, 1, a, -GLYPH_INT_MAX, GLYPH_INT_MAX, d));
            t = a[0] % 360;
            if (in->op == OP_LT) t = -t;
            vm->heading = ((in->op == OP_SETH ? 0 : vm->heading) + t + 360) % 360;
            break;
        }
        case OP_HOME:
            CHARGE(1);
            cost = turtle_goto(vm, vm->w / 2, vm->h / 2, AVAIL());
            CHARGE(cost);
            vm->heading = 0;
            break;
        case OP_SETXY:
            CHARGE(1);
            TRY(pop_ints(vm, in, 2, a, -GLYPH_MAX_COORD, GLYPH_MAX_COORD, d));
            cost = turtle_goto(vm, a[0], a[1], AVAIL());
            CHARGE(cost);
            break;
        case OP_CS:
        case OP_CLEAR:
            CHARGE(1);
            CHARGE((uint32_t)vm->w * vm->h);
            canvas_clear(vm);
            if (in->op == OP_CS) turtle_home(vm);
            break;
        case OP_COLOR: {
            glyph_value *cv;
            CHARGE(1);
            TRY(pop_n(vm, in, 2, &cv, d));
            TRY(want_int(&cv[0], in, d));
            TRY(want_int(&cv[1], in, d));
            if (cv[0].i < 0 || cv[0].i > 15) return range_err(d, in, cv[0].i, 0, 15);
            if (cv[1].i < 0 || cv[1].i > 7) return range_err(d, in, cv[1].i, 0, 7);
            vm->attr = (uint8_t)((cv[1].i << 4) | cv[0].i);
            break;
        }
        case OP_SETCOLOR:
            CHARGE(1);
            TRY(pop_ints(vm, in, 1, a, 0, 15, d));
            vm->attr = (uint8_t)((vm->attr & 0xF0) | a[0]);
            break;
        case OP_TEXT: {
            glyph_value *tv;
            char num[12];
            const char *s;
            int len, i;
            CHARGE(1);
            TRY(pop_n(vm, in, 3, &tv, d));
            for (i = 0; i < 2; i++) {
                TRY(want_int(&tv[i], in, d));
                if (tv[i].i < -GLYPH_MAX_COORD || tv[i].i > GLYPH_MAX_COORD)
                    return range_err(d, in, tv[i].i, -GLYPH_MAX_COORD, GLYPH_MAX_COORD);
            }
            if (tv[2].type == GLYPH_T_INT) { len = format_int(num, tv[2].i); s = num; }
            else { s = tv[2].s; len = tv[2].len; }
            CHARGE((uint32_t)len);
            canvas_text(vm, tv[0].i, tv[1].i, s, len);
            break;
        }
        case OP_LINE:
            CHARGE(1);
            TRY(pop_ints(vm, in, 4, a, -GLYPH_MAX_COORD, GLYPH_MAX_COORD, d));
            cost = canvas_line(vm, a[0], a[1], a[2], a[3], AVAIL());
            CHARGE(cost);
            break;
        case OP_RECT:
        case OP_BOX: {
            glyph_value *rv;
            int32_t minsz = in->op == OP_BOX ? 2 : 0;
            int i;
            CHARGE(1);
            TRY(pop_n(vm, in, 4, &rv, d));
            for (i = 0; i < 4; i++) {
                int32_t lo = i < 2 ? -GLYPH_MAX_COORD : minsz;
                TRY(want_int(&rv[i], in, d));
                if (rv[i].i < lo || rv[i].i > GLYPH_MAX_COORD) return range_err(d, in, rv[i].i, lo, GLYPH_MAX_COORD);
                a[i] = rv[i].i;
            }
            cost = in->op == OP_RECT ? canvas_rect(vm, a[0], a[1], a[2], a[3], AVAIL())
                                     : canvas_box(vm, a[0], a[1], a[2], a[3], AVAIL());
            CHARGE(cost);
            break;
        }
        case OP_STORE_G:
            CHARGE(1);
            TRY(pop_n(vm, in, 1, &v, d));
            state_set(vm, in->arg, v);
            break;
        case OP_STORE_L:
            CHARGE(1);
            TRY(store_local(vm, in, d));
            break;
        case OP_ADDV_G: {
            glyph_state *st = &vm->state[in->arg];
            CHARGE(1);
            TRY(add_to(vm, in, &st->v, vm->syms[st->sym].name, in->arg, d));
            break;
        }
        case OP_ADDV_L:
            CHARGE(1);
            TRY(add_to(vm, in, LOCAL(in->arg), vm->syms[in->arg >> 8].name, -1, d));
            break;
        case OP_SETITEM: {
            glyph_value *sv;
            CHARGE(1);
            TRY(pop_n(vm, in, 2, &sv, d));
            TRY(want_int(&sv[0], in, d));
            TRY(want_int(&sv[1], in, d));
            TRY(array_index(vm, in, sv[0].i, &cell, d));
            vm->cells[cell] = sv[1].i;
            break;
        }
        case OP_ENABLE:
        case OP_DISABLE: {
            int c = vm->syms[in->arg].comp;
            CHARGE(1);
            vm->comps[c].enabled = (uint8_t)(in->op == OP_ENABLE);
            if (in->op == OP_DISABLE) ui_on_disable(vm, c);
            break;
        }
        default:
            return rt_err(d, in, GLYPH_E_ARG, "internal: invalid instruction", 0, 0);
        }
    }
}

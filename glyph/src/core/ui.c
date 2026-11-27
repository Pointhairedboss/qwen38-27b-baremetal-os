/* ui.c — session lifecycle, component model, focus, event dispatch and
 * frame composition. This is the public face of the core (glyph.h). */
#include "glyph_internal.h"

/* ---- lifecycle ------------------------------------------------------------ */
size_t glyph_vm_size(void) { return sizeof(glyph_vm); }

void glyph_init(glyph_vm *vm)
{
    if (!vm) return;
    g_zero(vm, sizeof *vm);
    vm->phase = GLYPH_PHASE_EMPTY;
    vm->w = GLYPH_DEFAULT_W;
    vm->h = GLYPH_DEFAULT_H;
    vm->n_code = 1;                        /* instruction 0 means "none" */
    vm->focus = -1;
    vm->budget = GLYPH_DEFAULT_STEPS;
    canvas_reset(vm);
    turtle_home(vm);
}

void glyph_set_budget(glyph_vm *vm, uint32_t steps)
{
    if (!vm) return;
    vm->budget = steps == 0 ? GLYPH_DEFAULT_STEPS : steps > GLYPH_MAX_STEPS ? GLYPH_MAX_STEPS : steps;
}

int glyph_load(glyph_vm *vm, const char *src, size_t len, glyph_diag *d)
{
    uint32_t budget;
    int e;
    if (!vm) return GLYPH_E_ARG;
    if (!d) d = &vm->diag;
    budget = vm->budget ? vm->budget : GLYPH_DEFAULT_STEPS;
    glyph_init(vm);
    vm->budget = budget;
    g_diag(d, GLYPH_OK, 0, 0, 0, 0, 0);
    if (!src && len) {
        g_diag(d, GLYPH_E_ARG, 0, 0, "no source", 0, 0);
        return GLYPH_E_ARG;
    }
    if (len > GLYPH_MAX_SOURCE) {
        g_diag_num(d, GLYPH_E_SOURCE_SIZE, 0, 0, "program larger than ", GLYPH_MAX_SOURCE, " bytes");
        return GLYPH_E_SOURCE_SIZE;
    }
    lex_begin(&vm->lex, src ? src : "", (uint32_t)len);
    e = parse_program(vm, d);
    vm->lex.src = 0;                       /* never keep the caller's pointer */
    if (e) {
        glyph_diag keep;
        g_copy_diag(&keep, d);
        glyph_init(vm);
        vm->budget = budget;
        g_copy_diag(d, &keep);
        g_copy_diag(&vm->diag, &keep);
        return e;
    }
    canvas_reset(vm);
    turtle_home(vm);
    vm->phase = GLYPH_PHASE_LOADED;
    return GLYPH_OK;
}

/* ---- focus ---------------------------------------------------------------- */
static int focusable(const glyph_vm *vm, int i)
{
    const glyph_comp *c = &vm->comps[i];
    return c->enabled && (c->type == GLYPH_COMP_BUTTON || c->type == GLYPH_COMP_FIELD);
}

static int next_focusable(const glyph_vm *vm, int from)
{
    int k, n = vm->n_comps;
    for (k = 1; k <= n; k++) {
        int j = (from < 0) ? k - 1 : (from + k) % n;
        if (focusable(vm, j)) return j;
    }
    return -1;
}

void ui_focus_first(glyph_vm *vm) { vm->focus = (int8_t)next_focusable(vm, -1); }

void ui_on_disable(glyph_vm *vm, int comp)
{
    if (vm->focus == comp) vm->focus = (int8_t)next_focusable(vm, comp);
}

/* ---- results -------------------------------------------------------------- */
static void clear_dirty(glyph_vm *vm)
{
    int i;
    for (i = 0; i < vm->n_state; i++) vm->state[i].dirty = 0;
}

static int count_dirty(const glyph_vm *vm)
{
    int i, n = 0;
    for (i = 0; i < vm->n_state; i++) n += vm->state[i].dirty ? 1 : 0;
    return n;
}

static int finish(glyph_vm *vm, glyph_result *r, int status)
{
    if (r) {
        r->status = status;
        r->focus = vm->focus;
        r->changed = count_dirty(vm);
        if (status != GLYPH_OK && status != GLYPH_IGNORED && r->diag.code == GLYPH_OK)
            g_diag(&r->diag, status, 0, 0, 0, 0, 0);
    }
    return status;
}

static int input_begin(glyph_vm *vm, glyph_result *r)
{
    g_result_reset(vm, r);
    if (!vm) return r->status = GLYPH_E_ARG;
    clear_dirty(vm);
    if (vm->phase == GLYPH_PHASE_FAULTED) {
        g_diag(&r->diag, GLYPH_E_FAULTED, 0, 0, "session faulted during start", 0, 0);
        return r->status = GLYPH_E_FAULTED;
    }
    if (vm->phase != GLYPH_PHASE_RUNNING) {
        g_diag(&r->diag, GLYPH_E_STATE, 0, 0, "program is not started", 0, 0);
        return r->status = GLYPH_E_STATE;
    }
    return GLYPH_OK;
}

int glyph_start(glyph_vm *vm, glyph_result *r)
{
    glyph_result local;
    int e;
    if (!r) r = &local;
    g_result_reset(vm, r);
    if (!vm) return GLYPH_E_ARG;
    if (vm->phase != GLYPH_PHASE_LOADED) {
        g_diag(&r->diag, GLYPH_E_STATE, 0, 0, "start needs a freshly loaded program", 0, 0);
        return finish(vm, r, GLYPH_E_STATE);
    }
    clear_dirty(vm);
    e = exec_entry(vm, 0, &r->diag);
    r->steps = vm->steps;
    if (e) {
        vm->phase = GLYPH_PHASE_FAULTED;
        vm->focus = -1;
        g_copy_diag(&vm->diag, &r->diag);
        return finish(vm, r, e);
    }
    vm->phase = GLYPH_PHASE_RUNNING;
    ui_focus_first(vm);
    return finish(vm, r, GLYPH_OK);
}

/* Run the handler for action symbol `sym`. */
static int fire(glyph_vm *vm, int sym, glyph_result *r)
{
    int e;
    if (!(vm->syms[sym].flags & GLYPH_SYM_HANDLER)) return finish(vm, r, GLYPH_IGNORED);
    r->action = sym;
    e = exec_entry(vm, vm->syms[sym].handler_unit, &r->diag);
    r->steps = vm->steps;
    return finish(vm, r, e);
}

/* ---- fields ------------------------------------------------------------------- */
/* Current field text into buf (>= 64 bytes); returns length. */
static int field_text(const glyph_vm *vm, const glyph_comp *c, char *buf)
{
    const glyph_value *v = &vm->state[vm->syms[c->sym].state].v;
    int i;
    if (v->type == GLYPH_T_INT) return format_int(buf, v->i);
    if (v->type != GLYPH_T_STR) { buf[0] = 0; return 0; }
    for (i = 0; i < v->len; i++) buf[i] = v->s[i];
    buf[v->len] = 0;
    return v->len;
}

static int field_edit(glyph_vm *vm, const glyph_comp *c, int key)
{
    char buf[GLYPH_MAX_STR_LEN + 2];
    int len = field_text(vm, c, buf);
    int width = c->w - 2;
    int slot = vm->syms[c->sym].state;
    if (key == GLYPH_KEY_BS || key == GLYPH_KEY_DEL) {
        if (len == 0) return GLYPH_IGNORED;
        len--;
    } else {
        if (len >= width || len >= GLYPH_MAX_STR_LEN) return GLYPH_IGNORED;
        buf[len++] = (char)key;
    }
    {
        glyph_value nv;
        val_str(&nv, buf, len);
        state_set(vm, slot, &nv);
    }
    return GLYPH_OK;
}

/* ---- inputs ------------------------------------------------------------------- */
int glyph_key(glyph_vm *vm, int key, glyph_result *r)
{
    glyph_result local;
    const glyph_comp *c;
    int e;
    if (!r) r = &local;
    e = input_begin(vm, r);
    if (e) return e;
    c = vm->focus >= 0 ? &vm->comps[vm->focus] : 0;

    if (key == GLYPH_KEY_TAB) {
        int nf = next_focusable(vm, vm->focus);
        if (nf < 0) return finish(vm, r, GLYPH_IGNORED);
        vm->focus = (int8_t)nf;
        return finish(vm, r, GLYPH_OK);
    }
    if (!c) return finish(vm, r, GLYPH_IGNORED);
    if (key == GLYPH_KEY_LF || key == GLYPH_KEY_CR)
        return fire(vm, c->action, r);
    if (c->type == GLYPH_COMP_BUTTON) {
        if (key == GLYPH_KEY_SPACE) return fire(vm, c->action, r);
        return finish(vm, r, GLYPH_IGNORED);
    }
    /* focused field */
    if (key == GLYPH_KEY_BS || key == GLYPH_KEY_DEL || (key >= 0x20 && key <= 0x7E))
        return finish(vm, r, field_edit(vm, c, key));
    return finish(vm, r, GLYPH_IGNORED);
}

/* Look up a symbol by name given in any case. */
static int find_name(const glyph_vm *vm, const char *name)
{
    char up[GLYPH_MAX_NAME_LEN + 1];
    int i;
    if (!name) return -1;
    for (i = 0; name[i]; i++) {
        char ch = name[i];
        if (i >= GLYPH_MAX_NAME_LEN) return -1;
        up[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    up[i] = 0;
    return sym_find(vm, up);
}

int glyph_click(glyph_vm *vm, const char *id, glyph_result *r)
{
    glyph_result local;
    int s, ci, e;
    if (!r) r = &local;
    e = input_begin(vm, r);
    if (e) return e;
    s = find_name(vm, id);
    if (s < 0 || vm->syms[s].comp < 0) return finish(vm, r, GLYPH_IGNORED);
    ci = vm->syms[s].comp;
    if (!focusable(vm, ci)) return finish(vm, r, GLYPH_IGNORED);
    vm->focus = (int8_t)ci;
    if (vm->comps[ci].type == GLYPH_COMP_BUTTON) return fire(vm, vm->comps[ci].action, r);
    return finish(vm, r, GLYPH_OK);
}

int glyph_action(glyph_vm *vm, const char *name, glyph_result *r)
{
    glyph_result local;
    int s, e;
    if (!r) r = &local;
    e = input_begin(vm, r);
    if (e) return e;
    s = find_name(vm, name);
    if (s < 0) return finish(vm, r, GLYPH_IGNORED);
    return fire(vm, s, r);
}

/* ---- rendering ----------------------------------------------------------------- */
static void row_put(glyph_vm *vm, int x, char ch, uint8_t attr)
{
    if (x >= 0 && x < vm->w) vm->row[x] = GLYPH_CELL(ch, attr);
}

static void row_text(glyph_vm *vm, int x, const char *s, int n, uint8_t attr)
{
    int i;
    for (i = 0; i < n; i++) row_put(vm, x + i, s[i], attr);
}

static void draw_comp_row(glyph_vm *vm, int ci, int y)
{
    const glyph_comp *c = &vm->comps[ci];
    char buf[GLYPH_MAX_STR_LEN + 2];
    int i;
    if (c->type == GLYPH_COMP_PANEL) {
        int x2 = c->x + c->w - 1, y2 = c->y + c->h - 1;
        if (y < c->y || y > y2) return;
        if (y == c->y || y == y2) {
            for (i = c->x + 1; i < x2; i++) row_put(vm, i, '-', GLYPH_ATTR_PANEL);
            row_put(vm, c->x, '+', GLYPH_ATTR_PANEL);
            row_put(vm, x2, '+', GLYPH_ATTR_PANEL);
            if (y == c->y && c->text_len > 0) {
                int room = c->w - 4, n = c->text_len;
                if (room > 0) {
                    if (n > room) n = room;
                    row_put(vm, c->x + 1, ' ', GLYPH_ATTR_TITLE);
                    row_text(vm, c->x + 2, &vm->strpool[c->text], n, GLYPH_ATTR_TITLE);
                    row_put(vm, c->x + 2 + n, ' ', GLYPH_ATTR_TITLE);
                }
            }
        } else {
            row_put(vm, c->x, '|', GLYPH_ATTR_PANEL);
            row_put(vm, x2, '|', GLYPH_ATTR_PANEL);
        }
        return;
    }
    if (y != c->y) return;
    if (c->type == GLYPH_COMP_LABEL) {
        int n;
        if (c->bind_kind == K_INT) n = format_int(buf, c->bind);
        else if (c->bind_kind == K_STR) {
            n = (int)STR_LEN(c->bind);
            row_text(vm, c->x, &vm->strpool[STR_OFF(c->bind)], n, GLYPH_ATTR_LABEL);
            return;
        } else {
            const glyph_value *v = &vm->state[vm->syms[c->bind].state].v;
            if (v->type == GLYPH_T_INT) n = format_int(buf, v->i);
            else if (v->type == GLYPH_T_STR) { row_text(vm, c->x, v->s, v->len, GLYPH_ATTR_LABEL); return; }
            else n = 0;
        }
        row_text(vm, c->x, buf, n, GLYPH_ATTR_LABEL);
        return;
    }
    {
        char open = '[', close = ']';
        uint8_t attr = GLYPH_ATTR_CONTROL;
        int x = c->x;
        if (!c->enabled) { open = '('; close = ')'; attr = GLYPH_ATTR_DISABLED; }
        else if (vm->focus == ci) { open = '>'; close = '<'; attr = GLYPH_ATTR_FOCUS; }
        row_put(vm, x++, open, attr);
        if (c->type == GLYPH_COMP_BUTTON) {
            row_put(vm, x++, ' ', attr);
            row_text(vm, x, &vm->strpool[c->text], c->text_len, attr);
            x += c->text_len;
            row_put(vm, x++, ' ', attr);
        } else {
            int width = c->w - 2, n = field_text(vm, c, buf);
            if (n > width) n = width;
            row_text(vm, x, buf, n, attr);
            for (i = n; i < width; i++) row_put(vm, x + i, '_', attr);
            x += width;
        }
        row_put(vm, x, close, attr);
    }
}

void glyph_render(glyph_vm *vm, glyph_row_fn fn, void *user)
{
    int x, y, i;
    if (!vm || !fn) return;
    for (y = 0; y < vm->h; y++) {
        for (x = 0; x < vm->w; x++) vm->row[x] = vm->canvas[y * GLYPH_MAX_W + x];
        for (i = 0; i < vm->n_comps; i++) draw_comp_row(vm, i, y);
        fn(user, y, vm->row, vm->w);
    }
}

const glyph_diag *glyph_last_diag(const glyph_vm *vm) { return vm ? &vm->diag : 0; }

int glyph_width(const glyph_vm *vm)  { return vm ? vm->w : 0; }
int glyph_height(const glyph_vm *vm) { return vm ? vm->h : 0; }

/* ---- introspection ------------------------------------------------------------- */
int glyph_state_count(const glyph_vm *vm) { return vm ? vm->n_state : 0; }

static int state_ok(const glyph_vm *vm, int i) { return vm && i >= 0 && i < vm->n_state; }

const char *glyph_state_name(const glyph_vm *vm, int i)
{
    return state_ok(vm, i) ? vm->syms[vm->state[i].sym].name : "";
}
int glyph_state_type(const glyph_vm *vm, int i) { return state_ok(vm, i) ? vm->state[i].v.type : GLYPH_T_UNSET; }
int32_t glyph_state_int(const glyph_vm *vm, int i) { return state_ok(vm, i) ? vm->state[i].v.i : 0; }
const char *glyph_state_str(const glyph_vm *vm, int i) { return state_ok(vm, i) ? vm->state[i].v.s : ""; }
int glyph_state_dirty(const glyph_vm *vm, int i) { return state_ok(vm, i) ? vm->state[i].dirty : 0; }

int glyph_state_find(const glyph_vm *vm, const char *name)
{
    int s = vm ? find_name(vm, name) : -1;
    return s < 0 ? -1 : vm->syms[s].state;
}

int glyph_array_count(const glyph_vm *vm) { return vm ? vm->n_arrays : 0; }
static int array_ok(const glyph_vm *vm, int i) { return vm && i >= 0 && i < vm->n_arrays; }
const char *glyph_array_name(const glyph_vm *vm, int i)
{
    return array_ok(vm, i) ? vm->syms[vm->arrays[i].sym].name : "";
}
int glyph_array_size(const glyph_vm *vm, int i) { return array_ok(vm, i) ? vm->arrays[i].size : 0; }
int32_t glyph_array_item(const glyph_vm *vm, int i, int index)
{
    if (!array_ok(vm, i) || index < 1 || index > vm->arrays[i].size) return 0;
    return vm->cells[vm->arrays[i].base + index - 1];
}
int glyph_array_find(const glyph_vm *vm, const char *name)
{
    int s = vm ? find_name(vm, name) : -1;
    return s < 0 ? -1 : vm->syms[s].array;
}

int glyph_comp_count(const glyph_vm *vm) { return vm ? vm->n_comps : 0; }
const char *glyph_comp_name(const glyph_vm *vm, int i)
{
    return (vm && i >= 0 && i < vm->n_comps) ? vm->syms[vm->comps[i].sym].name : "";
}
int glyph_comp_enabled(const glyph_vm *vm, int i)
{
    return (vm && i >= 0 && i < vm->n_comps) ? vm->comps[i].enabled : 0;
}
int glyph_focus(const glyph_vm *vm) { return vm ? vm->focus : -1; }

const char *glyph_sym_name(const glyph_vm *vm, int sym)
{
    return (vm && sym >= 0 && sym < vm->n_syms) ? vm->syms[sym].name : "";
}

void glyph_turtle(const glyph_vm *vm, int *x, int *y, int *heading, int *pen)
{
    if (!vm) return;
    if (x) *x = canvas_round(vm->tx);
    if (y) *y = canvas_round(vm->ty);
    if (heading) *heading = vm->heading;
    if (pen) *pen = vm->pen;
}

glyph_cell glyph_canvas_cell(const glyph_vm *vm, int x, int y)
{
    if (!vm || x < 0 || y < 0 || x >= vm->w || y >= vm->h) return 0;
    return vm->canvas[y * GLYPH_MAX_W + x];
}

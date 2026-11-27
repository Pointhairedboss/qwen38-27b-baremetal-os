/* script.c — the shared conformance harness.
 *
 * Runs a program against a line-based event script and writes a
 * deterministic transcript. It lives in the core (freestanding) so the Linux
 * host and the bare-metal target produce byte-identical transcripts from the
 * same corpus. Script grammar: docs/SPEC.md appendix A.
 */
#include "glyph_internal.h"

typedef struct span { const char *p; size_t n; } span;

static int sp_is(span s, const char *w)
{
    size_t i;
    for (i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!w[i] || c != w[i]) return 0;
    }
    return w[i] == 0;
}

static span sp_trim(span s)
{
    while (s.n && (s.p[0] == ' ' || s.p[0] == '\t')) { s.p++; s.n--; }
    while (s.n && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t' || s.p[s.n - 1] == '\r')) s.n--;
    return s;
}

/* Split off the first word of *s. */
static span sp_word(span *s)
{
    span w;
    size_t i = 0;
    *s = sp_trim(*s);
    while (i < s->n && s->p[i] != ' ' && s->p[i] != '\t') i++;
    w.p = s->p; w.n = i;
    s->p += i; s->n -= i;
    *s = sp_trim(*s);
    return w;
}

/* Copy a span into a NUL-terminated name buffer; 0 if it does not fit. */
static int sp_name(span s, char *out, size_t cap)
{
    size_t i;
    if (s.n == 0 || s.n >= cap) return 0;
    for (i = 0; i < s.n; i++) out[i] = s.p[i];
    out[s.n] = 0;
    return 1;
}

static void echo(glyph_buf *b, span s)
{
    size_t i;
    for (i = 0; i < s.n; i++) {
        char c = s.p[i];
        glyph_buf_ch(b, (c >= 0x20 && c <= 0x7E) ? c : '?');
    }
}

/* ---- result lines ------------------------------------------------------------- */
typedef struct agg {
    int      status;      /* first error, else OK if any OK, else IGNORED */
    int      any;         /* at least one input */
    int      action;      /* last fired action */
    uint32_t steps;       /* total */
    uint32_t changed;     /* bit per state slot */
    glyph_diag diag;
} agg;

static void agg_init(agg *a)
{
    a->status = GLYPH_IGNORED; a->any = 0; a->action = -1; a->steps = 0; a->changed = 0;
    a->diag.code = GLYPH_OK; a->diag.line = a->diag.col = 0; a->diag.msg[0] = 0;
}

static void agg_add(agg *a, const glyph_vm *vm, const glyph_result *r)
{
    int i;
    a->any = 1;
    if (r->action >= 0) a->action = r->action;
    a->steps += r->steps;
    for (i = 0; i < glyph_state_count(vm) && i < 32; i++)
        if (glyph_state_dirty(vm, i)) a->changed |= (uint32_t)1 << i;
    if (r->status != GLYPH_OK && r->status != GLYPH_IGNORED) {
        if (a->status == GLYPH_OK || a->status == GLYPH_IGNORED) {
            a->status = r->status;
            g_copy_diag(&a->diag, &r->diag);
        }
    } else if (r->status == GLYPH_OK && a->status == GLYPH_IGNORED) {
        a->status = GLYPH_OK;
    }
}

static void write_result(glyph_buf *b, const glyph_vm *vm, const agg *a, int show_steps)
{
    int i, first = 1;
    if (a->status == GLYPH_OK) glyph_buf_str(b, "ok");
    else if (a->status == GLYPH_IGNORED) glyph_buf_str(b, "ignored");
    else glyph_buf_str(b, "error");
    if (a->action >= 0) {
        glyph_buf_str(b, " action=");
        glyph_buf_str(b, glyph_sym_name(vm, a->action));
    }
    if (show_steps || a->action >= 0) {
        glyph_buf_str(b, " steps=");
        glyph_buf_int(b, a->steps);
    }
    for (i = 0; i < glyph_state_count(vm) && i < 32; i++) {
        if (!(a->changed & ((uint32_t)1 << i))) continue;
        glyph_buf_str(b, first ? " changed=" : ",");
        glyph_buf_str(b, glyph_state_name(vm, i));
        first = 0;
    }
    glyph_buf_str(b, " focus=");
    glyph_buf_str(b, glyph_focus(vm) >= 0 ? glyph_comp_name(vm, glyph_focus(vm)) : "-");
    glyph_buf_ch(b, '\n');
    if (a->status != GLYPH_OK && a->status != GLYPH_IGNORED) {
        glyph_buf_str(b, "  ! ");
        glyph_format_diag(b, &a->diag);
        glyph_buf_ch(b, '\n');
    }
}

/* ---- dumps ---------------------------------------------------------------- */
typedef struct dump_ctx { glyph_buf *b; int attrs; } dump_ctx;

static void dump_row(void *user, int y, const glyph_cell *cells, int w)
{
    dump_ctx *dc = (dump_ctx *)user;
    int x, end = w;
    if (y < 10) glyph_buf_ch(dc->b, ' ');
    glyph_buf_int(dc->b, y);
    glyph_buf_ch(dc->b, '|');
    if (dc->attrs) {
        for (x = 0; x < w; x++) glyph_buf_hex2(dc->b, GLYPH_CELL_ATTR(cells[x]));
    } else {
        while (end > 0 && GLYPH_CELL_CH(cells[end - 1]) == ' ') end--;
        for (x = 0; x < end; x++) {
            unsigned char c = GLYPH_CELL_CH(cells[x]);
            glyph_buf_ch(dc->b, (c >= 0x20 && c <= 0x7E) ? (char)c : '?');
        }
    }
    glyph_buf_ch(dc->b, '\n');
}

static void dump_state(glyph_buf *b, const glyph_vm *vm)
{
    int i;
    for (i = 0; i < glyph_state_count(vm); i++) {
        glyph_buf_str(b, "  ");
        glyph_buf_str(b, glyph_state_name(vm, i));
        glyph_buf_str(b, " = ");
        switch (glyph_state_type(vm, i)) {
        case GLYPH_T_INT: glyph_buf_int(b, glyph_state_int(vm, i)); break;
        case GLYPH_T_STR:
            glyph_buf_ch(b, '"'); glyph_buf_str(b, glyph_state_str(vm, i)); glyph_buf_ch(b, '"');
            break;
        default: glyph_buf_str(b, "<unset>");
        }
        glyph_buf_ch(b, '\n');
    }
    for (i = 0; i < glyph_array_count(vm); i++) {
        int k, n = glyph_array_size(vm, i);
        glyph_buf_str(b, "  ");
        glyph_buf_str(b, glyph_array_name(vm, i));
        glyph_buf_ch(b, '[');
        glyph_buf_int(b, n);
        glyph_buf_str(b, "] =");
        for (k = 1; k <= n; k++) {
            glyph_buf_ch(b, ' ');
            glyph_buf_int(b, glyph_array_item(vm, i, k));
        }
        glyph_buf_ch(b, '\n');
    }
}

/* ---- keys ----------------------------------------------------------------- */
static int parse_key(span s)
{
    size_t i;
    int v = 0;
    if (sp_is(s, "TAB")) return GLYPH_KEY_TAB;
    if (sp_is(s, "ENTER")) return GLYPH_KEY_LF;
    if (sp_is(s, "CR")) return GLYPH_KEY_CR;
    if (sp_is(s, "SPACE")) return GLYPH_KEY_SPACE;
    if (sp_is(s, "BS")) return GLYPH_KEY_BS;
    if (sp_is(s, "DEL")) return GLYPH_KEY_DEL;
    if (sp_is(s, "ESC")) return GLYPH_KEY_ESC;
    if (s.n == 3 && s.p[0] == '\'' && s.p[2] == '\'') return (unsigned char)s.p[1];
    if (s.n == 0 || s.n > 3) return -1;
    for (i = 0; i < s.n; i++) {
        if (s.p[i] < '0' || s.p[i] > '9') return -1;
        v = v * 10 + (s.p[i] - '0');
    }
    return v <= 255 ? v : -1;
}

/* ---- transcript ---------------------------------------------------------------- */
static int script_error(glyph_buf *b, const char *msg)
{
    glyph_buf_str(b, " -> script error: ");
    glyph_buf_str(b, msg);
    glyph_buf_str(b, "\nend\n");
    return GLYPH_E_SYNTAX;
}

static int run_line(glyph_vm *vm, span line, glyph_buf *b)
{
    span rest = line, cmd = sp_word(&rest);
    char name[GLYPH_MAX_NAME_LEN + 2];
    glyph_result r;
    agg a;
    dump_ctx dc;

    glyph_buf_str(b, "@ ");
    echo(b, line);
    agg_init(&a);

    if (sp_is(cmd, "FRAME") || sp_is(cmd, "ATTRS")) {
        glyph_buf_ch(b, '\n');
        dc.b = b; dc.attrs = sp_is(cmd, "ATTRS");
        glyph_render(vm, dump_row, &dc);
        return GLYPH_OK;
    }
    if (sp_is(cmd, "STATE")) {
        glyph_buf_ch(b, '\n');
        dump_state(b, vm);
        return GLYPH_OK;
    }
    if (sp_is(cmd, "TURTLE")) {
        int x, y, h, pen;
        glyph_turtle(vm, &x, &y, &h, &pen);
        glyph_buf_str(b, " -> x="); glyph_buf_int(b, x);
        glyph_buf_str(b, " y="); glyph_buf_int(b, y);
        glyph_buf_str(b, " heading="); glyph_buf_int(b, h);
        glyph_buf_str(b, pen ? " pen=down\n" : " pen=up\n");
        return GLYPH_OK;
    }
    if (sp_is(cmd, "KEY")) {
        int k = parse_key(rest);
        if (k < 0) return script_error(b, "bad key");
        glyph_key(vm, k, &r);
        agg_add(&a, vm, &r);
    } else if (sp_is(cmd, "TYPE")) {
        size_t i;
        if (rest.n < 2 || rest.p[0] != '"' || rest.p[rest.n - 1] != '"')
            return script_error(b, "type expects \"text\"");
        for (i = 1; i + 1 < rest.n; i++) {
            glyph_key(vm, (unsigned char)rest.p[i], &r);
            agg_add(&a, vm, &r);
        }
    } else if (sp_is(cmd, "CLICK") || sp_is(cmd, "ACTION")) {
        if (!sp_name(rest, name, sizeof name)) return script_error(b, "expects a name");
        if (sp_is(cmd, "CLICK")) glyph_click(vm, name, &r);
        else glyph_action(vm, name, &r);
        agg_add(&a, vm, &r);
    } else {
        return script_error(b, "unknown command");
    }
    glyph_buf_str(b, " -> ");
    write_result(b, vm, &a, 0);
    return GLYPH_OK;
}

int glyph_transcript(glyph_vm *vm, const char *src, size_t srclen,
                     const char *script, size_t scriptlen, glyph_buf *b)
{
    glyph_diag d;
    glyph_result r;
    agg a;
    size_t pos = 0;
    int e;

    if (!vm || !b || (!script && scriptlen)) return GLYPH_E_ARG;
    glyph_buf_str(b, "glyph " GLYPH_VERSION "\n");
    if (scriptlen > GLYPH_MAX_SCRIPT) {
        glyph_buf_str(b, "script: error larger than 16384 bytes\nend\n");
        return GLYPH_E_SYNTAX;
    }

    e = glyph_load(vm, src, srclen, &d);
    if (e) {
        glyph_buf_str(b, "load: error ");
        glyph_format_diag(b, &d);
        glyph_buf_str(b, "\nend\n");
        return GLYPH_OK;
    }
    glyph_buf_str(b, "load: ok code=");
    glyph_buf_int(b, vm->n_code - 1);
    glyph_buf_str(b, " names=");      glyph_buf_int(b, vm->n_syms);
    glyph_buf_str(b, " components="); glyph_buf_int(b, vm->n_comps);
    glyph_buf_str(b, " state=");      glyph_buf_int(b, vm->n_state);
    glyph_buf_str(b, " arrays=");     glyph_buf_int(b, vm->n_arrays);
    glyph_buf_str(b, " procs=");      glyph_buf_int(b, vm->n_procs);
    glyph_buf_str(b, " handlers=");   glyph_buf_int(b, vm->n_handlers);
    glyph_buf_ch(b, '\n');

    glyph_start(vm, &r);
    agg_init(&a);
    agg_add(&a, vm, &r);
    glyph_buf_str(b, "main: ");
    write_result(b, vm, &a, 1);

    while (pos < scriptlen) {
        span line;
        size_t end = pos;
        while (end < scriptlen && script[end] != '\n') end++;
        line.p = script + pos; line.n = end - pos;
        pos = end + 1;
        line = sp_trim(line);
        if (line.n == 0 || line.p[0] == '#') continue;
        if (run_line(vm, line, b)) return GLYPH_E_SYNTAX;
    }
    glyph_buf_str(b, "end\n");
    return GLYPH_OK;
}

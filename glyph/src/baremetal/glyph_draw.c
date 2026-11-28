/* glyph_draw.c — the /draw loop (see glyph_draw.h). Freestanding. */
#include "glyph_draw.h"

static char g_prompt[GLYPH_DRAW_PROMPT];
static char g_reply[GLYPH_DRAW_REPLY];
static char g_program[GLYPH_DRAW_REPLY + 2];   /* glyph_extract: reply length + 2 */
static char g_error[sizeof(((glyph_diag *)0)->msg) + 24];

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int starts(const char *s, const char *p)
{
    while (*p && *s == *p) { s++; p++; }
    return *p == 0;
}

int glyph_draw_fill(char *out, int cap, const char *tmpl, const char *request,
                    const char *program, const char *error)
{
    int n = 0, end = slen(tmpl), i = 0;
    while (end > 0 && (tmpl[end - 1] == '\n' || tmpl[end - 1] == '\r')) end--;
    while (i < end) {
        const char *v = 0;
        int skip = 0;
        if (tmpl[i] == '{') {
            if (starts(tmpl + i, "{REQUEST}")) { v = request; skip = 9; }
            else if (starts(tmpl + i, "{PROGRAM}")) { v = program; skip = 9; }
            else if (starts(tmpl + i, "{ERROR}")) { v = error; skip = 7; }
        }
        if (skip) {
            int k = v ? slen(v) : 0;
            while (k > 0 && v[k - 1] == '\n') k--;           /* no trailing newline */
            if (n + k >= cap) return -1;
            while (k--) out[n++] = *v++;
            i += skip;
        } else {
            if (n + 1 >= cap) return -1;
            out[n++] = tmpl[i++];
        }
    }
    out[n] = 0;
    return n;
}

static void put_num(char *b, int *n, int v)
{
    char t[12];
    int k = 0;
    if (v < 0) v = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) b[(*n)++] = t[--k];
}

/* "line L: message" (or just the message), the form the repair prompt uses. */
static void format_error(const glyph_diag *d)
{
    int n = 0, i;
    if (d->line > 0) {
        const char *p = "line ";
        while (*p) g_error[n++] = *p++;
        put_num(g_error, &n, d->line);
        g_error[n++] = ':';
        g_error[n++] = ' ';
    }
    for (i = 0; d->msg[i] && n < (int)sizeof g_error - 1; i++) g_error[n++] = d->msg[i];
    g_error[n] = 0;
}

int glyph_draw(const glyph_draw_ops *ops, glyph_vm *scratch, const char *request)
{
    glyph_diag d;
    size_t plen = 0;
    uint64_t rlen;
    int attempt, e = GLYPH_E_ARG, rn;

    while (request && (*request == ' ' || *request == '\t')) request++;
    rn = request ? slen(request) : 0;
    while (rn > 0 && (request[rn - 1] == ' ' || request[rn - 1] == '\t')) rn--;
    if (rn == 0 || rn > GLYPH_DRAW_MAX_REQUEST) {
        ops->out(ops->user, rn ? "[draw] the request is too long\n"
                               : "[draw] usage: /draw WHAT, e.g. /draw a star\n");
        return GLYPH_E_ARG;
    }
    {
        static char req[GLYPH_DRAW_MAX_REQUEST + 1];
        int i;
        for (i = 0; i < rn; i++) req[i] = request[i];
        req[rn] = 0;
        request = req;
    }
    if (glyph_draw_fill(g_prompt, sizeof g_prompt, ops->primer, request, "", "") < 0) {
        ops->out(ops->user, "[draw] prompt too long\n");
        return GLYPH_E_ARG;
    }
    for (attempt = 0; attempt <= GLYPH_DRAW_RETRIES; attempt++) {
        ops->out(ops->user, attempt == 0 ? "[draw] asking the model for a Glyph program\n"
                                         : "[draw] asking the model to fix it\n");
        rlen = ops->generate(ops->user, g_prompt, g_reply, sizeof g_reply);
        if (rlen >= sizeof g_reply) rlen = sizeof g_reply - 1;
        plen = 0;
        e = glyph_extract(scratch, g_reply, (size_t)rlen, g_program, sizeof g_program, &plen, &d);
        if (e == GLYPH_OK) {
            ops->out(ops->user, "[draw] drawing it (ESC returns)\n");
            e = ops->run(ops->user, g_program, plen);
            /* after the run, so it is on the screen when the drawing closes */
            ops->out(ops->user, "[draw] program:\n");
            ops->out(ops->user, g_program);
            ops->out(ops->user, e == GLYPH_OK ? "[draw] done\n" : "[draw] the program stopped with an error\n");
            return e;
        }
        format_error(&d);
        ops->out(ops->user, "[draw] ");
        ops->out(ops->user, plen ? "error in the model's program: " : "");
        ops->out(ops->user, g_error);
        ops->out(ops->user, "\n");
        if (plen == 0) return e;                       /* no program: a retry would repeat */
        if (attempt == GLYPH_DRAW_RETRIES) break;
        if (glyph_draw_fill(g_prompt, sizeof g_prompt, ops->repair, request, g_program, g_error) < 0) {
            ops->out(ops->user, "[draw] repair prompt too long\n");
            return e;
        }
    }
    ops->out(ops->user, "[draw] no working program; try rewording the request\n");
    return e;
}

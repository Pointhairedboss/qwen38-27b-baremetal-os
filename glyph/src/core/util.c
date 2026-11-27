/* util.c — freestanding helpers: memory, strings, diagnostics, text writer.
 * No libc. Loops are written plainly; the freestanding build passes
 * -fno-tree-loop-distribute-patterns so the compiler does not turn them
 * back into memset/memcpy calls (checked by `make freestanding`). */
#include "glyph_internal.h"

void g_zero(void *p, size_t n)
{
    unsigned char *d = (unsigned char *)p;
    while (n--) *d++ = 0;
}

void g_copy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

size_t g_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int g_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

int g_ieq(const char *a, const char *b)
{
    while (*a && up(*a) == up(*b)) { a++; b++; }
    return up(*a) == up(*b);
}

/* ---- bounded writer --------------------------------------------------------- */
void glyph_buf_init(glyph_buf *b, char *mem, size_t cap)
{
    b->p = mem; b->cap = cap; b->len = 0; b->truncated = 0;
    if (mem && cap) mem[0] = 0;
}

void glyph_buf_ch(glyph_buf *b, char c)
{
    if (!b->p || b->cap == 0) { b->truncated = 1; return; }
    if (b->len + 1 >= b->cap) { b->truncated = 1; return; }
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

void glyph_buf_mem(glyph_buf *b, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) glyph_buf_ch(b, s[i]);
}

void glyph_buf_str(glyph_buf *b, const char *s)
{
    while (*s) glyph_buf_ch(b, *s++);
}

void glyph_buf_int(glyph_buf *b, long long v)
{
    char tmp[24];
    int n = 0;
    unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1u : (unsigned long long)v;
    if (v < 0) glyph_buf_ch(b, '-');
    do { tmp[n++] = (char)('0' + (int)(u % 10u)); u /= 10u; } while (u && n < 24);
    while (n) glyph_buf_ch(b, tmp[--n]);
}

void glyph_buf_hex2(glyph_buf *b, unsigned v)
{
    static const char hx[] = "0123456789abcdef";
    glyph_buf_ch(b, hx[(v >> 4) & 15u]);
    glyph_buf_ch(b, hx[v & 15u]);
}

/* ---- diagnostics ---------------------------------------------------------- */
static const char *const code_names[GLYPH_CODE_COUNT] = {
    "OK", "IGNORED", "E_ARG", "E_SOURCE_SIZE", "E_CHAR", "E_STRING", "E_STR_LEN",
    "E_NAME_LEN", "E_NUMBER", "E_TOKENS", "E_SYNTAX", "E_UNCLOSED", "E_NEST",
    "E_NODES", "E_NAMES", "E_STRPOOL", "E_UNKNOWN", "E_REDEFINE", "E_TOPLEVEL",
    "E_UNDEFINED", "E_KIND", "E_LIMIT", "E_RANGE", "E_TYPE", "E_UNSET",
    "E_BUDGET", "E_DEPTH", "E_FAULTED", "E_STATE"
};

const char *glyph_code_name(int code)
{
    if (code < 0 || code >= GLYPH_CODE_COUNT) return "E_?";
    return code_names[code];
}

void g_diag(glyph_diag *d, int code, int line, int col, const char *m1, const char *m2, const char *m3)
{
    glyph_buf b;
    if (!d) return;
    d->code = code; d->line = line; d->col = col;
    glyph_buf_init(&b, d->msg, sizeof d->msg);
    if (m1) glyph_buf_str(&b, m1);
    if (m2) glyph_buf_str(&b, m2);
    if (m3) glyph_buf_str(&b, m3);
}

void g_diag_num(glyph_diag *d, int code, int line, int col, const char *m1, long long v, const char *m2)
{
    glyph_buf b;
    if (!d) return;
    d->code = code; d->line = line; d->col = col;
    glyph_buf_init(&b, d->msg, sizeof d->msg);
    if (m1) glyph_buf_str(&b, m1);
    glyph_buf_int(&b, v);
    if (m2) glyph_buf_str(&b, m2);
}

void g_copy_diag(glyph_diag *dst, const glyph_diag *src)
{
    if (dst && src && dst != src) g_copy(dst, src, sizeof *dst);
}

void glyph_format_diag(glyph_buf *b, const glyph_diag *d)
{
    glyph_buf_str(b, glyph_code_name(d->code));
    if (d->line > 0) {
        glyph_buf_ch(b, ' ');
        glyph_buf_int(b, d->line);
        glyph_buf_ch(b, ':');
        glyph_buf_int(b, d->col);
    }
    if (d->msg[0]) {
        glyph_buf_ch(b, ' ');
        glyph_buf_str(b, d->msg);
    }
}

void g_result_reset(glyph_vm *vm, glyph_result *r)
{
    if (!r) return;
    r->status = GLYPH_OK;
    r->action = -1;
    r->focus = vm ? vm->focus : -1;
    r->steps = 0;
    r->changed = 0;
    r->diag.code = GLYPH_OK;
    r->diag.line = 0;
    r->diag.col = 0;
    r->diag.msg[0] = 0;
}

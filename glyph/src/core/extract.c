/* extract.c — pull a Glyph program out of free-form model output.
 *
 * Models wrap code in prose ("Here is the program:"), Markdown fences and
 * numbered lists. glyph_extract keeps the code and drops the rest, without
 * hiding genuine mistakes:
 *
 *  1. Each line is cleaned: ``` fence lines, <think> tags, headings, lines
 *     ending with ':' and lines that cannot start a Glyph statement (first
 *     character not a letter, '[' or ';') are dropped; list markers ("- ",
 *     "* ", "3. ", "3) "), labels ("a star: ", up to the first ": "
 *     outside strings and comments, which Glyph never uses) and Markdown
 *     wrappers around the code (**...**, __...__, `...`) are removed.
 *  2. The remaining text is loaded. If the first error is on a line that is
 *     prose — the error is at the line's first word and that word is not a
 *     Glyph word, or the line ends with ':', or it contains characters Glyph
 *     never uses outside strings and comments (` ? ! , ') — that line is
 *     dropped and loading is retried.
 *  3. Any other error is a genuine code error and is returned, so the caller
 *     can show it or hand it back to the model.
 *
 * Bounded: at most one load per input line, each load bounded by the
 * program limits. Freestanding like the rest of the core.
 */
#include "glyph_internal.h"

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* Characters that only prose uses (outside strings and comments). */
static int has_prose_char(const char *s, size_t n)
{
    size_t i;
    int in_str = 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (c == ';') return 0;
        if (c == '`' || c == '?' || c == '!' || c == ',' || c == '\'') return 1;
    }
    return 0;
}

static int ends_with_colon(const char *s, size_t n)
{
    while (n && is_space(s[n - 1])) n--;
    return n && s[n - 1] == ':';
}

/* Is the first word of the line a Glyph reserved word? */
static int starts_with_keyword(const char *s, size_t n)
{
    char w[GLYPH_MAX_NAME_LEN + 1];
    size_t k = 0;
    while (k < n && k < GLYPH_MAX_NAME_LEN && (is_alpha(s[k]) || is_digit(s[k]) || s[k] == '_')) {
        char c = s[k];
        w[k++] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    if (k == 0 || (k < n && (is_alpha(s[k]) || is_digit(s[k]) || s[k] == '_'))) return 0;
    w[k] = 0;
    return is_reserved(w);
}

/* Remove line `line` (1-based) from buf[0..*n). */
static void drop_line(char *buf, size_t *n, int line)
{
    size_t start = 0, end, i;
    int cur = 1;
    while (cur < line && start < *n) { if (buf[start] == '\n') cur++; start++; }
    end = start;
    while (end < *n && buf[end] != '\n') end++;
    if (end < *n) end++;                                  /* include the newline */
    for (i = 0; end + i < *n; i++) buf[start + i] = buf[end + i];
    *n -= end - start;
    buf[*n] = 0;
}

/* Start and length of line `line` (1-based). */
static const char *line_at(const char *buf, size_t n, int line, size_t *len)
{
    size_t start = 0, end;
    int cur = 1;
    while (cur < line && start < n) { if (buf[start] == '\n') cur++; start++; }
    end = start;
    while (end < n && buf[end] != '\n') end++;
    *len = end - start;
    return buf + start;
}

/* Markdown emphasis and inline code around the whole line: **x**, __x__, `x`. */
static void unwrap(const char *t, size_t *s, size_t *e)
{
    for (;;) {
        while (*s < *e && is_space(t[*s])) (*s)++;
        while (*e > *s && is_space(t[*e - 1])) (*e)--;
        if (*e - *s >= 4 && ((t[*s] == '*' && t[*s + 1] == '*') || (t[*s] == '_' && t[*s + 1] == '_'))) {
            *s += 2;
            if (t[*e - 1] == t[*s - 1] && t[*e - 2] == t[*s - 1]) *e -= 2;
            continue;
        }
        if (*e - *s >= 2 && t[*s] == '`' && t[*e - 1] == '`') { (*s)++; (*e)--; continue; }
        return;
    }
}

/* Narrow text[*s, *e) to the code on one line; 0 = drop the line. */
static int clean_line(const char *t, size_t *s, size_t *e)
{
    size_t k;
    while (*s < *e && is_space(t[*s])) (*s)++;
    while (*e > *s && is_space(t[*e - 1])) (*e)--;
    if (*s == *e) return 0;
    if (*e - *s >= 3 && t[*s] == '`' && t[*s + 1] == '`' && t[*s + 2] == '`') return 0;  /* fence */
    if (t[*s] == '<' || t[*s] == '#') return 0;                    /* <think> tags, headings */
    unwrap(t, s, e);
    if (*s < *e && (t[*s] == '-' || t[*s] == '*' || t[*s] == '+') && *s + 1 < *e && t[*s + 1] == ' ')
        *s += 2;                                                     /* bullet */
    k = *s;
    while (k < *e && is_digit(t[k])) k++;
    if (k > *s && k + 1 < *e && (t[k] == '.' || t[k] == ')') && t[k + 1] == ' ') *s = k + 2;  /* 3. 3) */
    unwrap(t, s, e);
    if (ends_with_colon(t + *s, *e - *s)) return 0;
    /* "a star: REPEAT 5 [...]", "Program: FD 3": Glyph never has ": " outside
     * strings and comments, so a prefix up to the first ": " is a label */
    for (k = *s; k + 1 < *e; k++) {
        if (t[k] == '"' || t[k] == '[' || t[k] == ';') break;
        if (t[k] == ':' && t[k + 1] == ' ') { *s = k + 2; break; }
    }
    unwrap(t, s, e);
    return *s < *e && (is_alpha(t[*s]) || t[*s] == '[' || t[*s] == ';');
}

int glyph_extract(glyph_vm *vm, const char *text, size_t len, char *out, size_t cap,
                  size_t *outlen, glyph_diag *d)
{
    size_t pos = 0, n = 0;
    int lines = 0, iter, e;
    glyph_diag local;
    if (!d) d = &local;
    if (outlen) *outlen = 0;
    g_diag(d, GLYPH_OK, 0, 0, 0, 0, 0);
    if (!vm || !out || (!text && len)) { g_diag(d, GLYPH_E_ARG, 0, 0, "bad arguments", 0, 0); return GLYPH_E_ARG; }
    /* cleaned text is at most len + 1 bytes (a newline is added to a last line
     * without one), plus the NUL */
    if (cap < len + 2 || len > GLYPH_MAX_SOURCE) {
        g_diag(d, GLYPH_E_SOURCE_SIZE, 0, 0, "model output too large", 0, 0);
        return GLYPH_E_SOURCE_SIZE;
    }

    /* 1. clean line by line into out */
    while (pos < len) {
        size_t s = pos, e2, k;
        while (pos < len && text[pos] != '\n') pos++;
        e2 = pos;
        if (pos < len) pos++;
        if (!clean_line(text, &s, &e2)) continue;
        for (k = s; k < e2; k++) out[n++] = text[k];
        out[n++] = '\n';
        lines++;
    }
    out[n] = 0;

    /* 2. load; drop prose lines that cause the first error */
    for (iter = 0; iter <= lines; iter++) {
        size_t llen, first;
        const char *l;
        if (n == 0) break;
        e = glyph_load(vm, out, n, d);
        if (e == GLYPH_OK) {
            if (outlen) *outlen = n;
            return GLYPH_OK;
        }
        if (d->line <= 0) return e;
        l = line_at(out, n, d->line, &llen);
        first = 0;
        while (first < llen && is_space(l[first])) first++;
        if (first < llen && ((d->col == (int)first + 1 && !starts_with_keyword(l + first, llen - first)) ||
                             has_prose_char(l, llen))) {
            drop_line(out, &n, d->line);
            continue;
        }
        if (outlen) *outlen = n;
        return e;                       /* genuine code error (or one at the end of the text) */
    }
    if (outlen) *outlen = 0;
    out[0] = 0;
    g_diag(d, GLYPH_E_SYNTAX, 0, 0, "no Glyph program found in the model output", 0, 0);
    return GLYPH_E_SYNTAX;
}

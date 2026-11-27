/* lex.c — incremental tokeniser. The parser pulls one token at a time; the
 * lexer never looks further ahead than the current token and never reads
 * outside [src, src+len).
 *
 * Minus follows UCBLogo's spacing rule: a '-' that follows whitespace, '('
 * or '[' (or starts the text) and is immediately followed by a non-space
 * character starts a new input ("SETXY 10 -5" is two inputs); a '-'
 * immediately followed by a digit in that position is a negative literal.
 * Any other '-' is binary subtraction (":a - 5", ":a-5"). */
#include "glyph_internal.h"

void lex_begin(glyph_lexer *lx, const char *src, uint32_t len)
{
    lx->src = src; lx->len = len; lx->pos = 0;
    lx->line = 1; lx->col = 1; lx->ntokens = 0;
    lx->type = TOK_EOF; lx->op = 0; lx->unary = 0; lx->tline = 1; lx->tcol = 1;
    lx->ival = 0; lx->tlen = 0; lx->text[0] = 0;
}

static int peekc(const glyph_lexer *lx) { return lx->pos < lx->len ? (unsigned char)lx->src[lx->pos] : -1; }

static void advance(glyph_lexer *lx)
{
    if (lx->pos >= lx->len) return;
    if (lx->src[lx->pos] == '\n') {
        if (lx->line < 0xFFFF) lx->line++;
        lx->col = 1;
    } else if (lx->col < 0xFFFF) {
        lx->col++;
    }
    lx->pos++;
}

static int is_alpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_word(int c)  { return is_alpha(c) || is_digit(c) || c == '_'; }
static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* True if the current character is in a position where '-' starts an input. */
static int starts_input(const glyph_lexer *lx)
{
    int prev;
    if (lx->pos == 0) return 1;
    prev = (unsigned char)lx->src[lx->pos - 1];
    return is_space(prev) || prev == '(' || prev == '[';
}

static int bad_char(glyph_lexer *lx, glyph_diag *d, int c)
{
    glyph_buf b;
    char msg[40];
    glyph_buf_init(&b, msg, sizeof msg);
    glyph_buf_str(&b, "unexpected character 0x");
    glyph_buf_hex2(&b, (unsigned)c);
    g_diag(d, GLYPH_E_CHAR, lx->line, lx->col, msg, 0, 0);
    return GLYPH_E_CHAR;
}

/* Read a word into lx->text (upper-cased). */
static int read_word(glyph_lexer *lx, glyph_diag *d)
{
    uint8_t n = 0;
    while (is_word(peekc(lx))) {
        int c = peekc(lx);
        if (n >= GLYPH_MAX_NAME_LEN) {
            g_diag(d, GLYPH_E_NAME_LEN, lx->tline, lx->tcol, "name longer than 15 characters", 0, 0);
            return GLYPH_E_NAME_LEN;
        }
        lx->text[n++] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
        advance(lx);
    }
    lx->text[n] = 0;
    lx->tlen = n;
    return GLYPH_OK;
}

int lex_next(glyph_lexer *lx, glyph_diag *d)
{
    int c;
    /* skip whitespace and comments */
    for (;;) {
        c = peekc(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(lx); continue; }
        if (c == ';') {
            while (peekc(lx) != -1 && peekc(lx) != '\n') {
                int k = peekc(lx);
                /* comments are never rendered: UTF-8 is allowed, control bytes are not */
                if (k != '\t' && k != '\r' && (k < 0x20 || k == 0x7F)) return bad_char(lx, d, k);
                advance(lx);
            }
            continue;
        }
        break;
    }
    lx->tline = lx->line; lx->tcol = lx->col;
    lx->tlen = 0; lx->text[0] = 0; lx->ival = 0; lx->op = 0; lx->unary = 0;
    if (c == -1) { lx->type = TOK_EOF; return GLYPH_OK; }

    if (++lx->ntokens > GLYPH_MAX_TOKENS) {
        g_diag_num(d, GLYPH_E_TOKENS, lx->tline, lx->tcol, "more than ", GLYPH_MAX_TOKENS, " tokens");
        return GLYPH_E_TOKENS;
    }

    if (c == '[') { advance(lx); lx->type = TOK_LBRACK; return GLYPH_OK; }
    if (c == ']') { advance(lx); lx->type = TOK_RBRACK; return GLYPH_OK; }
    if (c == '(') { advance(lx); lx->type = TOK_LPAREN; return GLYPH_OK; }
    if (c == ')') { advance(lx); lx->type = TOK_RPAREN; return GLYPH_OK; }
    if (c == '+' || c == '*' || c == '/' || c == '=' || c == '<' || c == '>' ||
        (c == '-' && !(starts_input(lx) && lx->pos + 1 < lx->len && is_digit((unsigned char)lx->src[lx->pos + 1])))) {
        lx->type = TOK_OP;
        if (c == '-') {
            lx->op = TOP_MINUS;
            lx->unary = (uint8_t)(starts_input(lx) && lx->pos + 1 < lx->len &&
                                  !is_space((unsigned char)lx->src[lx->pos + 1]));
            advance(lx);
            return GLYPH_OK;
        }
        advance(lx);
        switch (c) {
        case '+': lx->op = TOP_PLUS; break;
        case '*': lx->op = TOP_MUL; break;
        case '/': lx->op = TOP_DIV; break;
        case '=': lx->op = TOP_EQ; break;
        case '<':
            if (peekc(lx) == '=') { advance(lx); lx->op = TOP_LE; }
            else if (peekc(lx) == '>') { advance(lx); lx->op = TOP_NE; }
            else lx->op = TOP_LT;
            break;
        default:
            if (peekc(lx) == '=') { advance(lx); lx->op = TOP_GE; }
            else lx->op = TOP_GT;
        }
        return GLYPH_OK;
    }

    if (is_alpha(c)) { lx->type = TOK_WORD; return read_word(lx, d); }

    if (c == ':') {
        advance(lx);
        if (!is_alpha(peekc(lx))) {
            g_diag(d, GLYPH_E_SYNTAX, lx->tline, lx->tcol, "':' must be followed by a variable name", 0, 0);
            return GLYPH_E_SYNTAX;
        }
        lx->type = TOK_VAR;
        return read_word(lx, d);
    }

    if (is_digit(c) || c == '-') {     /* '-' here is always a negative literal */
        int neg = 0;
        int32_t v = 0;
        if (c == '-') { neg = 1; advance(lx); }
        while (is_digit(peekc(lx))) {
            int dgt = peekc(lx) - '0';
            if (v > (GLYPH_INT_MAX - dgt) / 10) {
                g_diag(d, GLYPH_E_NUMBER, lx->tline, lx->tcol, "number out of range (max 999999999)", 0, 0);
                return GLYPH_E_NUMBER;
            }
            v = v * 10 + dgt;
            advance(lx);
        }
        if (is_alpha(peekc(lx)) || peekc(lx) == '_') {
            g_diag(d, GLYPH_E_NUMBER, lx->tline, lx->tcol, "malformed number", 0, 0);
            return GLYPH_E_NUMBER;
        }
        lx->type = TOK_INT;
        lx->ival = neg ? -v : v;
        return GLYPH_OK;
    }

    if (c == '"') {
        uint8_t n = 0;
        advance(lx);
        for (;;) {
            int k = peekc(lx);
            if (k == -1 || k == '\n' || k == '\r') {
                g_diag(d, GLYPH_E_STRING, lx->tline, lx->tcol, "unterminated string", 0, 0);
                return GLYPH_E_STRING;
            }
            if (k == '"') { advance(lx); break; }
            if (k < 0x20 || k > 0x7E) return bad_char(lx, d, k);
            if (n >= GLYPH_MAX_STR_LEN) {
                g_diag(d, GLYPH_E_STR_LEN, lx->tline, lx->tcol, "string longer than 63 characters", 0, 0);
                return GLYPH_E_STR_LEN;
            }
            lx->text[n++] = (char)k;
            advance(lx);
        }
        lx->text[n] = 0;
        lx->tlen = n;
        lx->type = TOK_STR;
        return GLYPH_OK;
    }

    return bad_char(lx, d, c);
}

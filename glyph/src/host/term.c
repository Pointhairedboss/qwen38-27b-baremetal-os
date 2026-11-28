/* term.c — Linux terminal backend for the shared runner.
 *
 * Output: ANSI escape sequences, VGA attributes mapped to the 16 ANSI colours.
 * Input: raw-mode bytes from stdin. Escape sequences (arrow keys etc.) are
 * swallowed so they are not mistaken for a bare ESC; Ctrl-C ends the session.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "term.h"

static struct termios saved;
static int raw_active;

static void out(const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(STDOUT_FILENO, s, n);
        if (w < 0) { if (errno == EINTR) continue; return; }
        s += w; n -= (size_t)w;
    }
}

static void outs(const char *s) { out(s, strlen(s)); }

void term_close(void)
{
    if (!raw_active) return;
    outs("\x1b[0m\x1b[?25h\x1b[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    raw_active = 0;
}

static void on_signal(int sig)
{
    term_close();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Terminals whose locale is not UTF-8 get ASCII line art instead of the
 * CP437 box-drawing, block and arrow characters. */
static int utf8_ok = 1;
static int locale_is_utf8(void);

int term_open(void)
{
    utf8_ok = locale_is_utf8();
    struct termios t;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return -1;
    t = saved;
    t.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG | IEXTEN);
    t.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return -1;
    raw_active = 1;
    atexit(term_close);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    outs("\x1b[?1049h\x1b[?25l\x1b[2J");
    return 0;
}

/* ---- output --------------------------------------------------------------- */
/* VGA colour index -> ANSI colour index */
static const int vga_ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static char frame_buf[64 * 1024];
static size_t frame_len;

static void fb(const char *s, size_t n)
{
    if (frame_len + n > sizeof frame_buf) { out(frame_buf, frame_len); frame_len = 0; }
    memcpy(frame_buf + frame_len, s, n);
    frame_len += n;
}

static void fbs(const char *s) { fb(s, strlen(s)); }

static void sgr(int attr)
{
    char tmp[32];
    int fg = attr & 7, bright = attr & 8, bg = (attr >> 4) & 7, blink = attr & 0x80;
    int n = snprintf(tmp, sizeof tmp, "\x1b[0;%s%d;%dm", blink ? "5;" : "", (bright ? 90 : 30) + vga_ansi[fg],
                     40 + vga_ansi[bg]);
    fb(tmp, (size_t)n);
}

static void t_begin(void *user, int w, int h)
{
    (void)user; (void)w; (void)h;
    frame_len = 0;
    fbs("\x1b[H");
}

static int locale_is_utf8(void)
{
    const char *v = getenv("LC_ALL");
    if (!v || !*v) v = getenv("LC_CTYPE");
    if (!v || !*v) v = getenv("LANG");
    if (!v) return 0;
    for (; *v; v++)
        if ((v[0] == 'U' || v[0] == 'u') && (v[1] == 'T' || v[1] == 't') && (v[2] == 'F' || v[2] == 'f'))
            return 1;
    return 0;
}

static char ascii_of(unsigned char c)
{
    uint32_t u = glyph_cp437_unicode(c);
    if (c >= 0x20 && c <= 0x7E) return (char)c;
    if (u == 0x2500 || u == 0x2550) return '-';                     /* ─ ═ */
    if (u == 0x2502 || u == 0x2551) return '|';                     /* │ ║ */
    if (u >= 0x2500 && u <= 0x257F) return '+';                     /* corners, joins */
    if (u == 0x2591 || u == 0x2592) return ':';                     /* ░ ▒ */
    if (u == 0x2593 || (u >= 0x2580 && u <= 0x2590)) return '#';    /* ▓ █ ▀ ▄ ▌ ▐ */
    if (u == 0x25BA || u == 0x2192) return '>';
    if (u == 0x25C4 || u == 0x2190) return '<';
    if (u == 0x00B7 || u == 0x2219 || u == 0x2022) return '.';
    if (u == 0x0020 || u == 0x00A0) return ' ';
    return '?';
}

static void t_row(void *user, int y, const glyph_cell *cells, int w)
{
    int x, cur = -1;
    (void)user; (void)y;
    for (x = 0; x < w; x++) {
        int a = GLYPH_CELL_ATTR(cells[x]);
        char u[4];
        if (a != cur) { sgr(a); cur = a; }
        if (utf8_ok) fb(u, (size_t)glyph_cp437_utf8(GLYPH_CELL_CH(cells[x]), u));   /* CP437 -> UTF-8 */
        else { u[0] = ascii_of(GLYPH_CELL_CH(cells[x])); fb(u, 1); }
    }
    fbs("\x1b[0m\x1b[K\r\n");
}

static void t_status(void *user, const char *text, int len)
{
    (void)user;
    sgr(GLYPH_ATTR_STATUS);
    fb(text, (size_t)len);
    /* No newline: an 80x24 frame plus this line exactly fills an 80x25
     * terminal, and a newline here would scroll it every frame. */
    fbs("\x1b[0m\x1b[K");
}

static void t_end(void *user)
{
    (void)user;
    fbs("\x1b[J");
    out(frame_buf, frame_len);
    frame_len = 0;
}

/* ---- input ---------------------------------------------------------------- */
static int t_poll(void *user)
{
    unsigned char c;
    ssize_t r;
    (void)user;
    do r = read(STDIN_FILENO, &c, 1); while (r < 0 && errno == EINTR);
    if (r <= 0) return GLYPH_KEY_EOF;
    if (c == 3 || c == 4) return GLYPH_KEY_EOF;      /* Ctrl-C, Ctrl-D */
    if (c == 27) {
        struct pollfd p;
        p.fd = STDIN_FILENO; p.events = POLLIN;
        if (poll(&p, 1, 30) > 0) {                   /* escape sequence: swallow */
            unsigned char junk[16];
            do r = read(STDIN_FILENO, junk, sizeof junk); while (r < 0 && errno == EINTR);
            return GLYPH_KEY_NONE;
        }
        return GLYPH_KEY_ESC;
    }
    return c;
}

void term_backend(glyph_backend *be)
{
    memset(be, 0, sizeof *be);
    be->poll_key = t_poll;
    be->begin_frame = t_begin;
    be->draw_row = t_row;
    be->status = t_status;
    be->end_frame = t_end;
}

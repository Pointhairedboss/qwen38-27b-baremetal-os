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

int term_open(void)
{
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
    int fg = attr & 7, bright = attr & 8, bg = (attr >> 4) & 7;
    int n = snprintf(tmp, sizeof tmp, "\x1b[0;%d;%dm", (bright ? 90 : 30) + vga_ansi[fg], 40 + vga_ansi[bg]);
    fb(tmp, (size_t)n);
}

static void t_begin(void *user, int w, int h)
{
    (void)user; (void)w; (void)h;
    frame_len = 0;
    fbs("\x1b[H");
}

static void t_row(void *user, int y, const glyph_cell *cells, int w)
{
    int x, cur = -1;
    (void)user; (void)y;
    for (x = 0; x < w; x++) {
        int a = GLYPH_CELL_ATTR(cells[x]);
        char c = (char)GLYPH_CELL_CH(cells[x]);
        if (a != cur) { sgr(a); cur = a; }
        if (c < 0x20 || c > 0x7E) c = '?';
        fb(&c, 1);
    }
    fbs("\x1b[0m\x1b[K\r\n");
}

static void t_status(void *user, const char *text, int len)
{
    (void)user;
    fbs("\x1b[7m");
    fb(text, (size_t)len);
    fbs("\x1b[0m\x1b[K\r\n");
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

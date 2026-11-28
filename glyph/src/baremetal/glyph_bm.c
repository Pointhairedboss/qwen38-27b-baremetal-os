/* glyph_bm.c — bare-metal backend for Pointhairedboss/qwen38-27b-baremetal-os.
 *
 * Input and console text go through the kernel's EXISTING drivers
 * (02-baremetal-os/drivers.asm; byte-identical in 04-qwen38-27b-os at fork
 * commit 6c43dec); no driver code is duplicated.
 *
 * ABI notes (from source review of drivers.asm at 6c43dec):
 *   vga_clear(dil), console_write(rdi, rsi), ps2_try_getc() -> eax,
 *   serial_try_getc() -> eax take arguments in SysV registers and clobber only
 *   caller-saved registers, so they are callable from C. serial_putc takes
 *   its byte in AL and is NOT C-callable; it is not used.
 *
 * Rendering writes Glyph's cells straight into VGA text memory at 0xB8000,
 * the 80x25 buffer the kernel's own driver writes. A Glyph cell is already a
 * VGA cell (CP437 character + attribute byte), so a row is a plain copy. The
 * driver's vga_putc_utf8 would turn box-drawing and block characters into
 * '?', and the old clear-and-stream redraw flickered. The app is centred on a
 * shaded "desktop"; the status line is VGA row 24. The hardware cursor is
 * hidden during a session (cursor_hide). On exit vga_clear() resets the
 * screen and the driver's cursor for the asm-llm> prompt.
 *
 * Compile with the flags in the Makefile's FREESTANDING variable.
 */
#include "glyph.h"
#include "glyph_backend.h"

extern void vga_clear(uint8_t attr);
extern void console_write(const void *buf, uint64_t len);
extern int  ps2_try_getc(void);
extern int  serial_try_getc(void);

/* Kernel-lifetime storage, zeroed by boot.asm with the rest of .bss. */
static glyph_vm g_glyph_vm;
static char     g_glyph_out[32768];

#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_TEXT ((volatile uint16_t *)0xB8000)
#define DESKTOP  GLYPH_CELL(0xB0, 0x18)        /* ░ dark grey on blue */

/* ---- backend callbacks -------------------------------------------------------- */
typedef struct bm_state { int ox, oy; } bm_state;   /* app origin on the screen */

static int bm_poll(void *user)
{
    int c;
    (void)user;
    c = serial_try_getc();
    if (c < 0) c = ps2_try_getc();
    if (c < 0) return GLYPH_KEY_NONE;
    if (c == 4) return GLYPH_KEY_EOF;          /* Ctrl-D, as console_readline */
    return c & 0xFF;
}

static void bm_idle(void *user)
{
    (void)user;
    __asm__ volatile("pause");
}

static void bm_begin(void *user, int w, int h)
{
    bm_state *s = (bm_state *)user;
    int x, y, ox = (VGA_COLS - w) / 2, oy = (VGA_ROWS - 1 - h) / 2;
    if (ox != s->ox || oy != s->oy) {          /* first frame (or a new size): paint the desktop */
        for (y = 0; y < VGA_ROWS - 1; y++)
            for (x = 0; x < VGA_COLS; x++) {
                int shadow = (y == oy + h && x >= ox + 2 && x < ox + w + 2) ||
                             (x >= ox + w && x < ox + w + 2 && y > oy && y <= oy + h);
                VGA_TEXT[y * VGA_COLS + x] = shadow ? GLYPH_CELL(' ', GLYPH_ATTR_SHADOW) : DESKTOP;
            }
        s->ox = ox;
        s->oy = oy;
    }
}

static void bm_row(void *user, int y, const glyph_cell *cells, int w)
{
    bm_state *s = (bm_state *)user;
    volatile uint16_t *dst = VGA_TEXT + (s->oy + y) * VGA_COLS + s->ox;
    int x;
    if (s->oy + y < 0 || s->oy + y >= VGA_ROWS - 1) return;
    for (x = 0; x < w && s->ox + x < VGA_COLS; x++) dst[x] = cells[x];
}

static void bm_status(void *user, const char *text, int len)
{
    volatile uint16_t *dst = VGA_TEXT + (VGA_ROWS - 1) * VGA_COLS;
    int i;
    (void)user;
    for (i = 0; i < VGA_COLS; i++)
        dst[i] = GLYPH_CELL(i < len ? (unsigned char)text[i] : ' ', GLYPH_ATTR_STATUS);
}

/* The kernel's driver never moves or hides the VGA hardware cursor, so it
 * would blink at the top left over the app. Hide it for the session (CRTC
 * register 0x0A, "cursor start", bit 5) and put the old value back after. */
static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

static uint8_t cursor_hide(void)
{
    uint8_t old;
    outb(0x3D4, 0x0A);
    old = inb(0x3D5);
    outb(0x3D5, (uint8_t)(old | 0x20));
    return old;
}

static void cursor_restore(uint8_t old)
{
    outb(0x3D4, 0x0A);
    outb(0x3D5, old);
}

static void bm_backend(glyph_backend *be, bm_state *s)
{
    be->user = s;
    be->poll_key = bm_poll;
    be->idle = bm_idle;
    be->begin_frame = bm_begin;
    be->end_frame = 0;
    be->draw_row = bm_row;
    be->status = bm_status;
}

static void bm_print_diag(const char *what, const glyph_diag *d)
{
    char line[160];
    glyph_buf b;
    glyph_buf_init(&b, line, sizeof line);
    glyph_buf_str(&b, "[glyph] ");
    glyph_buf_str(&b, what);
    glyph_buf_str(&b, ": ");
    if (d && d->code != GLYPH_OK) glyph_format_diag(&b, d);
    else glyph_buf_str(&b, "ok");
    glyph_buf_ch(&b, '\n');
    console_write(line, b.len);
}

/* ---- entry points for the kernel ------------------------------------------- */

/* Validate a program; prints one line to the console. */
int glyph_bm_check(const char *src, uint64_t len);
int glyph_bm_check(const char *src, uint64_t len)
{
    glyph_diag d;
    int e = glyph_load(&g_glyph_vm, src, (size_t)len, &d);
    bm_print_diag("check", &d);
    return e;
}

/* Run a program interactively on VGA + PS/2/COM1 until ESC or Ctrl-D.
 * Synchronous; returns to the caller with the screen cleared. */
int glyph_bm_run(const char *src, uint64_t len);
int glyph_bm_run(const char *src, uint64_t len)
{
    glyph_backend be;
    bm_state s;
    glyph_diag d;
    uint8_t cursor;
    int e = glyph_load(&g_glyph_vm, src, (size_t)len, &d);
    if (e) { bm_print_diag("load", &d); return e; }
    s.ox = s.oy = -1;
    bm_backend(&be, &s);
    cursor = cursor_hide();
    e = glyph_run_interactive(&g_glyph_vm, &be, 0);
    cursor_restore(cursor);
    vga_clear(GLYPH_ATTR_DEFAULT);
    if (e) bm_print_diag("start", glyph_last_diag(&g_glyph_vm));
    return e;
}

/* Conformance: run an event script and write the transcript to the console
 * (COM1 carries the exact bytes for comparison with tests/corpus/ *.expect). */
int glyph_bm_transcript(const char *src, uint64_t len, const char *script, uint64_t slen);
int glyph_bm_transcript(const char *src, uint64_t len, const char *script, uint64_t slen)
{
    glyph_buf b;
    int e;
    glyph_buf_init(&b, g_glyph_out, sizeof g_glyph_out);
    e = glyph_transcript(&g_glyph_vm, src, (size_t)len, script, (size_t)slen, &b);
    console_write(g_glyph_out, b.len);
    if (b.truncated) console_write("[glyph] transcript truncated\n", 29);
    return e;
}

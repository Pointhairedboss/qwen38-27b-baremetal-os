/* glyph_bm.c — bare-metal backend for Pointhairedboss/qwen38-27b-baremetal-os.
 *
 * Implements struct glyph_backend with the kernel's EXISTING drivers
 * (02-baremetal-os/drivers.asm; byte-identical in 04-qwen38-27b-os at fork
 * commit 6c43dec). No driver code is duplicated and no hardware is touched
 * here directly.
 *
 * ABI notes (from source review of drivers.asm at 6c43dec):
 *   vga_clear(dil), vga_set_color(dil), vga_putc_utf8(dil),
 *   console_write(rdi, rsi), ps2_try_getc() -> eax, serial_try_getc() -> eax
 *   take arguments in SysV registers and clobber only caller-saved registers,
 *   so they are callable from C. serial_putc takes its byte in AL and is NOT
 *   C-callable; it is not used.
 *
 * Rendering: a frame is redrawn by vga_clear() (which homes the driver's
 * cursor) followed by streaming every cell through vga_set_color() and
 * vga_putc_utf8(). Glyph frames are at most 80x24 so the status line lands on
 * VGA row 24 and is limited to 79 characters; the driver scrolls if the 80th
 * column of the last row is written.
 *
 * Compile with the flags in the Makefile's FREESTANDING variable.
 */
#include "glyph.h"
#include "glyph_backend.h"

extern void vga_clear(uint8_t attr);
extern void vga_set_color(uint8_t attr);
extern void vga_putc_utf8(uint8_t byte);
extern void console_write(const void *buf, uint64_t len);
extern int  ps2_try_getc(void);
extern int  serial_try_getc(void);

/* Kernel-lifetime storage, zeroed by boot.asm with the rest of .bss. */
static glyph_vm g_glyph_vm;
static char     g_glyph_out[32768];

#define VGA_COLS 80

/* ---- backend callbacks -------------------------------------------------------- */
typedef struct bm_state { uint8_t attr; } bm_state;

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

static void bm_set(bm_state *s, uint8_t attr)
{
    if (s->attr != attr) { vga_set_color(attr); s->attr = attr; }
}

static void bm_begin(void *user, int w, int h)
{
    bm_state *s = (bm_state *)user;
    (void)w; (void)h;
    vga_clear(GLYPH_ATTR_DEFAULT);             /* also homes the cursor */
    s->attr = GLYPH_ATTR_DEFAULT;
}

static void bm_row(void *user, int y, const glyph_cell *cells, int w)
{
    bm_state *s = (bm_state *)user;
    int x;
    (void)y;
    for (x = 0; x < w; x++) {
        bm_set(s, GLYPH_CELL_ATTR(cells[x]));
        vga_putc_utf8(GLYPH_CELL_CH(cells[x]));
    }
    if (w < VGA_COLS) vga_putc_utf8('\n');     /* a full 80-cell row wraps by itself */
}

static void bm_status(void *user, const char *text, int len)
{
    bm_state *s = (bm_state *)user;
    int i;
    bm_set(s, GLYPH_ATTR_FOCUS);
    for (i = 0; i < len && i < VGA_COLS - 1; i++) vga_putc_utf8((uint8_t)text[i]);
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
    int e = glyph_load(&g_glyph_vm, src, (size_t)len, &d);
    if (e) { bm_print_diag("load", &d); return e; }
    s.attr = 0;
    bm_backend(&be, &s);
    e = glyph_run_interactive(&g_glyph_vm, &be, 0);
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

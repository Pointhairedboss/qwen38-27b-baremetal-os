/* glyph_os.c — a standalone Glyph kernel on the fork's boot path.
 *
 * Linked with the fork's own, unmodified 02-baremetal-os/boot.asm,
 * drivers.asm and runtime.asm and its linker.ld, in the same way Phase 4's
 * bartest.asm supplies its own kernel_main. boot.asm brings the machine to
 * 64-bit long mode (identity-mapped 0-4 GiB, exception IDT, stack aligned)
 * and calls kernel_main below. The Qwen inference kernel is not involved
 * (for Glyph inside the inference kernel see glyph_repl.c).
 *
 * Multiboot command line:
 *   app=NAME     run examples/NAME.glyph first (default: demo)
 *   glyph=test   run every embedded conformance case, write the transcripts
 *                to COM1 between BEGIN/END markers, then power off
 *
 * Interactive use: ESC leaves an app and shows a menu of the examples;
 * ESC in the menu powers off.
 */
#include "glyph.h"
#include "glyph_apps.h"

/* fork: boot.asm */
extern uint32_t g_mb_magic, g_mb_info;
/* fork: drivers.asm */
extern void serial_init(void);
extern void vga_clear(uint8_t attr);
extern void vga_set_color(uint8_t attr);
extern int  ps2_try_getc(void);
extern int  serial_try_getc(void);
extern void qemu_shutdown(void);

void kernel_main(void);

static uint64_t slen(const char *s) { uint64_t n = 0; while (s[n]) n++; return n; }

/* Value of key=value in the Multiboot command line, or 0. */
static const char *arg(const char *cmd, const char *key, uint64_t *vlen)
{
    uint64_t k = slen(key);
    while (*cmd) {
        uint64_t i = 0, n = 0;
        while (*cmd == ' ') cmd++;
        while (i < k && cmd[i] == key[i]) i++;
        if (i == k && cmd[k] == '=') {
            const char *v = cmd + k + 1;
            while (v[n] && v[n] != ' ') n++;
            *vlen = n;
            return v;
        }
        while (*cmd && *cmd != ' ') cmd++;
    }
    return 0;
}

static const char *cmdline(void)
{
    const uint32_t *mbi = (const uint32_t *)(uintptr_t)g_mb_info;
    if (g_mb_magic != 0x2BADB002u || !mbi || !(mbi[0] & 4u)) return "";
    return (const char *)(uintptr_t)mbi[4];             /* MBI_CMDLINE, offset 16 */
}

static int key_wait(void)
{
    for (;;) {
        int c = serial_try_getc();
        if (c < 0) c = ps2_try_getc();
        if (c >= 0) return c;
        __asm__ volatile("pause");
    }
}

/* Menu keys: 1-9, then a-z. */
static char menu_key(uint32_t n) { return (char)(n < 9 ? '1' + n : 'a' + (n - 9)); }

static int menu(void)
{
    uint32_t n, k;
    int i;
    char last[2] = { 0, 0 };
    vga_clear(0x07);
    vga_set_color(0x0B);
    glyph_apps_out("Glyph " GLYPH_VERSION " on bare metal (qwen38-27b-baremetal-os boot path)\n\n");
    vga_set_color(0x07);
    for (n = 0; (i = glyph_apps_nth(n)) >= 0 && n < 9 + 26; n++) {
        char key[4] = { ' ', menu_key(n), ' ', 0 };
        glyph_apps_out(key);
        glyph_apps_out(glyph_payloads[i].name + 9);        /* skip "examples/" */
        glyph_apps_out("\n");
    }
    vga_set_color(0x0E);
    glyph_apps_out("\nPress 1-");
    last[0] = menu_key(n ? n - 1 : 0);
    glyph_apps_out(last);
    glyph_apps_out(" to run a program, ESC to power off.\n");
    vga_set_color(0x07);
    for (;;) {
        int c = key_wait();
        if (c == 27) return -1;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        for (k = 0; k < n; k++)
            if (c == menu_key(k)) return glyph_apps_nth(k);
    }
}

void kernel_main(void)
{
    const char *cmd, *v;
    uint64_t vlen = 0;
    int app;

    serial_init();
    vga_clear(0x07);
    cmd = cmdline();
    glyph_apps_out("[glyph-os] Glyph " GLYPH_VERSION ", cmdline: ");
    glyph_apps_out(cmd);
    glyph_apps_out("\n");

    v = arg(cmd, "glyph", &vlen);
    if (v && vlen == 4 && v[0] == 't' && v[1] == 'e' && v[2] == 's' && v[3] == 't') {
        glyph_apps_conformance();
        qemu_shutdown();
    }
    v = arg(cmd, "app", &vlen);
    app = v ? glyph_apps_find(v, vlen) : glyph_apps_find("demo", 4);
    for (;;) {
        if (app >= 0) glyph_apps_run(app);
        app = menu();
        if (app < 0) {
            vga_clear(0x07);
            glyph_apps_out("[glyph-os] powering off\n");
            qemu_shutdown();
        }
    }
}

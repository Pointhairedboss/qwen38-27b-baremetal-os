/* glyph_repl.c — the /glyph command of the inference kernel's REPL.
 *
 * 02-baremetal-os/kernel.asm hands every trimmed REPL line to
 * glyph_shell_repl (glyph_bm_entry.asm), which calls glyph_repl_command
 * below on a 16-byte aligned stack. Lines that are not /glyph, /glyphtest or
 * /draw return 0 and the kernel handles them as before.
 *
 *   /glyph            list the built-in programs
 *   /glyph NAME       run examples/NAME.glyph on VGA (keys: PS/2 or COM1);
 *                     ESC returns to the asm-llm> prompt
 *   /glyphtest        conformance: every embedded program's transcript on
 *                     COM1, for comparison with the host's .expect files
 *   /draw REQUEST     the model writes a Glyph program for REQUEST and Glyph
 *                     draws it (glyph_draw.c); ESC returns. Uses the model
 *                     like /bench: one fresh turn, then the chat starts anew
 *
 * Glyph runs on the BSP between inference turns, synchronously. It does not
 * touch the model, the KV cache, the arena or the AP cores: all of its state
 * is in .bss (glyph_bm.c, glyph_draw.c). /draw asks the kernel for a reply
 * through glyph_generate (kernel.asm) and reads the reply's text with
 * glyph_model_text (glyph_model.asm); the model's output is untrusted input
 * to glyph_extract and glyph_load, like any other source.
 */
#include "glyph.h"
#include "glyph_apps.h"
#include "glyph_draw.h"

int glyph_repl_command(const char *line);
int glyph_bm_run(const char *src, uint64_t len);
/* kernel.asm (Phase 2). Weak, so a kernel without it (Phase 4 today) still
 * links with this object; /draw then says it is not available there. */
uint64_t glyph_generate(const char *prompt, uint64_t max_tok) __attribute__((weak));
uint64_t glyph_model_text(uint64_t n, char *out, uint64_t cap);         /* glyph_model.asm */
extern const char glyph_draw_primer[], glyph_draw_repair[];             /* tools/draw_*.txt */

static glyph_vm g_draw_vm;           /* scratch VM for glyph_extract */

static uint64_t k_generate(void *u, const char *prompt, char *out, uint64_t cap)
{
    (void)u;
    return glyph_model_text(glyph_generate(prompt, GLYPH_DRAW_MAX_TOKENS), out, cap);
}

static int k_run(void *u, const char *src, uint64_t len) { (void)u; return glyph_bm_run(src, len); }
static void k_out(void *u, const char *s) { (void)u; glyph_apps_out(s); }

static int draw(const char *request)
{
    glyph_draw_ops ops;
    if (!glyph_generate) {
        glyph_apps_out("[draw] not available: this kernel has no glyph_generate hook\n");
        return 1;
    }
    ops.generate = k_generate;
    ops.run = k_run;
    ops.out = k_out;
    ops.user = 0;
    ops.primer = glyph_draw_primer;
    ops.repair = glyph_draw_repair;
    /* GLYPH_E_ARG: a usage error, nothing was generated and the chat is intact */
    if (glyph_draw(&ops, &g_draw_vm, request) == GLYPH_E_ARG) return 1;
    glyph_apps_out("[draw] back to asm-llm (new chat)\n");
    return 1;
}

static int starts(const char *s, const char *p)
{
    while (*p && *s == *p) { s++; p++; }
    return *p == 0;
}

int glyph_repl_command(const char *line)
{
    const char *arg;
    uint64_t n = 0;
    int i;

    if (starts(line, "/draw") && (line[5] == 0 || line[5] == ' ')) return draw(line + 5);
    if (!starts(line, "/glyph")) return 0;
    if (starts(line, "/glyphtest") && (line[10] == 0 || line[10] == ' ')) {
        glyph_apps_conformance();
        return 1;
    }
    arg = line + 6;
    if (*arg != 0 && *arg != ' ') return 0;                 /* e.g. "/glyphs" */
    while (*arg == ' ') arg++;
    while (arg[n] && arg[n] != ' ') n++;
    if (n == 0) {
        glyph_apps_out("Glyph " GLYPH_VERSION ": /glyph NAME runs a program (ESC returns here); "
                       "/glyphtest runs the conformance corpus.\nPrograms:\n");
        glyph_apps_list();
        return 1;
    }
    i = glyph_apps_find(arg, n);
    if (i < 0) {
        glyph_apps_out("[glyph] no program with that name; /glyph lists them\n");
        return 1;
    }
    glyph_apps_run(i);
    glyph_apps_out("[glyph] back to asm-llm\n");
    return 1;
}

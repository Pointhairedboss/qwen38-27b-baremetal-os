/* glyph_repl.c — the /glyph command of the inference kernel's REPL.
 *
 * 02-baremetal-os/kernel.asm hands every trimmed REPL line to
 * glyph_shell_repl (glyph_bm_entry.asm), which calls glyph_repl_command
 * below on a 16-byte aligned stack. Lines that do not start with "/glyph"
 * return 0 and the kernel handles them as before.
 *
 *   /glyph            list the built-in programs
 *   /glyph NAME       run examples/NAME.glyph on VGA (keys: PS/2 or COM1);
 *                     ESC returns to the asm-llm> prompt
 *   /glyphtest        conformance: every embedded program's transcript on
 *                     COM1, for comparison with the host's .expect files
 *
 * Glyph runs on the BSP between inference turns, synchronously. It does not
 * touch the model, the KV cache, the arena or the AP cores: all of its state
 * is in .bss (glyph_bm.c).
 */
#include "glyph.h"
#include "glyph_apps.h"

int glyph_repl_command(const char *line);

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

/* glyph_draw.h — /draw REQUEST: the model writes a Glyph program, Glyph draws it.
 *
 * The loop is portable (no kernel calls, no libc) so the host can test it
 * with a fake model (tests/draw_test.c); glyph_repl.c wires it to the
 * inference kernel. One /draw:
 *
 *  1. Fill the primer template ({REQUEST}) and generate a reply.
 *  2. glyph_extract the program from the reply (prose, fences, list markers
 *     are dropped; the program is loaded into the scratch VM, so the limits
 *     and checks of glyph_load apply to model output like any other source).
 *  3. On a code error, fill the repair template ({REQUEST}, {PROGRAM},
 *     {ERROR} as "line L: message") and generate again, at most
 *     GLYPH_DRAW_RETRIES more times. A reply with no program at all is not
 *     retried: decoding is greedy, so the same prompt gives the same reply.
 *  4. Run the program (ops->run); it executes under the step budget.
 */
#ifndef GLYPH_DRAW_H
#define GLYPH_DRAW_H

#include <stdint.h>
#include "glyph.h"

#define GLYPH_DRAW_RETRIES     2      /* repair attempts after the first reply */
#define GLYPH_DRAW_MAX_TOKENS  64     /* tokens per reply */
#define GLYPH_DRAW_MAX_REQUEST 160    /* bytes of request text */
#define GLYPH_DRAW_PROMPT      2048   /* prompt buffer */
#define GLYPH_DRAW_REPLY       4096   /* reply buffer */

typedef struct glyph_draw_ops {
    /* Generate a reply to prompt into out (NUL-terminated, < cap). Returns its length. */
    uint64_t (*generate)(void *user, const char *prompt, char *out, uint64_t cap);
    /* Draw the program; returns a Glyph status. */
    int (*run)(void *user, const char *src, uint64_t len);
    void (*out)(void *user, const char *text);
    void *user;
    const char *primer;   /* templates; a trailing newline is ignored */
    const char *repair;
} glyph_draw_ops;

/* Returns GLYPH_OK when a program was drawn, else the last error code.
 * GLYPH_E_ARG (an empty or too long request, or a template that does not fit)
 * means nothing was generated. */
int glyph_draw(const glyph_draw_ops *ops, glyph_vm *scratch, const char *request);

/* Fill {REQUEST}, {PROGRAM}, {ERROR} in tmpl into out (cap bytes). Values are
 * inserted once, never rescanned. Returns the length, or -1 if it did not fit. */
int glyph_draw_fill(char *out, int cap, const char *tmpl, const char *request,
                    const char *program, const char *error);

#endif

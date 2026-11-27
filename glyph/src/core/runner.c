/* runner.c — the interactive session loop, shared by every backend.
 *
 * The loop is the only place that connects the core to a platform, and it
 * does so solely through struct glyph_backend. Linux supplies a terminal
 * backend; the bare-metal adapter supplies VGA/PS/2/COM1 callbacks.
 */
#include "glyph_internal.h"

#define STATUS_MAX 79   /* one line; a full 80th column would scroll VGA */

static void status_text(const glyph_vm *vm, const glyph_result *r, const char *prefix, glyph_buf *b)
{
    glyph_buf_str(b, prefix);
    if (r->status == GLYPH_OK || r->status == GLYPH_IGNORED) {
        if (r->action >= 0) {
            glyph_buf_str(b, glyph_sym_name(vm, r->action));
            glyph_buf_str(b, ": ok  ");
        }
        glyph_buf_str(b, "TAB next  ENTER/SPACE activate  ESC exit");
    } else {
        glyph_buf_str(b, "error ");
        glyph_format_diag(b, &r->diag);
    }
}

static void present(glyph_vm *vm, const glyph_backend *be, const char *status, size_t len)
{
    if (be->begin_frame) be->begin_frame(be->user, vm->w, vm->h);
    glyph_render(vm, be->draw_row, be->user);
    if (be->status) be->status(be->user, status, (int)len);
    if (be->end_frame) be->end_frame(be->user);
}

int glyph_run_interactive(glyph_vm *vm, const glyph_backend *be, uint32_t max_inputs)
{
    char line[STATUS_MAX + 1];
    glyph_buf b;
    glyph_result r;
    uint32_t inputs = 0;
    int start;

    if (!vm || !be || !be->poll_key || !be->draw_row) return GLYPH_E_ARG;
    start = glyph_start(vm, &r);
    glyph_buf_init(&b, line, sizeof line);
    status_text(vm, &r, start ? "start: " : "", &b);
    present(vm, be, line, b.len);

    for (;;) {
        int key;
        if (max_inputs && inputs >= max_inputs) break;
        key = be->poll_key(be->user);
        if (key == GLYPH_KEY_NONE) {
            if (be->idle) be->idle(be->user);
            continue;
        }
        if (key == GLYPH_KEY_EOF || key == GLYPH_KEY_ESC) break;
        inputs++;
        if (start) continue;              /* faulted: keep showing the start error */
        glyph_key(vm, key, &r);
        glyph_buf_init(&b, line, sizeof line);
        status_text(vm, &r, "", &b);
        present(vm, be, line, b.len);
    }
    return start;
}

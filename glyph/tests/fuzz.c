/* fuzz.c — libFuzzer target for the shared core.
 *
 * Input layout: PROGRAM [ "\n%%\n" SCRIPT ]. The program is loaded, started
 * and driven by the event script through glyph_transcript; then every script
 * byte is also delivered as a raw key. Invariants are asserted after each
 * step. The whole input is also treated as model output for glyph_extract:
 * the result must be NUL-terminated, no longer than the input, and load
 * again on its own when extraction succeeds. Build/run: `make fuzz` (clang, ASan, UBSan).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "glyph.h"
#include "glyph_backend.h"

static glyph_vm vm;
static char out[1 << 16];
static char extracted[GLYPH_MAX_SOURCE + 2];

static void sink(void *u, int y, const glyph_cell *c, int w)
{
    (void)u; (void)c;
    if (y < 0 || y >= GLYPH_MAX_H || w < 1 || w > GLYPH_MAX_W) abort();
}

static void invariants(void)
{
    int f = glyph_focus(&vm);
    if (f >= glyph_comp_count(&vm)) abort();
    if (f >= 0 && !glyph_comp_enabled(&vm, f)) abort();
    if (glyph_width(&vm) < 1 || glyph_width(&vm) > GLYPH_MAX_W) abort();
    if (glyph_height(&vm) < 1 || glyph_height(&vm) > GLYPH_MAX_H) abort();
    glyph_render(&vm, sink, 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const char *src = (const char *)data, *script = "";
    size_t n = size, sn = 0, i;
    glyph_buf b;
    glyph_result r;

    {
        size_t xn = 12345;
        glyph_diag d;
        int e = glyph_extract(&vm, src, size, extracted, sizeof extracted, &xn, &d);
        if (e < 0 || e >= GLYPH_CODE_COUNT) abort();
        if (e != GLYPH_E_SOURCE_SIZE) {
            if (xn > size + 1 || extracted[xn] != 0) abort();
            if (e == GLYPH_OK && glyph_load(&vm, extracted, xn, &d) != GLYPH_OK) abort();
        }
    }
    for (i = 0; i + 4 <= size; i++) {
        if (memcmp(data + i, "\n%%\n", 4) == 0) {
            n = i;
            script = (const char *)data + i + 4;
            sn = size - i - 4;
            break;
        }
    }
    glyph_buf_init(&b, out, sizeof out);
    glyph_transcript(&vm, src, n, script, sn, &b);
    if (b.len >= sizeof out) abort();
    invariants();
    for (i = 0; i < sn && i < 256; i++) {
        glyph_key(&vm, (unsigned char)script[i], &r);
        if (r.status < 0 || r.status >= GLYPH_CODE_COUNT) abort();
        if (r.steps > GLYPH_MAX_STEPS) abort();
        invariants();
    }
    return 0;
}

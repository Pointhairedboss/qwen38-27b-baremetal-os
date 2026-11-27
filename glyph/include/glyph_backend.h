/* glyph_backend.h — the platform boundary of Glyph.
 *
 * The interpreter core (glyph.h) never performs I/O. A backend is a small
 * table of callbacks that a platform supplies; the shared runner
 * (src/core/runner.c) drives an interactive session through it. The same
 * runner is compiled into the Linux host and into the bare-metal adapter.
 *
 * Also declared here: the shared conformance harness (event script ->
 * transcript), so every target produces byte-identical transcripts.
 */
#ifndef GLYPH_BACKEND_H
#define GLYPH_BACKEND_H

#include "glyph.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GLYPH_KEY_NONE  (-1)   /* poll_key: nothing available right now */
#define GLYPH_KEY_EOF   (-2)   /* poll_key: input closed; runner exits */

typedef struct glyph_backend {
    void *user;
    /* Required. Return a key code (0..255), GLYPH_KEY_NONE or GLYPH_KEY_EOF.
     * May block (host) or poll (bare metal). */
    int  (*poll_key)(void *user);
    /* Optional. Called after poll_key returned GLYPH_KEY_NONE. */
    void (*idle)(void *user);
    /* Optional. Bracket one frame; w/h are the frame size in cells. */
    void (*begin_frame)(void *user, int w, int h);
    void (*end_frame)(void *user);
    /* Required. One composed row of the application frame. */
    void (*draw_row)(void *user, int y, const glyph_cell *cells, int w);
    /* Optional. One status line drawn below the frame (ASCII, len <= 79). */
    void (*status)(void *user, const char *text, int len);
} glyph_backend;

/* Run an interactive session on a vm that has been loaded with glyph_load.
 * Calls glyph_start, renders, then loops: poll key -> ESC/EOF exits ->
 * glyph_key -> render. max_inputs bounds the loop (0 = unbounded).
 * Returns GLYPH_OK on a normal exit or the start error code. */
int glyph_run_interactive(glyph_vm *vm, const glyph_backend *be, uint32_t max_inputs);

/* ---- bounded text writer (freestanding) ---------------------------------- */
typedef struct glyph_buf {
    char  *p;
    size_t cap, len;
    int    truncated;
} glyph_buf;

void glyph_buf_init(glyph_buf *b, char *mem, size_t cap);
void glyph_buf_str(glyph_buf *b, const char *s);
void glyph_buf_mem(glyph_buf *b, const char *s, size_t n);
void glyph_buf_ch(glyph_buf *b, char c);
void glyph_buf_int(glyph_buf *b, long long v);
void glyph_buf_hex2(glyph_buf *b, unsigned v);

/* ---- conformance harness --------------------------------------------------- */
/* Load src, start it, run the event script and write the transcript to out.
 * Uses (and clobbers) vm. Returns GLYPH_OK if the transcript was produced,
 * GLYPH_E_ARG on bad arguments, or GLYPH_E_SYNTAX for a malformed script
 * (also reported inside the transcript). out->truncated is set if out was
 * too small. Program errors are recorded in the transcript, not returned. */
int glyph_transcript(glyph_vm *vm, const char *src, size_t srclen,
                     const char *script, size_t scriptlen, glyph_buf *out);

/* Write a one-line diagnostic: "E_CODE line:col message". */
void glyph_format_diag(glyph_buf *b, const glyph_diag *d);

#ifdef __cplusplus
}
#endif
#endif

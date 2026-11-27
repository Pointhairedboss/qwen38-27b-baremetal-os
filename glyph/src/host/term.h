/* term.h — Linux terminal backend (ANSI output, raw keyboard input). */
#ifndef GLYPH_TERM_H
#define GLYPH_TERM_H

#include "glyph_backend.h"

/* Put the terminal in raw mode and switch to the alternate screen.
 * Returns 0 on success, -1 if stdin/stdout is not a terminal. */
int  term_open(void);
/* Restore the terminal. Safe to call more than once. */
void term_close(void);
/* Backend callbacks bound to stdin/stdout. */
void term_backend(glyph_backend *be);

#endif

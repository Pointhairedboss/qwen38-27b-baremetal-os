/* glyph_apps.c — built-in programs for bare-metal kernels (see glyph_apps.h). */
#include "glyph.h"
#include "glyph_backend.h"
#include "glyph_apps.h"

/* fork: drivers.asm */
extern void console_write(const void *buf, uint64_t len);
/* glyph_bm.c */
int glyph_bm_run(const char *src, uint64_t len);
int glyph_bm_transcript(const char *src, uint64_t len, const char *script, uint64_t slen);

static uint64_t slen(const char *s) { uint64_t n = 0; while (s[n]) n++; return n; }

void glyph_apps_out(const char *s) { console_write(s, slen(s)); }

void glyph_apps_out_u32(uint32_t v)
{
    char b[12];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) console_write(&b[--n], 1);
}

void glyph_apps_conformance(void)
{
    uint32_t i;
    for (i = 0; i < glyph_payload_count; i++) {
        const glyph_payload *p = &glyph_payloads[i];
        glyph_apps_out("=== GLYPH BEGIN ");
        glyph_apps_out(p->name);
        glyph_apps_out(" ===\n");
        glyph_bm_transcript(p->src, p->len, p->events, p->elen);
        glyph_apps_out("=== GLYPH END ");
        glyph_apps_out(p->name);
        glyph_apps_out(" ===\n");
    }
    glyph_apps_out("=== GLYPH DONE ");
    glyph_apps_out_u32(glyph_payload_count);
    glyph_apps_out(" ===\n");
}

int glyph_apps_find(const char *name, uint64_t n)
{
    uint32_t i;
    for (i = 0; i < glyph_payload_count; i++) {
        const char *s = glyph_payloads[i].name + 9;          /* skip "examples/" */
        uint64_t k;
        if (!glyph_payloads[i].is_app) continue;
        for (k = 0; k < n && s[k] == name[k]; k++) {}
        if (k == n && s[n] == 0) return (int)i;
    }
    return -1;
}

int glyph_apps_run(int index)
{
    return glyph_bm_run(glyph_payloads[index].src, glyph_payloads[index].len);
}

int glyph_apps_nth(uint32_t n)
{
    uint32_t i, seen = 0;
    for (i = 0; i < glyph_payload_count; i++)
        if (glyph_payloads[i].is_app && seen++ == n) return (int)i;
    return -1;
}

void glyph_apps_list(void)
{
    uint32_t n;
    int i;
    for (n = 0; (i = glyph_apps_nth(n)) >= 0; n++) {
        glyph_apps_out("  ");
        glyph_apps_out(glyph_payloads[i].name + 9);
        glyph_apps_out("\n");
    }
}

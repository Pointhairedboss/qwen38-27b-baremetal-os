/* main.c — the native Linux Glyph executable.
 *
 *   glyph run FILE                interactive session in this terminal
 *   glyph check FILE...           load and validate, print diagnostics
 *   glyph replay FILE [EVENTS|-]  run an event script, print the transcript
 *   glyph test [--update] [DIR]   run the conformance corpus
 *   glyph dump FILE               print the compiled IR
 *   glyph extract FILE|-          pull a Glyph program out of model output
 *   glyph limits                  print compiled-in limits
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glyph_internal.h"
#include "term.h"

static glyph_vm g_vm;
static char g_src[GLYPH_MAX_SOURCE + 1];
static char g_script[GLYPH_MAX_SCRIPT + 1];
static char g_out[1 << 20];
static char g_expect[1 << 20];

static const char default_script[] = "frame\nstate\n";

/* Read at most cap bytes of a file ("-" = stdin). Reading one byte past a
 * limit lets the core report the size error itself. Returns -1 on error. */
static long read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    size_t n;
    if (!f) return -1;
    n = fread(buf, 1, cap, f);
    if (ferror(f)) n = (size_t)-1;
    if (f != stdin) fclose(f);
    return n == (size_t)-1 ? -1 : (long)n;
}

static int load_or_report(const char *path, long *len)
{
    glyph_diag d;
    int e;
    *len = read_file(path, g_src, sizeof g_src);
    if (*len < 0) {
        fprintf(stderr, "%s: cannot read: %s\n", path, strerror(errno));
        return -1;
    }
    e = glyph_load(&g_vm, g_src, (size_t)*len, &d);
    if (e) {
        if (d.line > 0) fprintf(stderr, "%s:%d:%d: error %s: %s\n", path, d.line, d.col, glyph_code_name(d.code), d.msg);
        else fprintf(stderr, "%s: error %s: %s\n", path, glyph_code_name(d.code), d.msg);
        return 1;
    }
    return 0;
}

static int cmd_check(int argc, char **argv)
{
    int i, bad = 0;
    long len;
    if (argc < 1) { fprintf(stderr, "usage: glyph check FILE...\n"); return 2; }
    for (i = 0; i < argc; i++) {
        int e = load_or_report(argv[i], &len);
        if (e) { bad = 1; continue; }
        printf("%s: ok (%d instructions, %d procedures, %d handlers, %d components, %d globals, %d arrays, screen %dx%d)\n",
               argv[i], g_vm.n_code - 1, g_vm.n_procs, g_vm.n_handlers, g_vm.n_comps, g_vm.n_state,
               g_vm.n_arrays, g_vm.w, g_vm.h);
    }
    return bad;
}

/* Strip a leading "--budget N" option. Returns -1 on a bad value. */
static int take_budget(int *argc, char ***argv)
{
    char *end;
    unsigned long n;
    if (*argc < 2 || strcmp((*argv)[0], "--budget") != 0) return 0;
    n = strtoul((*argv)[1], &end, 10);
    if (*end || n == 0 || n > GLYPH_MAX_STEPS) {
        fprintf(stderr, "--budget must be 1..%d\n", GLYPH_MAX_STEPS);
        return -1;
    }
    glyph_set_budget(&g_vm, (uint32_t)n);
    *argc -= 2; *argv += 2;
    return 0;
}

static int cmd_run(int argc, char **argv)
{
    glyph_backend be;
    long len;
    int e;
    if (take_budget(&argc, &argv)) return 2;
    if (argc != 1) { fprintf(stderr, "usage: glyph run [--budget N] FILE\n"); return 2; }
    if (load_or_report(argv[0], &len)) return 1;
    if (term_open() != 0) {
        fprintf(stderr, "glyph run needs a terminal; use `glyph replay FILE` for headless output\n");
        return 2;
    }
    term_backend(&be);
    e = glyph_run_interactive(&g_vm, &be, 0);
    term_close();
    if (e) {
        const glyph_diag *d = glyph_last_diag(&g_vm);
        fprintf(stderr, "%s:%d:%d: start failed %s: %s\n", argv[0], d->line, d->col,
                glyph_code_name(d->code), d->msg);
    }
    return e ? 1 : 0;
}

static int transcript_for(const char *src, long srclen, const char *script, size_t scriptlen)
{
    glyph_buf b;
    int e;
    glyph_buf_init(&b, g_out, sizeof g_out);
    e = glyph_transcript(&g_vm, src, (size_t)srclen, script, scriptlen, &b);
    if (b.truncated) fprintf(stderr, "warning: transcript truncated\n");
    return e;
}

static int cmd_replay(int argc, char **argv)
{
    long len, slen;
    const char *script = default_script;
    int e;
    if (take_budget(&argc, &argv)) return 2;
    if (argc < 1 || argc > 2) { fprintf(stderr, "usage: glyph replay [--budget N] FILE [EVENTS|-]\n"); return 2; }
    len = read_file(argv[0], g_src, sizeof g_src);
    if (len < 0) { fprintf(stderr, "%s: cannot read: %s\n", argv[0], strerror(errno)); return 1; }
    slen = (long)strlen(default_script);
    if (argc == 2) {
        slen = read_file(argv[1], g_script, sizeof g_script);
        if (slen < 0) { fprintf(stderr, "%s: cannot read: %s\n", argv[1], strerror(errno)); return 1; }
        script = g_script;
    }
    e = transcript_for(g_src, len, script, (size_t)slen);
    fputs(g_out, stdout);
    return e ? 1 : 0;
}

/* ---- conformance runner --------------------------------------------------- */
static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void first_diff(const char *want, const char *got)
{
    int line = 1;
    while (*want && *want == *got) { if (*want == '\n') line++; want++; got++; }
    while (want > g_expect && want[-1] != '\n') want--;
    while (got > g_out && got[-1] != '\n') got--;
    fprintf(stderr, "    first difference at line %d\n    expected: %.*s\n    actual:   %.*s\n", line,
            (int)strcspn(want, "\n"), want, (int)strcspn(got, "\n"), got);
}

static int cmd_test(int argc, char **argv)
{
    const char *dir = "tests/corpus";
    int update = 0, i, n = 0, pass = 0, fail = 0;
    char *names[512];
    DIR *dp;
    struct dirent *de;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--update") == 0) update = 1;
        else dir = argv[i];
    }
    dp = opendir(dir);
    if (!dp) { fprintf(stderr, "%s: %s\n", dir, strerror(errno)); return 2; }
    while ((de = readdir(dp)) != NULL && n < 512) {
        size_t l = strlen(de->d_name);
        if (l > 6 && strcmp(de->d_name + l - 6, ".glyph") == 0) names[n++] = strdup(de->d_name);
    }
    closedir(dp);
    qsort(names, (size_t)n, sizeof names[0], cmp_names);

    for (i = 0; i < n; i++) {
        char path[1024], base[512];
        long len, slen, elen;
        const char *script = default_script;
        snprintf(base, sizeof base, "%.*s", (int)(strlen(names[i]) - 6), names[i]);
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        len = read_file(path, g_src, sizeof g_src);
        snprintf(path, sizeof path, "%s/%s.events", dir, base);
        slen = read_file(path, g_script, sizeof g_script);
        if (slen >= 0) script = g_script; else slen = (long)strlen(default_script);
        if (len < 0) { printf("FAIL %s (unreadable)\n", base); fail++; continue; }
        transcript_for(g_src, len, script, (size_t)slen);
        snprintf(path, sizeof path, "%s/%s.expect", dir, base);
        if (update) {
            FILE *f = fopen(path, "wb");
            if (!f || fputs(g_out, f) < 0) { printf("FAIL %s (cannot write)\n", base); fail++; }
            else { printf("UPDATED %s\n", base); pass++; }
            if (f) fclose(f);
            continue;
        }
        elen = read_file(path, g_expect, sizeof g_expect - 1);
        if (elen < 0) { printf("FAIL %s (no .expect)\n", base); fail++; continue; }
        g_expect[elen] = 0;
        if (strcmp(g_expect, g_out) == 0) { printf("PASS %s\n", base); pass++; }
        else { printf("FAIL %s\n", base); first_diff(g_expect, g_out); fail++; }
    }
    for (i = 0; i < n; i++) free(names[i]);
    printf("%d passed, %d failed (%s)\n", pass, fail, dir);
    return (fail || n == 0) ? 1 : 0;
}

/* ---- bytecode dump --------------------------------------------------------- */
static const char *unit_label(int u)
{
    static char buf[48];
    const glyph_unit *un = &g_vm.units[u];
    if (un->sym == 0xFFFF) return "main";
    snprintf(buf, sizeof buf, "%s %s", (g_vm.syms[un->sym].flags & GLYPH_SYM_PROC) &&
             g_vm.syms[un->sym].proc_unit == u ? "TO" : "ON", g_vm.syms[un->sym].name);
    return buf;
}

static void dump_insn(int pc)
{
    const glyph_insn *in = &g_vm.code[pc];
    printf("%5d  %4u:%-3u %-12s", pc, in->line, in->col, glyph_op_name(in->op));
    switch (in->op) {
    case OP_PUSH_INT: printf(" %d", in->arg); break;
    case OP_PUSH_STR: printf(" \"%s\"", &g_vm.strpool[STR_OFF(in->arg)]); break;
    case OP_LOAD_G: case OP_STORE_G: case OP_ADDV_G:
        printf(" %s (global %d)", g_vm.syms[g_vm.state[in->arg].sym].name, in->arg); break;
    case OP_LOAD_L: case OP_STORE_L: case OP_ADDV_L:
        printf(" %s (local %d)", g_vm.syms[in->arg >> 8].name, in->arg & 0xFF); break;
    case OP_ITEM: case OP_SETITEM:
        printf(" %s", g_vm.syms[g_vm.arrays[in->arg].sym].name); break;
    case OP_CALL: case OP_CALLV: case OP_ENABLE: case OP_DISABLE:
        printf(" %s", g_vm.syms[in->arg].name); break;
    case OP_JMP: case OP_IF_JZ: case OP_WHILE_JZ: printf(" -> %d", in->arg); break;
    case OP_REP_INIT: printf(" counter %d", in->arg); break;
    case OP_REP_NEXT: printf(" counter %d, exit -> %d", in->arg >> 16, in->arg & 0xFFFF); break;
    default: break;
    }
    printf("\n");
}

static int cmd_dump(int argc, char **argv)
{
    long len;
    int i, u;
    static const char *kinds[] = { "?", "PANEL", "LABEL", "BUTTON", "FIELD" };
    if (argc != 1) { fprintf(stderr, "usage: glyph dump FILE\n"); return 2; }
    if (load_or_report(argv[0], &len)) return 1;
    printf("screen %dx%d, %d instructions, %d names, %u pool bytes, vm %zu bytes\n", g_vm.w, g_vm.h,
           g_vm.n_code - 1, g_vm.n_syms, g_vm.strpool_len, glyph_vm_size());
    for (u = 0; u < g_vm.n_units; u++)
        printf("unit %d %-20s entry %4d  inputs %d  locals %d\n", u, unit_label(u), g_vm.units[u].entry,
               g_vm.units[u].nparams, g_vm.units[u].nlocals);
    printf("code:\n");
    for (i = 1; i < g_vm.n_code; i++) {
        for (u = 0; u < g_vm.n_units; u++)
            if (g_vm.units[u].entry == i && u > 0) printf("  ; %s\n", unit_label(u));
        dump_insn(i);
    }
    for (i = 0; i < g_vm.n_comps; i++) {
        const glyph_comp *c = &g_vm.comps[i];
        printf("component %d %s %s at %d,%d size %dx%d", i, kinds[c->type], g_vm.syms[c->sym].name,
               c->x, c->y, c->w, c->h);
        if (c->type == GLYPH_COMP_BUTTON || c->type == GLYPH_COMP_FIELD)
            printf(" action %s", g_vm.syms[c->action].name);
        printf("\n");
    }
    for (i = 0; i < g_vm.n_state; i++)
        printf("global %d %s\n", i, g_vm.syms[g_vm.state[i].sym].name);
    for (i = 0; i < g_vm.n_arrays; i++)
        printf("array %d %s[%d]\n", i, g_vm.syms[g_vm.arrays[i].sym].name, g_vm.arrays[i].size);
    return 0;
}

static char g_extracted[GLYPH_MAX_SOURCE + 2];

static int cmd_extract(int argc, char **argv)
{
    glyph_diag d;
    size_t n = 0;
    long len;
    int e;
    if (argc != 1) { fprintf(stderr, "usage: glyph extract FILE|-\n"); return 2; }
    len = read_file(argv[0], g_src, sizeof g_src);
    if (len < 0) { fprintf(stderr, "%s: cannot read: %s\n", argv[0], strerror(errno)); return 2; }
    e = glyph_extract(&g_vm, g_src, (size_t)len, g_extracted, sizeof g_extracted, &n, &d);
    fwrite(g_extracted, 1, n, stdout);
    if (e) {
        if (d.line > 0) fprintf(stderr, "extracted:%d:%d: error %s: %s\n", d.line, d.col, glyph_code_name(d.code), d.msg);
        else fprintf(stderr, "error %s: %s\n", glyph_code_name(d.code), d.msg);
        return 1;
    }
    return 0;
}

static int cmd_limits(void)
{
    printf("glyph %s\n", GLYPH_VERSION);
    printf("source bytes      %d\n", GLYPH_MAX_SOURCE);
    printf("tokens            %d\n", GLYPH_MAX_TOKENS);
    printf("nesting depth     %d\n", GLYPH_MAX_NEST);
    printf("instructions      %d\n", GLYPH_MAX_CODE - 1);
    printf("expression depth  %d\n", GLYPH_MAX_EXPR_DEPTH);
    printf("names             %d (length %d)\n", GLYPH_MAX_NAMES, GLYPH_MAX_NAME_LEN);
    printf("string length     %d (pool %d bytes)\n", GLYPH_MAX_STR_LEN, GLYPH_MAX_STRPOOL);
    printf("procedures        %d\n", GLYPH_MAX_PROCS);
    printf("handlers          %d\n", GLYPH_MAX_HANDLERS);
    printf("components        %d\n", GLYPH_MAX_COMPONENTS);
    printf("state variables   %d\n", GLYPH_MAX_STATE);
    printf("call depth        %d\n", GLYPH_MAX_CALL_DEPTH);
    printf("local slots       %d (%d per procedure)\n", GLYPH_MAX_LOCALS, GLYPH_MAX_UNIT_LOCALS);
    printf("value stack       %d\n", GLYPH_MAX_VSTACK);
    printf("arrays            %d (%d cells)\n", GLYPH_MAX_ARRAYS, GLYPH_MAX_ARRAY_CELLS);
    printf("repeat count      %d\n", GLYPH_MAX_REPEAT);
    printf("steps per entry   %d default, %d maximum\n", GLYPH_DEFAULT_STEPS, GLYPH_MAX_STEPS);
    printf("screen            %dx%d\n", GLYPH_MAX_W, GLYPH_MAX_H);
    printf("sizeof(glyph_vm)  %zu bytes\n", glyph_vm_size());
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "glyph %s\n"
            "usage:\n"
            "  glyph run [--budget N] FILE   interactive session (TAB, ENTER, SPACE, ESC to exit)\n"
            "  glyph check FILE...           load and validate\n"
            "  glyph replay [--budget N] FILE [EVENTS|-]  run an event script, print the transcript\n"
            "  glyph test [--update] [DIR]   run the conformance corpus (default tests/corpus)\n"
            "  glyph dump FILE               print the compiled bytecode\n"
            "  glyph extract FILE|-          pull a Glyph program out of model output\n"
            "  glyph limits                  print compiled-in limits\n",
            GLYPH_VERSION);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    if (strcmp(argv[1], "run") == 0) return cmd_run(argc - 2, argv + 2);
    if (strcmp(argv[1], "check") == 0) return cmd_check(argc - 2, argv + 2);
    if (strcmp(argv[1], "replay") == 0) return cmd_replay(argc - 2, argv + 2);
    if (strcmp(argv[1], "test") == 0) return cmd_test(argc - 2, argv + 2);
    if (strcmp(argv[1], "dump") == 0) return cmd_dump(argc - 2, argv + 2);
    if (strcmp(argv[1], "extract") == 0) return cmd_extract(argc - 2, argv + 2);
    if (strcmp(argv[1], "limits") == 0) return cmd_limits();
    if (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0) {
        printf("glyph %s\n", GLYPH_VERSION);
        return 0;
    }
    usage();
    return 2;
}

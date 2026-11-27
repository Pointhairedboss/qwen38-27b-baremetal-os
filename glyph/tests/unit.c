/* unit.c — unit tests for the Glyph shared core.
 *
 * Expected values are derived by hand from docs/SPEC.md (positions, cells,
 * step counts), not captured from the implementation. Each section matches a
 * required test category. Build and run: `make unit`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glyph.h"
#include "glyph_backend.h"

static glyph_vm vm;
static glyph_diag diag;
static glyph_result res;
static int failures, checks;
static const char *current;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, current, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); checks++; \
    if (a_ != b_) { failures++; printf("  FAIL %s:%d [%s] %s == %lld, expected %lld\n", \
    __FILE__, __LINE__, current, #a, a_, b_); } } while (0)
#define CHECK_CODE(a, code) do { int a_ = (a); checks++; if (a_ != (code)) { failures++; \
    printf("  FAIL %s:%d [%s] got %s (%s), expected %s\n", __FILE__, __LINE__, current, \
    glyph_code_name(a_), diag.msg[0] ? diag.msg : res.diag.msg, glyph_code_name(code)); } } while (0)
#define TEST(name) static void name(void)
#define RUN(name) do { current = #name; name(); } while (0)

/* ---- helpers ---------------------------------------------------------------- */
static int load(const char *src) { return glyph_load(&vm, src, strlen(src), &diag); }

static int run(const char *src)
{
    int e = load(src);
    if (e) return e;
    return glyph_start(&vm, &res);
}

static char grid[GLYPH_MAX_H][GLYPH_MAX_W + 1];
static unsigned char gattr[GLYPH_MAX_H][GLYPH_MAX_W];

static void grab_row(void *u, int y, const glyph_cell *cells, int w)
{
    int x;
    (void)u;
    for (x = 0; x < w; x++) { grid[y][x] = (char)GLYPH_CELL_CH(cells[x]); gattr[y][x] = GLYPH_CELL_ATTR(cells[x]); }
    grid[y][w] = 0;
}

static void render(void) { memset(grid, 0, sizeof grid); glyph_render(&vm, grab_row, 0); }

/* Rendered text of row y from column x, length n. */
static int row_is(int y, int x, const char *s) { render(); return strncmp(&grid[y][x], s, strlen(s)) == 0; }

static char canvas_ch(int x, int y) { return (char)GLYPH_CELL_CH(glyph_canvas_cell(&vm, x, y)); }

static int sint(const char *name) { return glyph_state_int(&vm, glyph_state_find(&vm, name)); }
static const char *sstr(const char *name) { return glyph_state_str(&vm, glyph_state_find(&vm, name)); }

static void turtle(int *x, int *y, int *h, int *p) { glyph_turtle(&vm, x, y, h, p); }

static int count_canvas(char ch)
{
    int x, y, n = 0;
    for (y = 0; y < glyph_height(&vm); y++)
        for (x = 0; x < glyph_width(&vm); x++) n += canvas_ch(x, y) == ch;
    return n;
}

static char big[GLYPH_MAX_SOURCE + 64];

static const char *repeat_src(const char *unit, int times, const char *tail)
{
    size_t u = strlen(unit), i;
    big[0] = 0;
    for (i = 0; i < (size_t)times && (i + 1) * u < sizeof big - 64; i++) memcpy(big + i * u, unit, u + 1);
    strcat(big, tail);
    return big;
}

/* ---- 1. lexer and parser ------------------------------------------------- */
TEST(lex_case_and_comments)
{
    int x, y, h, p;
    CHECK_CODE(run("; a comment\nfd 3 ; trailing\n  Rt 90\n"), GLYPH_OK);
    turtle(&x, &y, &h, &p);
    CHECK_EQ(y, 9); CHECK_EQ(h, 90);
}

TEST(lex_numbers)
{
    int x, y, h, p;
    CHECK_CODE(run("FD -5"), GLYPH_OK);
    turtle(&x, &y, &h, &p);
    CHECK_EQ(y, 17);
    CHECK_CODE(run("FD - 5"), GLYPH_OK);                 /* unary minus on an expression */
    turtle(&x, &y, &h, &p);
    CHECK_EQ(y, 17);
    CHECK_CODE(load("FD 1000000000"), GLYPH_E_NUMBER);
    CHECK_CODE(load("SET x 999999999"), GLYPH_OK);
    CHECK_CODE(load("FD 12abc"), GLYPH_E_NUMBER);
}

TEST(lex_strings_and_names)
{
    char s[128];
    CHECK_CODE(load("TEXT 0 0 \"abc"), GLYPH_E_STRING);
    CHECK_CODE(load("TEXT 0 0 \"ab\ncd\""), GLYPH_E_STRING);
    memset(s, 0, sizeof s);
    strcpy(s, "TEXT 0 0 \""); memset(s + 10, 'a', 63); strcat(s, "\"");
    CHECK_CODE(load(s), GLYPH_OK);
    strcpy(s, "TEXT 0 0 \""); memset(s + 10, 'a', 64); s[74] = 0; strcat(s, "\"");
    CHECK_CODE(load(s), GLYPH_E_STR_LEN);
    CHECK_CODE(load("SET abcdefghijklmno 1"), GLYPH_OK);
    CHECK_CODE(load("SET abcdefghijklmnop 1"), GLYPH_E_NAME_LEN);
    CHECK_CODE(load("SET x :"), GLYPH_E_SYNTAX);
}

TEST(lex_bad_characters_and_positions)
{
    CHECK_CODE(load("FD 1\nRT 2 @"), GLYPH_E_CHAR);
    CHECK_EQ(diag.line, 2); CHECK_EQ(diag.col, 6);
    CHECK_CODE(load("TEXT 0 0 \"caf\xc3\xa9\""), GLYPH_E_CHAR);
    CHECK_CODE(load("FD 1 ; caf\xc3\xa9 \xe2\x80\x94 ok in a comment\nFD 2"), GLYPH_OK);
    CHECK_CODE(load("FD 1 ; bell \x07 is not"), GLYPH_E_CHAR);
    {
        static const char nul[] = { 'F', 'D', ' ', '1', 0, 'P', 'U' };
        CHECK_CODE(glyph_load(&vm, nul, sizeof nul, &diag), GLYPH_E_CHAR);
    }
}

TEST(parse_structure_errors)
{
    CHECK_CODE(load("FD 1 ]"), GLYPH_E_SYNTAX);
    CHECK_CODE(load("REPEAT 2 [FD 1\nRT 3"), GLYPH_E_UNCLOSED);
    CHECK_EQ(diag.line, 1); CHECK_EQ(diag.col, 10);
    CHECK_CODE(load("TO sq FD 1"), GLYPH_E_UNCLOSED);
    CHECK_CODE(load("END"), GLYPH_E_SYNTAX);
    CHECK_CODE(load("REPEAT 2 [BUTTON b 0 0 \"x\" a]"), GLYPH_E_TOPLEVEL);
    CHECK_CODE(load("TO a TO b END END"), GLYPH_E_TOPLEVEL);
    CHECK_CODE(run("FD \"far\""), GLYPH_E_TYPE);      /* inputs are expressions, typed at run time */
    CHECK_CODE(load("FD"), GLYPH_E_SYNTAX);
    CHECK_CODE(load("SET fd 1"), GLYPH_E_SYNTAX);
    CHECK_CODE(load("REPEAT 3"), GLYPH_E_SYNTAX);
    CHECK_CODE(load("5"), GLYPH_E_SYNTAX);
}

TEST(parse_link_errors)
{
    CHECK_CODE(load("FD 1\n  wiggle"), GLYPH_E_UNKNOWN);
    CHECK_EQ(diag.line, 2); CHECK_EQ(diag.col, 3);
    CHECK_CODE(load("FD 1\nJUMP 5"), GLYPH_E_UNKNOWN);      /* not "expected a command" */
    CHECK_EQ(diag.line, 2); CHECK_EQ(diag.col, 1);
    CHECK_CODE(load("TO sq END sq 5"), GLYPH_E_SYNTAX);     /* procedures take no inputs */
    CHECK_CODE(load("FD :len"), GLYPH_E_UNDEFINED);
    CHECK_CODE(load("ADD n 1"), GLYPH_E_UNDEFINED);
    CHECK_CODE(load("ENABLE ghost"), GLYPH_E_UNDEFINED);
    CHECK_CODE(load("LABEL l 0 0 \"x\"\nDISABLE l"), GLYPH_E_KIND);
    CHECK_CODE(load("BUTTON b 0 0 \"x\" go"), GLYPH_E_UNDEFINED);
    /* earliest link error wins: line 2 before line 3 */
    CHECK_CODE(load("FD 1\nBUTTON b 0 0 \"x\" go\nFD :never"), GLYPH_E_UNDEFINED);
    CHECK_EQ(diag.line, 2);
    /* unknown commands are found while parsing, before linking */
    CHECK_CODE(load("FD 1\nBUTTON b 0 0 \"x\" go\nnope"), GLYPH_E_UNKNOWN);
    CHECK_EQ(diag.line, 3);
    /* forward reference to a procedure is fine */
    CHECK_CODE(load("sq\nTO sq FD 1 END"), GLYPH_OK);
}

TEST(parse_redefinitions)
{
    CHECK_CODE(load("TO a END TO a END"), GLYPH_E_REDEFINE);
    CHECK_CODE(load("TO fd END"), GLYPH_E_REDEFINE);
    CHECK_CODE(load("ON x PU ON x PD"), GLYPH_E_REDEFINE);
    CHECK_CODE(load("SCREEN 10 5 SCREEN 10 5"), GLYPH_E_REDEFINE);
    CHECK_CODE(load("LABEL a 0 0 1 LABEL a 1 1 2"), GLYPH_E_REDEFINE);
}

/* ---- 2. LOGO commands and turtle state --------------------------------------- */
TEST(turtle_home_and_moves)
{
    int x, y, h, p;
    CHECK_CODE(run(""), GLYPH_OK);
    turtle(&x, &y, &h, &p);
    CHECK_EQ(x, 40); CHECK_EQ(y, 12); CHECK_EQ(h, 0); CHECK_EQ(p, 1);

    CHECK_CODE(run("FD 5"), GLYPH_OK);
    turtle(&x, &y, &h, &p);
    CHECK_EQ(x, 40); CHECK_EQ(y, 7);
    CHECK(canvas_ch(40, 12) == '*' && canvas_ch(40, 8) == '*');
    CHECK(canvas_ch(40, 7) == ' ');                  /* final position not plotted */
    CHECK_EQ(count_canvas('*'), 5);

    CHECK_CODE(run("BK 3"), GLYPH_OK);
    turtle(&x, &y, &h, &p);
    CHECK_EQ(y, 15);
    CHECK(canvas_ch(40, 12) == '*' && canvas_ch(40, 14) == '*' && canvas_ch(40, 15) == ' ');

    CHECK_CODE(run("RT 90 FD 4"), GLYPH_OK);
    turtle(&x, &y, &h, &p);
    CHECK_EQ(x, 44); CHECK_EQ(y, 12); CHECK_EQ(h, 90);
    CHECK(canvas_ch(43, 12) == '*' && canvas_ch(44, 12) == ' ');
}

TEST(turtle_headings)
{
    int x, y, h, p;
    CHECK_CODE(run("LT 90"), GLYPH_OK);   turtle(&x, &y, &h, &p); CHECK_EQ(h, 270);
    CHECK_CODE(run("RT 450"), GLYPH_OK);  turtle(&x, &y, &h, &p); CHECK_EQ(h, 90);
    CHECK_CODE(run("LT -90"), GLYPH_OK);  turtle(&x, &y, &h, &p); CHECK_EQ(h, 90);
    CHECK_CODE(run("RT -30"), GLYPH_OK);  turtle(&x, &y, &h, &p); CHECK_EQ(h, 330);
    CHECK_CODE(run("RT 999999999"), GLYPH_OK); turtle(&x, &y, &h, &p); CHECK_EQ(h, 999999999 % 360);
    CHECK_CODE(run("RIGHT 10 LEFT 20"), GLYPH_OK); turtle(&x, &y, &h, &p); CHECK_EQ(h, 350);
}

TEST(turtle_pen)
{
    int x, y, h, p;
    CHECK_CODE(run("PU FD 3 PD FD 2"), GLYPH_OK);
    turtle(&x, &y, &h, &p);
    CHECK_EQ(y, 7); CHECK_EQ(p, 1);
    CHECK_EQ(count_canvas('*'), 2);
    CHECK(canvas_ch(40, 9) == '*' && canvas_ch(40, 8) == '*' && canvas_ch(40, 10) == ' ');
    CHECK_CODE(run("PENUP"), GLYPH_OK); turtle(&x, &y, &h, &p); CHECK_EQ(p, 0);
    CHECK_CODE(run("FD -2 FORWARD 0 BACK 0"), GLYPH_OK); turtle(&x, &y, &h, &p); CHECK_EQ(y, 14);
}

TEST(turtle_diagonal_fixed_point)
{
    /* 45 degrees: step = (46341, -46341)/65536. Hand-computed plots:
     * (40,12), (41,11), (41,11); end (42,10). */
    int x, y, h, p;
    CHECK_CODE(run("RT 45 FD 3"), GLYPH_OK);
    turtle(&x, &y, &h, &p);
    CHECK_EQ(x, 42); CHECK_EQ(y, 10);
    CHECK(canvas_ch(40, 12) == '*' && canvas_ch(41, 11) == '*');
    CHECK_EQ(count_canvas('*'), 2);
}

TEST(turtle_clearscreen_and_color)
{
    int x, y, h, p;
    CHECK_CODE(run("SETCOLOR 4 FD 1 RT 90 PU FD 3 CLEARSCREEN"), GLYPH_OK);
    turtle(&x, &y, &h, &p);
    CHECK_EQ(x, 40); CHECK_EQ(y, 12); CHECK_EQ(h, 0); CHECK_EQ(p, 1);
    CHECK_EQ(count_canvas('*'), 0);
    CHECK_CODE(run("SETCOLOR 4 FD 1"), GLYPH_OK);
    CHECK_EQ(GLYPH_CELL_ATTR(glyph_canvas_cell(&vm, 40, 12)), 0x04);
    CHECK_CODE(run("COLOR 14 1 FD 1 SETCOLOR 2 FD 1"), GLYPH_OK);
    CHECK_EQ(GLYPH_CELL_ATTR(glyph_canvas_cell(&vm, 40, 12)), 0x1E);
    CHECK_EQ(GLYPH_CELL_ATTR(glyph_canvas_cell(&vm, 40, 11)), 0x12);
    CHECK_CODE(run("COLOR 16 0"), GLYPH_E_RANGE);
    CHECK_CODE(run("COLOR 1 8"), GLYPH_E_RANGE);
}

TEST(turtle_clamp)
{
    int x, y, h, p;
    /* 5000 steps north, far beyond the 4096-cell clamp */
    CHECK_CODE(run("REPEAT 5 [FD 1000]"), GLYPH_OK);
    turtle(&x, &y, &h, &p);
    CHECK_EQ(y, -4096);
}

/* ---- 3. REPEAT and procedures --------------------------------------------- */
static void check_square(void)
{
    int x, y, h, p;
    turtle(&x, &y, &h, &p);
    CHECK_EQ(x, 40); CHECK_EQ(y, 12); CHECK_EQ(h, 0);
    CHECK_EQ(count_canvas('*'), 8);
    CHECK(canvas_ch(40, 10) == '*' && canvas_ch(42, 10) == '*' && canvas_ch(42, 12) == '*');
    CHECK(canvas_ch(41, 11) == ' ');
}

TEST(repeat_forms)
{
    int x, y, h, p;
    CHECK_CODE(run("REPEAT 4 [FD 2 RT 90]"), GLYPH_OK);
    check_square();
    /* 1 REPEAT + 4 iterations + 4 * (FD 1+2, RT 1) = 21 */
    CHECK_EQ(res.steps, 21);
    CHECK_CODE(run("REPEAT 0 [FD 5]"), GLYPH_OK);
    CHECK_EQ(count_canvas('*'), 0);
    CHECK_CODE(run("REPEAT 3 REPEAT 2 FD 1"), GLYPH_OK);
    turtle(&x, &y, &h, &p); CHECK_EQ(y, 6);
    CHECK_CODE(run("[FD 1 [FD 1]] REPEAT 2 []"), GLYPH_OK);
    turtle(&x, &y, &h, &p); CHECK_EQ(y, 10);
    CHECK_CODE(run("REPEAT -1 FD 1"), GLYPH_E_RANGE);
    CHECK_CODE(run("REPEAT 10001 PU"), GLYPH_E_RANGE);
}

TEST(procedures)
{
    int x, y, h, p;
    CHECK_CODE(run("TO sq [REPEAT 4 [FD 2 RT 90]] END\nsq"), GLYPH_OK);
    check_square();
    CHECK_CODE(run("TO side FD 2 RT 90 END TO sq REPEAT 4 side END sq"), GLYPH_OK);
    check_square();
    /* a procedure is only run when called */
    CHECK_CODE(run("TO up FD 5 END"), GLYPH_OK);
    turtle(&x, &y, &h, &p); CHECK_EQ(y, 12);
    /* bootLogo README example: three procedures, nested calls */
    CHECK_CODE(run("TO CURVE REPEAT 4 [LT 10 FD 10] END\n"
                   "TO PETAL [CURVE LT 140 CURVE] END\n"
                   "TO FLOWER REPEAT 4 [PETAL LT 50] END\n"
                   "FLOWER RT 180 FD 50 RT 140 PETAL PU FD 50\n"), GLYPH_OK);
    turtle(&x, &y, &h, &p);
    CHECK_EQ(p, 0);
    CHECK(count_canvas('*') > 50);
}

TEST(procedure_depth)
{
    CHECK_CODE(run("TO a a END a"), GLYPH_E_DEPTH);
    CHECK_CODE(run("TO a b END TO b a END a"), GLYPH_E_DEPTH);
    /* 16 nested calls are allowed */
    CHECK_CODE(run("TO p1 p2 END TO p2 p3 END TO p3 p4 END TO p4 p5 END TO p5 p6 END "
                   "TO p6 p7 END TO p7 p8 END TO p8 p9 END TO p9 p10 END TO p10 p11 END "
                   "TO p11 p12 END TO p12 p13 END TO p13 p14 END TO p14 p15 END "
                   "TO p15 p16 END TO p16 FD 1 END p1"), GLYPH_OK);
}

/* ---- 4. drawing ------------------------------------------------------------------ */
TEST(drawing_text_line)
{
    CHECK_CODE(run("SCREEN 20 6 TEXT 1 1 \"hi\" SET n -42 TEXT 5 1 :n TEXT 18 2 \"abcdef\""), GLYPH_OK);
    CHECK(row_is(1, 1, "hi  -42"));
    CHECK(row_is(2, 18, "ab"));
    CHECK_EQ(strlen(grid[2]), 20);
    CHECK_CODE(run("SCREEN 20 8 LINE 1 1 5 1 LINE 0 2 0 5 LINE 2 3 4 5 LINE 9 3 7 5 LINE 10 0 13 1"), GLYPH_OK);
    CHECK(row_is(1, 1, "-----"));
    CHECK(canvas_ch(0, 2) == '|' && canvas_ch(0, 5) == '|');
    CHECK(canvas_ch(2, 3) == '\\' && canvas_ch(3, 4) == '\\' && canvas_ch(4, 5) == '\\');
    CHECK(canvas_ch(9, 3) == '/' && canvas_ch(7, 5) == '/');
    /* Bresenham (10,0)-(13,1): 4 cells, '*' */
    CHECK_EQ(count_canvas('*'), 4);
    CHECK(canvas_ch(10, 0) == '*' && canvas_ch(13, 1) == '*');
}

TEST(drawing_rect_box_clear)
{
    int x, y, h, p;
    CHECK_CODE(run("SCREEN 10 6 COLOR 15 4 RECT -2 -2 4 4 BOX 3 1 4 3"), GLYPH_OK);
    CHECK_EQ(GLYPH_CELL_ATTR(glyph_canvas_cell(&vm, 0, 0)), 0x4F);
    CHECK_EQ(GLYPH_CELL_ATTR(glyph_canvas_cell(&vm, 1, 1)), 0x4F);
    CHECK_EQ(GLYPH_CELL_ATTR(glyph_canvas_cell(&vm, 2, 2)), 0x07);
    CHECK(row_is(1, 3, "+--+"));
    CHECK(row_is(2, 3, "|  |"));
    CHECK(row_is(3, 3, "+--+"));
    CHECK_CODE(run("SCREEN 10 6 FD 2 COLOR 0 2 CLEAR"), GLYPH_OK);
    turtle(&x, &y, &h, &p);
    CHECK_EQ(y, 1);
    CHECK_EQ(count_canvas('*'), 0);
    CHECK_EQ(GLYPH_CELL_ATTR(glyph_canvas_cell(&vm, 9, 5)), 0x20);
    CHECK_CODE(run("BOX 1 1 1 5"), GLYPH_E_RANGE);
    CHECK_CODE(run("TEXT 1001 0 \"x\""), GLYPH_E_RANGE);
    CHECK_CODE(run("LINE 0 0 0 -1001"), GLYPH_E_RANGE);
    CHECK_CODE(run("RECT 0 0 -1 3"), GLYPH_E_RANGE);
}

/* ---- 5. layout ---------------------------------------------------------------- */
TEST(layout_components)
{
    CHECK_CODE(run("SCREEN 30 6\n"
                   "PANEL p 0 0 30 6 \"Settings\"\n"
                   "LABEL l 2 1 \"Name:\"\n"
                   "FIELD name 8 1 6 save\n"
                   "BUTTON ok 2 3 \"OK\" save\n"
                   "BUTTON no 9 3 \"Cancel\" save\n"
                   "ON save PU\n"
                   "DISABLE no\n"), GLYPH_OK);
    render();
    CHECK(strcmp(grid[0], "+ Settings ------------------+") == 0);
    CHECK(strcmp(grid[1], "| Name: >______<             |") == 0);
    CHECK(strcmp(grid[3], "| [ OK ] ( Cancel )          |") == 0);
    CHECK(strcmp(grid[5], "+----------------------------+") == 0);
    CHECK_EQ(gattr[0][0], GLYPH_ATTR_PANEL);
    CHECK_EQ(gattr[0][2], GLYPH_ATTR_TITLE);
    CHECK_EQ(gattr[1][8], GLYPH_ATTR_FOCUS);
    CHECK_EQ(gattr[3][2], GLYPH_ATTR_CONTROL);
    CHECK_EQ(gattr[3][9], GLYPH_ATTR_DISABLED);
}

TEST(layout_title_truncation_and_clipping)
{
    CHECK_CODE(run("SCREEN 12 3 PANEL p 0 0 8 3 \"Very long title\" BUTTON b 9 1 \"Wide\" go ON go PU"), GLYPH_OK);
    render();
    CHECK(strncmp(grid[0], "+ Very +", 8) == 0);
    CHECK(strcmp(grid[1], "|      | > W") == 0);   /* button clipped at the right edge */
}

TEST(layout_errors)
{
    CHECK_CODE(load("SCREEN 81 10"), GLYPH_E_RANGE);
    CHECK_CODE(load("SCREEN 10 25"), GLYPH_E_RANGE);
    CHECK_CODE(load("SCREEN 10 5 LABEL l 10 0 \"x\""), GLYPH_E_RANGE);
    CHECK_CODE(load("LABEL l 0 -1 \"x\""), GLYPH_E_RANGE);
    CHECK_CODE(load("PANEL p 0 0 1 5 \"x\""), GLYPH_E_RANGE);
    CHECK_CODE(load("FIELD f 0 0 64 a ON a PU"), GLYPH_E_RANGE);
    CHECK_CODE(load("LABEL l :x 0 \"x\""), GLYPH_E_SYNTAX);
    CHECK_CODE(load("BUTTON b 0 0 OK go"), GLYPH_E_SYNTAX);
    CHECK_CODE(load("LABEL l 0 0 :missing"), GLYPH_E_UNDEFINED);
}

/* ---- 6. application state ---------------------------------------------------- */
TEST(state_set_add)
{
    CHECK_CODE(run("SET a 5 SET b \"hi\" SET c :a ADD c 10 ADD a -7"), GLYPH_OK);
    CHECK_EQ(sint("a"), -2);
    CHECK(strcmp(sstr("B"), "hi") == 0);
    CHECK_EQ(sint("c"), 15);
    CHECK_EQ(glyph_state_type(&vm, glyph_state_find(&vm, "b")), GLYPH_T_STR);
    CHECK_CODE(run("SET a 999999990 ADD a 100"), GLYPH_OK);
    CHECK_EQ(sint("a"), 999999999);
    CHECK_CODE(run("SET a -999999990 ADD a -100"), GLYPH_OK);
    CHECK_EQ(sint("a"), -999999999);
    CHECK_CODE(run("SET a 1 SET a \"x\" SET b 2 ADD b :a"), GLYPH_E_TYPE);
    CHECK_CODE(run("SET s \"x\" ADD s 1"), GLYPH_E_TYPE);
    CHECK_CODE(run("FD :n SET n 3"), GLYPH_E_UNSET);
    CHECK_CODE(run("ADD n 1 SET n 3"), GLYPH_E_UNSET);
    CHECK_CODE(run("SET s \"x\" FD :s"), GLYPH_E_TYPE);
    CHECK_EQ(glyph_state_find(&vm, "nothing"), -1);
}

TEST(state_change_tracking)
{
    CHECK_CODE(run("SET a 5 SET s \"x\" ON same [SET a 5 SET s \"x\"] ON diff [SET a 6] ON type [SET a \"5\"]"), GLYPH_OK);
    CHECK_EQ(res.changed, 2);
    CHECK_CODE(glyph_action(&vm, "same", &res), GLYPH_OK);
    CHECK_EQ(res.changed, 0);
    CHECK_CODE(glyph_action(&vm, "diff", &res), GLYPH_OK);
    CHECK_EQ(res.changed, 1);
    CHECK(glyph_state_dirty(&vm, glyph_state_find(&vm, "a")));
    CHECK_CODE(glyph_action(&vm, "type", &res), GLYPH_OK);
    CHECK_EQ(res.changed, 1);
}

TEST(state_limits)
{
    char src[2048];
    int i;
    src[0] = 0;
    for (i = 0; i < 32; i++) sprintf(src + strlen(src), "SET v%d %d\n", i, i);
    CHECK_CODE(run(src), GLYPH_OK);
    CHECK_EQ(glyph_state_count(&vm), 32);
    CHECK_EQ(sint("v31"), 31);
    strcat(src, "SET v32 0\n");
    CHECK_CODE(load(src), GLYPH_E_LIMIT);
}

/* ---- 7. buttons and fields ------------------------------------------------------- */
static const char *form =
    "SCREEN 40 6\n"
    "SET count 0\n"
    "LABEL hello 0 0 :name\n"
    "FIELD name 0 1 5 submit\n"
    "BUTTON inc 0 3 \"+\" up\n"
    "BUTTON off 6 3 \"off\" lock\n"
    "ON up ADD count 1\n"
    "ON submit SET count 100\n"
    "ON lock [DISABLE inc DISABLE off]\n";

TEST(focus_and_buttons)
{
    CHECK_CODE(run(form), GLYPH_OK);
    CHECK_EQ(glyph_focus(&vm), 1);                        /* the field */
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_TAB, &res), GLYPH_OK);
    CHECK_EQ(res.focus, 2);
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_LF, &res), GLYPH_OK);
    CHECK_EQ(sint("count"), 1);
    CHECK(strcmp(glyph_sym_name(&vm, res.action), "UP") == 0);
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_SPACE, &res), GLYPH_OK);
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_CR, &res), GLYPH_OK);
    CHECK_EQ(sint("count"), 3);
    CHECK_CODE(glyph_key(&vm, 'x', &res), GLYPH_IGNORED);  /* letters do nothing on a button */
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_TAB, &res), GLYPH_OK);
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_TAB, &res), GLYPH_OK);   /* wraps to the field */
    CHECK_EQ(res.focus, 1);
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_ESC, &res), GLYPH_IGNORED);
}

TEST(fields)
{
    CHECK_CODE(run(form), GLYPH_OK);
    CHECK(strcmp(sstr("name"), "") == 0);
    CHECK_CODE(glyph_key(&vm, 'B', &res), GLYPH_OK);
    CHECK_EQ(res.changed, 1);
    glyph_key(&vm, 'o', &res); glyph_key(&vm, ' ', &res); glyph_key(&vm, 'b', &res);
    glyph_key(&vm, '!', &res);
    CHECK(strcmp(sstr("name"), "Bo b!") == 0);
    CHECK_CODE(glyph_key(&vm, '?', &res), GLYPH_IGNORED);  /* width 5 reached */
    CHECK(row_is(0, 0, "Bo b!"));                       /* bound label */
    CHECK(row_is(1, 0, ">Bo b!<"));
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_BS, &res), GLYPH_OK);
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_DEL, &res), GLYPH_OK);
    CHECK(strcmp(sstr("name"), "Bo ") == 0);
    CHECK(row_is(1, 0, ">Bo __<"));
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_LF, &res), GLYPH_OK);
    CHECK_EQ(sint("count"), 100);
    glyph_key(&vm, GLYPH_KEY_BS, &res); glyph_key(&vm, GLYPH_KEY_BS, &res); glyph_key(&vm, GLYPH_KEY_BS, &res);
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_BS, &res), GLYPH_IGNORED);
    CHECK_CODE(glyph_key(&vm, 200, &res), GLYPH_IGNORED);
    CHECK_CODE(glyph_key(&vm, 7, &res), GLYPH_IGNORED);
}

TEST(field_holding_number)
{
    CHECK_CODE(run("FIELD f 0 0 6 go ON go PU SET f 42"), GLYPH_OK);
    CHECK(row_is(0, 0, ">42____<"));
    CHECK_CODE(glyph_key(&vm, '7', &res), GLYPH_OK);
    CHECK(strcmp(sstr("f"), "427") == 0);
    CHECK_EQ(glyph_state_type(&vm, glyph_state_find(&vm, "f")), GLYPH_T_STR);
}

TEST(clicks_and_disable)
{
    CHECK_CODE(run(form), GLYPH_OK);
    CHECK_CODE(glyph_click(&vm, "INC", &res), GLYPH_OK);
    CHECK_EQ(sint("count"), 1);
    CHECK_EQ(res.focus, 2);
    CHECK_CODE(glyph_click(&vm, "hello", &res), GLYPH_IGNORED);   /* label */
    CHECK_CODE(glyph_click(&vm, "nobody", &res), GLYPH_IGNORED);
    CHECK_CODE(glyph_click(&vm, 0, &res), GLYPH_IGNORED);
    CHECK_CODE(glyph_click(&vm, "name", &res), GLYPH_OK);
    CHECK_EQ(res.focus, 1);
    CHECK_CODE(glyph_click(&vm, "off", &res), GLYPH_OK);           /* disables inc and itself */
    CHECK_EQ(res.focus, 1);                                         /* moved to the field */
    CHECK(!glyph_comp_enabled(&vm, 2) && !glyph_comp_enabled(&vm, 3));
    CHECK_CODE(glyph_click(&vm, "inc", &res), GLYPH_IGNORED);
    CHECK(row_is(3, 0, "( + ) ( off )"));
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_TAB, &res), GLYPH_OK);    /* only the field is left */
    CHECK_EQ(res.focus, 1);
}

TEST(focus_none)
{
    CHECK_CODE(run("BUTTON a 0 0 \"a\" x ON x [DISABLE a] ON y [ENABLE a]"), GLYPH_OK);
    CHECK_EQ(glyph_focus(&vm), 0);
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_LF, &res), GLYPH_OK);
    CHECK_EQ(glyph_focus(&vm), -1);
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_LF, &res), GLYPH_IGNORED);
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_TAB, &res), GLYPH_IGNORED);
    CHECK_CODE(glyph_action(&vm, "y", &res), GLYPH_OK);
    CHECK_EQ(glyph_focus(&vm), -1);                     /* ENABLE does not move focus */
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_TAB, &res), GLYPH_OK);
    CHECK_EQ(glyph_focus(&vm), 0);
    CHECK_CODE(run("LABEL l 0 0 \"no controls\""), GLYPH_OK);
    CHECK_EQ(glyph_focus(&vm), -1);
}

/* ---- 8. event dispatch ----------------------------------------------------------- */
TEST(dispatch_semantics)
{
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_TAB, &res), res.status);  /* whatever state, no crash */
    glyph_init(&vm);
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_TAB, &res), GLYPH_E_STATE);
    CHECK_CODE(load("ON a PU"), GLYPH_OK);
    CHECK_CODE(glyph_action(&vm, "a", &res), GLYPH_E_STATE);      /* not started */
    CHECK_CODE(glyph_start(&vm, &res), GLYPH_OK);
    CHECK_CODE(glyph_start(&vm, &res), GLYPH_E_STATE);            /* only once */
    CHECK_CODE(glyph_action(&vm, "a", &res), GLYPH_OK);
    CHECK_EQ(res.steps, 1);
    CHECK_CODE(glyph_action(&vm, "b", &res), GLYPH_IGNORED);
    CHECK_CODE(glyph_action(&vm, "a_name_that_is_far_too_long", &res), GLYPH_IGNORED);
    CHECK_CODE(glyph_action(&vm, 0, &res), GLYPH_IGNORED);
}

TEST(dispatch_handler_error_keeps_session)
{
    CHECK_CODE(run("SET n 0 ON bad [ADD n 1 FD 5000] ON good ADD n 10"), GLYPH_OK);
    CHECK_CODE(glyph_action(&vm, "bad", &res), GLYPH_E_RANGE);
    CHECK_EQ(res.diag.line, 1);
    CHECK_EQ(sint("n"), 1);                              /* no rollback */
    CHECK_CODE(glyph_action(&vm, "good", &res), GLYPH_OK);
    CHECK_EQ(sint("n"), 11);
}

TEST(dispatch_start_error_faults)
{
    CHECK_CODE(run("SET n 0 ON a ADD n 1\nFD 9999"), GLYPH_E_RANGE);
    CHECK_EQ(res.diag.line, 2);
    CHECK_EQ(glyph_last_diag(&vm)->code, GLYPH_E_RANGE);
    CHECK_CODE(glyph_action(&vm, "a", &res), GLYPH_E_FAULTED);
    CHECK_CODE(glyph_key(&vm, GLYPH_KEY_TAB, &res), GLYPH_E_FAULTED);
    CHECK_EQ(sint("n"), 0);
}

TEST(dispatch_determinism)
{
    static char a[65536], b[65536];
    const char *src = "SET n 0 BUTTON x 0 0 \"x\" go ON go [ADD n 3 RT 30 FD 4]";
    const char *script = "key ENTER\nclick x\naction go\nframe\nstate\n";
    glyph_buf ba, bb;
    glyph_buf_init(&ba, a, sizeof a);
    glyph_buf_init(&bb, b, sizeof b);
    CHECK_CODE(glyph_transcript(&vm, src, strlen(src), script, strlen(script), &ba), GLYPH_OK);
    CHECK_CODE(glyph_transcript(&vm, src, strlen(src), script, strlen(script), &bb), GLYPH_OK);
    CHECK(strcmp(a, b) == 0);
    CHECK(strstr(a, "  N = 9\n") != 0);
}

/* ---- 9. malformed input ------------------------------------------------------------ */
TEST(malformed_inputs)
{
    CHECK_CODE(run(""), GLYPH_OK);
    CHECK_CODE(run("; only a comment"), GLYPH_OK);
    CHECK_CODE(glyph_load(&vm, 0, 0, &diag), GLYPH_OK);
    CHECK_CODE(glyph_load(&vm, 0, 5, &diag), GLYPH_E_ARG);
    CHECK_CODE(glyph_load(0, "FD 1", 4, &diag), GLYPH_E_ARG);
    CHECK_CODE(load("[[[[["), GLYPH_E_UNCLOSED);
    CHECK_CODE(load("]]]]"), GLYPH_E_SYNTAX);
    CHECK_CODE(load("REPEAT REPEAT REPEAT"), GLYPH_E_SYNTAX);
    CHECK_CODE(load("TO"), GLYPH_E_SYNTAX);
    CHECK_CODE(load("ON"), GLYPH_E_SYNTAX);
    CHECK_CODE(load("ON x"), GLYPH_E_SYNTAX);
    CHECK_CODE(load("PANEL"), GLYPH_E_SYNTAX);
    CHECK_CODE(load("\"unterminated"), GLYPH_E_STRING);
    /* failed load leaves an empty, usable vm */
    CHECK_CODE(glyph_start(&vm, &res), GLYPH_E_STATE);
    CHECK_CODE(run("FD 1"), GLYPH_OK);
}

/* ---- 10. resource limits ------------------------------------------------------------- */
TEST(limit_source_tokens_code)
{
    char *src = malloc(GLYPH_MAX_SOURCE + 2);
    memset(src, ' ', GLYPH_MAX_SOURCE + 1);
    CHECK_CODE(glyph_load(&vm, src, GLYPH_MAX_SOURCE, &diag), GLYPH_OK);
    CHECK_CODE(glyph_load(&vm, src, GLYPH_MAX_SOURCE + 1, &diag), GLYPH_E_SOURCE_SIZE);
    free(src);
    /* "[]" is two tokens and one instruction, so tokens run out first */
    CHECK_CODE(load(repeat_src("[] ", 2048, "")), GLYPH_OK);            /* 4096 tokens */
    CHECK_CODE(load(repeat_src("[] ", 2049, "")), GLYPH_E_TOKENS);
    /* 4094 PU + the final RET = 4095 instructions, the maximum */
    CHECK_CODE(load(repeat_src("PU ", GLYPH_MAX_CODE - 2, "")), GLYPH_OK);
    CHECK_CODE(load(repeat_src("PU ", GLYPH_MAX_CODE - 1, "")), GLYPH_E_CODE);
}

TEST(limit_nesting)
{
    char src[256];
    int i;
    src[0] = 0;
    for (i = 0; i < 16; i++) strcat(src, "[");
    strcat(src, "FD 1");
    for (i = 0; i < 16; i++) strcat(src, "]");
    CHECK_CODE(run(src), GLYPH_OK);
    src[0] = 0;
    for (i = 0; i < 17; i++) strcat(src, "[");
    CHECK_CODE(load(src), GLYPH_E_NEST);
    CHECK_CODE(load(repeat_src("REPEAT 1 ", 16, "FD 1")), GLYPH_OK);
    CHECK_CODE(load(repeat_src("REPEAT 1 ", 17, "FD 1")), GLYPH_E_NEST);
    CHECK_CODE(load(repeat_src("[", 5000, "")), GLYPH_E_NEST);
}

TEST(limit_tables)
{
    char src[8192];
    int i;
    src[0] = 0;
    for (i = 0; i < 129; i++) sprintf(src + strlen(src), "ENABLE n%d ", i);
    CHECK_CODE(load(src), GLYPH_E_NAMES);
    src[0] = 0;
    for (i = 0; i < 33; i++) sprintf(src + strlen(src), "TO p%d END ", i);
    CHECK_CODE(load(src), GLYPH_E_LIMIT);
    src[0] = 0;
    for (i = 0; i < 33; i++) sprintf(src + strlen(src), "ON h%d PU ", i);
    CHECK_CODE(load(src), GLYPH_E_LIMIT);
    src[0] = 0;
    for (i = 0; i < 33; i++) sprintf(src + strlen(src), "LABEL l%d 0 0 1 ", i);
    CHECK_CODE(load(src), GLYPH_E_LIMIT);
    CHECK_CODE(load(repeat_src("TEXT 0 0 \"123456789012345678901234567890123456789012345678901234567890123\"\n",
                               65, "")), GLYPH_E_STRPOOL);
}

/* ---- 11. execution budget ------------------------------------------------------------ */
TEST(budget)
{
    /* 1 + 10000 iterations + 10000 PU = 20001 steps */
    CHECK_CODE(run("REPEAT 10000 PU"), GLYPH_OK);
    CHECK_EQ(res.steps, 20001);
    CHECK_CODE(run("REPEAT 10000 [REPEAT 10000 []]"), GLYPH_E_BUDGET);
    CHECK_EQ(res.steps, GLYPH_DEFAULT_STEPS);
    CHECK_CODE(run("REPEAT 10000 [REPEAT 10000 [PU]]"), GLYPH_E_BUDGET);
    CHECK_CODE(run("REPEAT 200 [FD 1000]"), GLYPH_E_BUDGET);
    /* 1 + 100 * (1 iteration + 1 CLEAR + 1920 cells) = 192201 > 100000 */
    CHECK_CODE(run("REPEAT 100 [CLEAR]"), GLYPH_E_BUDGET);
}

TEST(budget_accounting)
{
    /* SCREEN 10 4: CLEAR costs 1 + 40 */
    CHECK_CODE(run("SCREEN 10 4 CLEAR"), GLYPH_OK);
    CHECK_EQ(res.steps, 41);
    CHECK_CODE(run("SCREEN 10 4 TEXT 0 0 \"abc\" LINE 0 0 3 0 RECT 0 0 2 2 BOX 0 0 3 3"), GLYPH_OK);
    /* 4 statements + 3 + 4 + 4 + 8 */
    CHECK_EQ(res.steps, 23);
    glyph_set_budget(&vm, 50);
    CHECK_CODE(load("FD 10 ON a FD 10"), GLYPH_OK);       /* budget survives load */
    CHECK_CODE(glyph_start(&vm, &res), GLYPH_OK);
    CHECK_CODE(glyph_action(&vm, "a", &res), GLYPH_OK);    /* fresh budget per entry */
    CHECK_CODE(glyph_action(&vm, "a", &res), GLYPH_OK);
    glyph_set_budget(&vm, 10);
    CHECK_CODE(glyph_action(&vm, "a", &res), GLYPH_E_BUDGET);
    glyph_set_budget(&vm, 0);                              /* 0 = default maximum */
    CHECK_CODE(run("REPEAT 10000 PU"), GLYPH_OK);
}

/* ---- 12. Turing-complete LOGO (v0.2) ------------------------------------------- */
TEST(tc_minus_rule)
{
    int x, y, h, p;
    CHECK_CODE(run("SCREEN 20 6 PU SETXY 10 -5"), GLYPH_OK);      /* two inputs */
    turtle(&x, &y, &h, &p);
    CHECK_EQ(x, 10); CHECK_EQ(y, -5);
    CHECK_CODE(run("SET a 7 SET b :a-5 SET c :a - 5 SET d 3*-2"), GLYPH_OK);
    CHECK_EQ(sint("b"), 2); CHECK_EQ(sint("c"), 2); CHECK_EQ(sint("d"), -6);
    CHECK_CODE(load("SET a 7 FD :a -5"), GLYPH_E_SYNTAX);          /* -5 starts a new input */
}

TEST(tc_procedures_and_output)
{
    CHECK_CODE(run("TO sq :n OUTPUT :n * :n END SET a sq sq 3"), GLYPH_OK);
    CHECK_EQ(sint("a"), 81);
    CHECK_CODE(run("TO five OUTPUT 5 END five SET a 1"), GLYPH_OK);   /* output discarded */
    CHECK_CODE(run("TO f :n IF :n > 0 [OUTPUT 1] END SET a f 0"), GLYPH_E_TYPE);
    CHECK_CODE(run("SET a 1 STOP SET a 2"), GLYPH_OK);                 /* STOP ends main */
    CHECK_EQ(sint("a"), 1);
    CHECK_CODE(run("IF 1 = 1 [SET a 1] [SET a 2]"), GLYPH_OK);         /* else list */
    CHECK_EQ(sint("a"), 1);
    CHECK_CODE(run("SET a 0 TO up ADD a 1 END REPEAT 3 up"), GLYPH_OK); /* global from a procedure */
    CHECK_EQ(sint("a"), 3);
    CHECK_CODE(load("TO f :x END SET y :x"), GLYPH_E_UNDEFINED);        /* inputs are not global */
}

TEST(tc_recursion_values)
{
    CHECK_CODE(run("TO fact :n IF :n = 0 [OUTPUT 1] OUTPUT :n * fact :n - 1 END SET a fact 10"), GLYPH_OK);
    CHECK_EQ(sint("a"), 3628800);
    CHECK_CODE(run("TO fib :n IF :n < 2 [OUTPUT :n] OUTPUT (fib :n - 1) + (fib :n - 2) END SET a fib 20"), GLYPH_OK);
    CHECK_EQ(sint("a"), 6765);
    /* 64 nested calls are allowed, the 65th is not */
    CHECK_CODE(run("TO d :n IF :n = 0 [OUTPUT 0] OUTPUT 1 + d :n - 1 END SET a d 63"), GLYPH_OK);
    CHECK_EQ(sint("a"), 63);
    CHECK_CODE(run("TO d :n IF :n = 0 [OUTPUT 0] OUTPUT 1 + d :n - 1 END SET a d 64"), GLYPH_E_DEPTH);
}

TEST(tc_arrays)
{
    CHECK_CODE(run("ARRAY v 5 SETITEM v 1 10 SETITEM v 5 50 SET s (ITEM v 1) + ITEM v 5"), GLYPH_OK);
    CHECK_EQ(sint("s"), 60);
    /* UCBLogo rule: a prefix reporter's input is a whole expression, so
     * this is ITEM v (1 + ITEM v 5) = ITEM v 51 */
    CHECK_CODE(run("ARRAY v 5 SETITEM v 5 50 SET s ITEM v 1 + ITEM v 5"), GLYPH_E_RANGE);
    CHECK_EQ(glyph_array_count(&vm), 1);
    CHECK_EQ(glyph_array_size(&vm, 0), 5);
    CHECK_EQ(glyph_array_item(&vm, glyph_array_find(&vm, "v"), 5), 50);
    CHECK_EQ(glyph_array_item(&vm, 0, 6), 0);
    CHECK(strcmp(glyph_array_name(&vm, 0), "V") == 0);
    CHECK_CODE(run("ARRAY v 5 SET s ITEM v 0"), GLYPH_E_RANGE);
    CHECK_CODE(run("ARRAY v 5 SETITEM v 1 \"x\""), GLYPH_E_TYPE);
    CHECK_CODE(load("ARRAY v 2049"), GLYPH_E_RANGE);
    CHECK_CODE(load("ARRAY v 2000 ARRAY w 49"), GLYPH_E_LIMIT);
    CHECK_CODE(load("ARRAY v 3 ARRAY v 3"), GLYPH_E_REDEFINE);
    CHECK_CODE(load("SET s ITEM nothing 1"), GLYPH_E_UNDEFINED);
}

TEST(tc_budget_ceiling)
{
    /* 1 + 2 * 300000 = 600001 steps: too many for the default budget */
    const char *loop = "SET i 0 WHILE :i < 300000 [ADD i 1]";
    CHECK_CODE(run(loop), GLYPH_E_BUDGET);
    glyph_set_budget(&vm, 1000000);
    CHECK_CODE(run(loop), GLYPH_OK);
    CHECK_EQ(res.steps, 600002);
    CHECK_EQ(sint("i"), 300000);
    glyph_set_budget(&vm, 4000000000u);                  /* clamped to the ceiling */
    CHECK_CODE(run("SET i 0 WHILE 1 [ADD i 1]"), GLYPH_E_BUDGET);
    CHECK_EQ(res.steps, GLYPH_MAX_STEPS);
    glyph_set_budget(&vm, 0);                            /* back to the default */
    CHECK_CODE(run(loop), GLYPH_E_BUDGET);
}

/* ---- shared runner over a scripted backend --------------------------------------------- */
typedef struct fake { const int *keys; int n, pos, rows, frames; char status[96]; } fake;
static int f_poll(void *u) { fake *f = u; return f->pos < f->n ? f->keys[f->pos++] : GLYPH_KEY_EOF; }
static void f_row(void *u, int y, const glyph_cell *c, int w) { fake *f = u; (void)y; (void)c; (void)w; f->rows++; }
static void f_begin(void *u, int w, int h) { fake *f = u; (void)w; (void)h; f->frames++; }
static void f_status(void *u, const char *t, int n) { fake *f = u; memcpy(f->status, t, (size_t)n); f->status[n] = 0; }

TEST(runner_loop)
{
    static const int keys[] = { GLYPH_KEY_TAB, GLYPH_KEY_NONE, GLYPH_KEY_LF, GLYPH_KEY_ESC, GLYPH_KEY_LF };
    fake f;
    glyph_backend be;
    memset(&f, 0, sizeof f); memset(&be, 0, sizeof be);
    f.keys = keys; f.n = 5;
    be.user = &f; be.poll_key = f_poll; be.draw_row = f_row; be.begin_frame = f_begin; be.status = f_status;
    CHECK_CODE(load("SCREEN 10 3 SET n 0 BUTTON a 0 0 \"a\" x BUTTON b 0 1 \"b\" y ON x ADD n 1 ON y ADD n 10"), GLYPH_OK);
    CHECK_CODE(glyph_run_interactive(&vm, &be, 0), GLYPH_OK);
    CHECK_EQ(f.frames, 3);                   /* start, TAB, ENTER; ESC exits */
    CHECK_EQ(f.rows, 9);
    CHECK_EQ(sint("n"), 10);
    CHECK(strncmp(f.status, "Y: ok", 5) == 0);
    CHECK_CODE(load("FD 5000"), GLYPH_OK);
    f.pos = 0; f.frames = 0;
    CHECK_CODE(glyph_run_interactive(&vm, &be, 0), GLYPH_E_RANGE);
    CHECK(strncmp(f.status, "start: error E_RANGE 1:1", 24) == 0);
    CHECK_CODE(glyph_run_interactive(&vm, 0, 0), GLYPH_E_ARG);
}

TEST(transcript_script_errors)
{
    static char out[8192];
    glyph_buf b;
    const char *src = "PU";
    glyph_buf_init(&b, out, sizeof out);
    CHECK_CODE(glyph_transcript(&vm, src, 2, "key NOPE\n", 9, &b), GLYPH_E_SYNTAX);
    CHECK(strstr(out, "script error: bad key") != 0);
    glyph_buf_init(&b, out, sizeof out);
    CHECK_CODE(glyph_transcript(&vm, src, 2, "dance\n", 6, &b), GLYPH_E_SYNTAX);
    glyph_buf_init(&b, out, 40);
    CHECK_CODE(glyph_transcript(&vm, src, 2, "frame\n", 6, &b), GLYPH_OK);
    CHECK(b.truncated && strlen(out) == 39);
    CHECK_CODE(glyph_transcript(0, src, 2, "", 0, &b), GLYPH_E_ARG);
}

int main(void)
{
    printf("sizeof(glyph_vm) = %zu bytes\n", glyph_vm_size());
    glyph_init(&vm);
    RUN(lex_case_and_comments);
    RUN(lex_numbers);
    RUN(lex_strings_and_names);
    RUN(lex_bad_characters_and_positions);
    RUN(parse_structure_errors);
    RUN(parse_link_errors);
    RUN(parse_redefinitions);
    RUN(turtle_home_and_moves);
    RUN(turtle_headings);
    RUN(turtle_pen);
    RUN(turtle_diagonal_fixed_point);
    RUN(turtle_clearscreen_and_color);
    RUN(turtle_clamp);
    RUN(repeat_forms);
    RUN(procedures);
    RUN(procedure_depth);
    RUN(drawing_text_line);
    RUN(drawing_rect_box_clear);
    RUN(layout_components);
    RUN(layout_title_truncation_and_clipping);
    RUN(layout_errors);
    RUN(state_set_add);
    RUN(state_change_tracking);
    RUN(state_limits);
    RUN(focus_and_buttons);
    RUN(fields);
    RUN(field_holding_number);
    RUN(clicks_and_disable);
    RUN(focus_none);
    RUN(dispatch_semantics);
    RUN(dispatch_handler_error_keeps_session);
    RUN(dispatch_start_error_faults);
    RUN(dispatch_determinism);
    RUN(malformed_inputs);
    RUN(limit_source_tokens_code);
    RUN(limit_nesting);
    RUN(limit_tables);
    RUN(budget);
    RUN(budget_accounting);
    RUN(tc_minus_rule);
    RUN(tc_procedures_and_output);
    RUN(tc_recursion_values);
    RUN(tc_arrays);
    RUN(tc_budget_ceiling);
    RUN(runner_loop);
    RUN(transcript_script_errors);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

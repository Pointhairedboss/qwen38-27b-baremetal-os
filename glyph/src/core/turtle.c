/* turtle.c — canvas and LOGO turtle state.
 *
 * Integer-only geometry: positions are 16.16 fixed point in cell units and
 * directions come from a one-entry-per-degree sine table (round(sin*65536)).
 * Each primitive takes `avail` (remaining step budget) and returns its cost
 * in steps; when the cost exceeds `avail` it draws nothing, so the caller can
 * fail the entry with E_BUDGET deterministically.
 */
#include "glyph_internal.h"

static const int32_t sin_tab[91] = {
    0, 1144, 2287, 3430, 4572, 5712, 6850, 7987, 9121, 10252, 11380, 12505, 13626,
    14742, 15855, 16962, 18064, 19161, 20252, 21336, 22415, 23486, 24550, 25607,
    26656, 27697, 28729, 29753, 30767, 31772, 32768, 33754, 34729, 35693, 36647,
    37590, 38521, 39441, 40348, 41243, 42126, 42995, 43852, 44695, 45525, 46341,
    47143, 47930, 48703, 49461, 50203, 50931, 51643, 52339, 53020, 53684, 54332,
    54963, 55578, 56175, 56756, 57319, 57865, 58393, 58903, 59396, 59870, 60326,
    60764, 61183, 61584, 61966, 62328, 62672, 62997, 63303, 63589, 63856, 64104,
    64332, 64540, 64729, 64898, 65048, 65177, 65287, 65376, 65446, 65496, 65526,
    65536
};

int32_t turtle_sin(int32_t deg)
{
    int32_t d = deg % 360;
    if (d < 0) d += 360;
    if (d <= 90) return sin_tab[d];
    if (d <= 180) return sin_tab[180 - d];
    if (d <= 270) return -sin_tab[d - 180];
    return -sin_tab[360 - d];
}

/* floor((f + 0.5 cell) / 1 cell), valid for negative values too. */
int canvas_round(int32_t f)
{
    int64_t v = (int64_t)f + 32768;
    if (v >= 0) return (int)(v / 65536);
    return (int)(-((-v + 65535) / 65536));
}

void canvas_put(glyph_vm *vm, int x, int y, char ch, uint8_t attr)
{
    if (x < 0 || y < 0 || x >= vm->w || y >= vm->h) return;
    vm->canvas[y * GLYPH_MAX_W + x] = GLYPH_CELL(ch, attr);
}

void canvas_reset(glyph_vm *vm)
{
    int i;
    for (i = 0; i < GLYPH_MAX_W * GLYPH_MAX_H; i++)
        vm->canvas[i] = GLYPH_CELL(' ', GLYPH_ATTR_DEFAULT);
    vm->attr = GLYPH_ATTR_DEFAULT;
}

void turtle_home(glyph_vm *vm)
{
    vm->tx = (int32_t)(vm->w / 2) * 65536;
    vm->ty = (int32_t)(vm->h / 2) * 65536;
    vm->heading = 0;
    vm->pen = 1;
}

static int32_t clamp_pos(int32_t p)
{
    const int32_t lim = (int32_t)GLYPH_TURTLE_LIMIT * 65536;
    if (p > lim) return lim;
    if (p < -lim) return -lim;
    return p;
}

int turtle_move(glyph_vm *vm, int32_t dist, uint32_t avail, uint32_t *used)
{
    uint32_t steps = (uint32_t)(dist < 0 ? -dist : dist);
    int32_t dx = turtle_sin(vm->heading);
    int32_t dy = -turtle_sin(vm->heading + 90);
    uint32_t i;
    *used = steps;
    if (steps > avail) return -1;
    if (dist < 0) { dx = -dx; dy = -dy; }
    for (i = 0; i < steps; i++) {
        if (vm->pen) canvas_put(vm, canvas_round(vm->tx), canvas_round(vm->ty), '*', vm->attr);
        vm->tx = clamp_pos(vm->tx + dx);
        vm->ty = clamp_pos(vm->ty + dy);
    }
    return 0;
}

uint32_t canvas_text(glyph_vm *vm, int x, int y, const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++) canvas_put(vm, x + i, y, s[i], vm->attr);
    return (uint32_t)n;
}

static int iabs(int v) { return v < 0 ? -v : v; }

uint32_t canvas_line(glyph_vm *vm, int x1, int y1, int x2, int y2, uint32_t avail)
{
    int dx = iabs(x2 - x1), dy = -iabs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    uint32_t cost = (uint32_t)((dx > -dy ? dx : -dy) + 1);
    char ch;
    if (cost > avail) return cost;
    if (dy == 0) ch = '-';
    else if (dx == 0) ch = '|';
    else if (dx == -dy) ch = (sx == sy) ? '\\' : '/';
    else ch = '*';
    for (;;) {
        int e2;
        canvas_put(vm, x1, y1, ch, vm->attr);
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
    return cost;
}

uint32_t canvas_rect(glyph_vm *vm, int x, int y, int w, int h, uint32_t avail)
{
    uint32_t cost = (uint32_t)w * (uint32_t)h;
    int i, j;
    if (cost > avail) return cost;
    for (j = (y < 0 ? 0 : y); j < y + h && j < vm->h; j++)
        for (i = (x < 0 ? 0 : x); i < x + w && i < vm->w; i++)
            canvas_put(vm, i, j, ' ', vm->attr);
    return cost;
}

uint32_t canvas_box(glyph_vm *vm, int x, int y, int w, int h, uint32_t avail)
{
    uint32_t cost = (uint32_t)(2 * w + 2 * (h - 2));
    int i;
    if (cost > avail) return cost;
    for (i = 1; i < w - 1; i++) {
        canvas_put(vm, x + i, y, '-', vm->attr);
        canvas_put(vm, x + i, y + h - 1, '-', vm->attr);
    }
    for (i = 1; i < h - 1; i++) {
        canvas_put(vm, x, y + i, '|', vm->attr);
        canvas_put(vm, x + w - 1, y + i, '|', vm->attr);
    }
    canvas_put(vm, x, y, '+', vm->attr);
    canvas_put(vm, x + w - 1, y, '+', vm->attr);
    canvas_put(vm, x, y + h - 1, '+', vm->attr);
    canvas_put(vm, x + w - 1, y + h - 1, '+', vm->attr);
    return cost;
}

uint32_t canvas_clear(glyph_vm *vm)
{
    int x, y;
    for (y = 0; y < vm->h; y++)
        for (x = 0; x < vm->w; x++)
            vm->canvas[y * GLYPH_MAX_W + x] = GLYPH_CELL(' ', vm->attr);
    return (uint32_t)vm->w * vm->h;
}

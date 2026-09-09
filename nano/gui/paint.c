/* paint.c - Paint, for fingers.
 *
 * A canvas of its own, a rail of brushes down the left, colours and two
 * sliders along the bottom.  A finger has no pressure, so the brushes
 * take their life from how fast it moves, how their marks are spaced and
 * what grain they carry: ink that thins with speed, a soft round, a
 * chisel marker that follows the stroke's direction, an airbrush of
 * scattered dots, a neon line with a glow, a grainy pencil, a wash that
 * builds up where strokes overlap, and an eraser.  Strokes are drawn
 * between the points the screen reports, so a quick finger still leaves
 * one line.  Undo keeps the last few strokes; Save writes a BMP that
 * Pictures can open.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"

#define PANEL       0x1A140D
#define PANEL_LIT   0x2A2114
#define EDGE        0x4A3618
#define AMBER       0xF0A020
#define AMBER_HOT   0xFFC65A
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68

#define RAIL_W      84
#define FOOT_H      92
#define UNDO_LEVELS 4
#define NSWATCH     12

enum { B_INK, B_SOFT, B_MARKER, B_SPRAY, B_NEON, B_PENCIL, B_WASH, B_ERASER, B_COUNT };
static const char *brush_names[B_COUNT] = { "Ink", "Soft", "Marker", "Spray", "Neon", "Pencil", "Wash", "Eraser" };

static const uint32_t swatches[NSWATCH] = {
    0x1A140D, 0xFFFFFF, 0xF0A020, 0xF0602A, 0xD8322A, 0xC79BE8,
    0x6FB7E8, 0x2C6FB0, 0x9BD27A, 0x2E8B57, 0xF4D35E, 0x8A5E16
};

static int win_id = -1;
static uint32_t *canvas, *undo[UNDO_LEVELS];
static int undo_n;                       /* how many undo levels hold something */
static int cw, ch;                       /* the canvas, in pixels */
static int brush = B_INK, size = 18, opacity = 100, color_idx = 2;
static uint32_t color = 0xF0A020, paper = 0xF4EEE2;
static int paper_idx;
static const uint32_t papers[3] = { 0xF4EEE2, 0x0E0B08, 0x1A140D };

static int stroking, last_x, last_y;
static unsigned last_ms;
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1;   /* what a stroke touched, canvas coords */
static char status[64];
static unsigned rng = 0x2545F491u;

static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* ------------------------------------------------------------ the canvas */
static void touch_dirty(int x, int y, int r)
{
    if (x - r < dirty_x0) dirty_x0 = x - r;
    if (y - r < dirty_y0) dirty_y0 = y - r;
    if (x + r + 1 > dirty_x1) dirty_x1 = x + r + 1;
    if (y + r + 1 > dirty_y1) dirty_y1 = y + r + 1;
}

static void blend(int x, int y, uint32_t c, int a)
{
    uint32_t *p;
    if (x < 0 || y < 0 || x >= cw || y >= ch || a <= 0) return;
    p = canvas + (size_t)y * cw + x;
    *p = a >= 255 ? c : mix(*p, c, a);
}

/* a round dab: alpha falls from the centre by the softness (0 hard, 255 fully soft) */
static void dab(int x, int y, int r, uint32_t c, int alpha, int soft)
{
    int dx, dy, r2 = r * r;
    if (r < 1) r = 1;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy, a;
            if (d2 > r2) continue;
            if (soft) {
                /* linear fall-off from the core outwards, the core as wide as the hardness */
                int d = 0;
                while ((d + 1) * (d + 1) <= d2) d++;
                a = alpha - alpha * soft / 255 * d / r;
                if (a < 0) a = 0;
            } else {
                a = (d2 > (r - 1) * (r - 1)) ? alpha / 2 : alpha;   /* one softened edge */
            }
            blend(x + dx, y + dy, c, a);
        }
    touch_dirty(x, y, r);
}

static void chisel(int x, int y, int dx, int dy, int len, int th, uint32_t c, int alpha)
{
    /* a slanted mark across the stroke's direction: the normal, in 1/256ths */
    int l = 1, nx, ny, i;
    while (l * l < dx * dx + dy * dy) l++;
    nx = -dy * 256 / l;
    ny = dx * 256 / l;
    for (i = -len; i <= len; i++)
        dab(x + nx * i / 256, y + ny * i / 256, th, c, alpha, 0);
}

static void spray(int x, int y, int r, uint32_t c, int alpha)
{
    int i, n = r * 3 + 8;
    for (i = 0; i < n; i++) {
        int dx = (int)(rnd() % (2 * r + 1)) - r, dy = (int)(rnd() % (2 * r + 1)) - r;
        if (dx * dx + dy * dy <= r * r) blend(x + dx, y + dy, c, alpha / 3 + (int)(rnd() % 20));
    }
    touch_dirty(x, y, r);
}

static void pencil(int x, int y, int r, uint32_t c, int alpha)
{
    int dx, dy;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            if ((rnd() & 3) == 0) continue;                 /* the grain */
            blend(x + dx, y + dy, c, alpha * (60 + (int)(rnd() % 60)) / 200);
        }
    touch_dirty(x, y, r);
}

/* one mark of the current brush, with the stroke's direction and speed */
static void mark(int x, int y, int dx, int dy, int speed)
{
    int a = opacity * 255 / 100, r = size / 2;
    switch (brush) {
    case B_INK: {                                            /* thins as the finger speeds up */
        int rr = r - r * (speed > 40 ? 40 : speed) / 70;
        dab(x, y, rr < 2 ? 2 : rr, color, a, 0);
        break;
    }
    case B_SOFT:   dab(x, y, r, color, a * 2 / 3, 220); break;
    case B_MARKER: chisel(x, y, dx, dy, r, r / 3 + 1, color, a / 3); break;
    case B_SPRAY:  spray(x, y, r + r / 2, color, a); break;
    case B_NEON:
        dab(x, y, r + r / 2 + 3, color, a / 4, 255);
        dab(x, y, r / 3 + 1, mix(color, 0xFFFFFF, 170), a, 0);
        break;
    case B_PENCIL: pencil(x, y, r / 2 + 1, color, a); break;
    case B_WASH:   dab(x, y, r + r / 2, color, a / 10 + 4, 200); break;
    case B_ERASER: dab(x, y, r, paper, 255, 0); break;
    }
}

static int spacing(void)
{
    switch (brush) {
    case B_SPRAY: return size / 3 + 2;
    case B_WASH:  return size / 4 + 2;
    case B_MARKER: return 2;
    default:      return size / 6 + 1;
    }
}

/* marks laid from the last point to this one */
static void stroke_to(int x, int y)
{
    int dx = x - last_x, dy = y - last_y, dist = 0, i, n, sp = spacing();
    unsigned now = now_ms(), dt = now - last_ms;
    int speed;
    while ((dist + 1) * (dist + 1) <= dx * dx + dy * dy) dist++;
    speed = dt ? dist * 16 / (int)(dt > 200 ? 200 : dt) : 0;    /* pixels per 16 ms */
    n = dist / sp;
    if (n < 1) n = 1;
    for (i = 1; i <= n; i++)
        mark(last_x + dx * i / n, last_y + dy * i / n, dx, dy, speed);
    last_x = x;
    last_y = y;
    last_ms = now;
}

static void push_undo(void)
{
    uint32_t *oldest;
    int i;
    if (!undo[0]) return;
    oldest = undo[UNDO_LEVELS - 1];
    for (i = UNDO_LEVELS - 1; i > 0; i--) undo[i] = undo[i - 1];
    undo[0] = oldest;
    memcpy(undo[0], canvas, (size_t)cw * ch * 4);
    if (undo_n < UNDO_LEVELS) undo_n++;
}

static void pop_undo(void)
{
    uint32_t *top;
    int i;
    if (!undo_n) return;
    top = undo[0];
    memcpy(canvas, top, (size_t)cw * ch * 4);
    for (i = 0; i < UNDO_LEVELS - 1; i++) undo[i] = undo[i + 1];
    undo[UNDO_LEVELS - 1] = top;
    undo_n--;
}

static void clear_canvas(void)
{
    size_t i, n = (size_t)cw * ch;
    for (i = 0; i < n; i++) canvas[i] = paper;
}

/* ------------------------------------------------------------ colour */
/* hue 0..359, full saturation, value 0..255 */
static uint32_t hsv(int h, int v)
{
    int sector = (h / 60) % 6, f = (h % 60) * 255 / 60;
    int p = 0, q = v - v * f / 255, t = v * f / 255, r, g, b;
    switch (sector) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ------------------------------------------------------------ the window */
static int canvas_x(struct window *w) { return w->x + RAIL_W + 10; }
static int canvas_y(struct window *w) { return w->y + 10; }

static void brush_preview(int x, int y, int w, int h, int b, int lit)
{
    /* a short stroke of the brush on a small card */
    int i, cx = x + 12, cy = y + h / 2;
    uint32_t c = lit ? AMBER_HOT : TEXT;
    round_fill(x, y, w, h, 6, lit ? PANEL_LIT : PANEL);
    round_frame(x, y, w, h, 6, lit ? AMBER : EDGE);
    for (i = 0; i <= 8; i++) {
        int px = cx + i * (w - 24) / 8, py = cy - 6 + (i * (8 - i)) * 12 / 16 - 6;
        switch (b) {
        case B_INK:    soft_ellipse(px, py, 5 - i / 3, 5 - i / 3, c, 255); break;
        case B_SOFT:   soft_ellipse(px, py, 7, 7, c, 110); break;
        case B_MARKER: fill_alpha(px - 2, py - 6, 5, 12, c, 90); break;
        case B_SPRAY:  { int k; for (k = 0; k < 10; k++) pixel_blend(px + (int)(rnd() % 11) - 5, py + (int)(rnd() % 11) - 5, c, 120); } break;
        case B_NEON:   soft_ellipse(px, py, 7, 7, c, 60); soft_ellipse(px, py, 2, 2, 0xFFFFFF, 255); break;
        case B_PENCIL: { int k; for (k = 0; k < 6; k++) pixel_blend(px + (int)(rnd() % 5) - 2, py + (int)(rnd() % 5) - 2, c, 200); } break;
        case B_WASH:   soft_ellipse(px, py, 9, 9, c, 40); break;
        case B_ERASER: fill(px - 3, py - 3, 7, 7, lit ? 0x3A2A12 : 0x241A0E); break;
        }
    }
}

static void paint_draw(struct window *w)
{
    int cx0 = canvas_x(w), cy0 = canvas_y(w), i, y;
    char b[48];

    /* the rail */
    vgradient(w->x, w->y, RAIL_W, w->h, PANEL_LIT, PANEL);
    fill(w->x + RAIL_W - 1, w->y, 1, w->h, EDGE);
    for (i = 0; i < B_COUNT; i++) {
        int by = w->y + 10 + i * 58;
        brush_preview(w->x + 8, by, RAIL_W - 16, 40, i, i == brush);
        text(F_SMALL, w->x + RAIL_W / 2 - text_width(F_SMALL, brush_names[i]) / 2, by + 41, brush_names[i],
             i == brush ? AMBER_HOT : TEXT_DIM);
    }
    /* undo, clear, paper, save */
    {
        int by = w->y + w->h - 4 * 34 - 8;
        const char *labels[4] = { "Undo", "Clear", "Paper", "Save" };
        for (i = 0; i < 4; i++) {
            round_fill(w->x + 8, by + i * 34, RAIL_W - 16, 28, 4, 0x2A2114);
            round_frame(w->x + 8, by + i * 34, RAIL_W - 16, 28, 4, EDGE);
            text(F_SMALL, w->x + RAIL_W / 2 - text_width(F_SMALL, labels[i]) / 2, by + i * 34 + 6, labels[i], TEXT);
        }
    }

    /* the canvas */
    if (canvas) {
        int ox, oy, ow, oh;
        clip_get(&ox, &oy, &ow, &oh);
        for (y = 0; y < ch; y++) {
            int py = cy0 + y;
            int x0 = cx0 < ox ? ox : cx0, x1 = cx0 + cw > ox + ow ? ox + ow : cx0 + cw;
            if (py < oy || py >= oy + oh || x1 <= x0) continue;
            memcpy(back + (size_t)py * scr_w + x0, canvas + (size_t)y * cw + (x0 - cx0), (size_t)(x1 - x0) * 4);
        }
        damage(cx0, cy0, cw, ch);
    }
    round_frame(cx0 - 1, cy0 - 1, cw + 2, ch + 2, 2, EDGE);

    /* the foot: swatches, the hue strip, two sliders */
    y = w->y + w->h - FOOT_H;
    vgradient(w->x + RAIL_W, y, w->w - RAIL_W, FOOT_H, PANEL, 0x120E09);
    fill(w->x + RAIL_W, y, w->w - RAIL_W, 1, EDGE);
    for (i = 0; i < NSWATCH; i++) {
        int sx = cx0 + i * 34;
        round_fill(sx, y + 12, 28, 28, 5, swatches[i]);
        if (i == color_idx) round_frame(sx - 2, y + 10, 32, 32, 6, AMBER_HOT);
        else round_frame(sx, y + 12, 28, 28, 5, EDGE);
    }
    /* the hue strip, and the chosen colour large */
    for (i = 0; i < 180; i++) fill(cx0 + NSWATCH * 34 + 12 + i, y + 12, 1, 28, hsv(i * 2, 255));
    round_frame(cx0 + NSWATCH * 34 + 12, y + 12, 180, 28, 3, EDGE);
    round_fill(cx0 + NSWATCH * 34 + 204, y + 8, 36, 36, 6, color);
    round_frame(cx0 + NSWATCH * 34 + 204, y + 8, 36, 36, 6, AMBER_HOT);

    /* sliders: size and opacity */
    {
        int sx = cx0, sw = 220;
        text(F_SMALL, sx, y + 52, "Size", TEXT_DIM);
        round_fill(sx + 44, y + 60, sw, 6, 3, 0x0E0B08);
        round_fill(sx + 44, y + 60, (size - 2) * sw / 78, 6, 3, AMBER);
        soft_ellipse(sx + 44 + (size - 2) * sw / 78, y + 63, 7, 7, AMBER_HOT, 255);
        snprintf(b, sizeof b, "%d", size);
        text(F_SMALL, sx + 44 + sw + 10, y + 52, b, TEXT);

        sx = cx0 + 320;
        text(F_SMALL, sx, y + 52, "Opacity", TEXT_DIM);
        round_fill(sx + 60, y + 60, sw, 6, 3, 0x0E0B08);
        round_fill(sx + 60, y + 60, (opacity - 5) * sw / 95, 6, 3, AMBER);
        soft_ellipse(sx + 60 + (opacity - 5) * sw / 95, y + 63, 7, 7, AMBER_HOT, 255);
        snprintf(b, sizeof b, "%d%%", opacity);
        text(F_SMALL, sx + 60 + sw + 10, y + 52, b, TEXT);
    }
    text(F_SMALL, w->x + w->w - 14 - text_width(F_SMALL, status), y + 52, status, TEXT_DIM);
}

static void save_canvas(void)
{
    char name[32];
    int n, h;
    for (n = 1; n < 1000; n++) {
        snprintf(name, sizeof name, "\\PAINT%03d.PNG", n);
        h = sys_open(name);
        if (h < 0) break;
        sys_close(h);
    }
    if (png_write(name, canvas, cw, ch) == 0) snprintf(status, sizeof status, "saved %s", name);
    else strcpy(status, "could not save");
}

static void foot_click(struct window *w, int lx, int ly)
{
    int cx0 = RAIL_W + 10, y = w->h - FOOT_H, i;
    if (ly >= y + 8 && ly < y + 44) {
        for (i = 0; i < NSWATCH; i++)
            if (lx >= cx0 + i * 34 && lx < cx0 + i * 34 + 28) { color_idx = i; color = swatches[i]; return; }
        if (lx >= cx0 + NSWATCH * 34 + 12 && lx < cx0 + NSWATCH * 34 + 192) {
            color = hsv((lx - cx0 - NSWATCH * 34 - 12) * 2, 255);
            color_idx = -1;
            return;
        }
    }
    if (ly >= y + 50 && ly < y + 76) {
        if (lx >= cx0 + 44 && lx <= cx0 + 44 + 220) { size = 2 + (lx - cx0 - 44) * 78 / 220; return; }
        if (lx >= cx0 + 380 && lx <= cx0 + 380 + 220) { opacity = 5 + (lx - cx0 - 380) * 95 / 220; return; }
    }
}

static int paint_event(struct window *w, struct event *e)
{
    int cx0 = RAIL_W + 10, cy0 = 10;
    if (e->type == EV_MOUSE_DOWN) {
        int lx = e->a, ly = e->b;
        if (lx < RAIL_W) {                                       /* the rail */
            int by = w->h - 4 * 34 - 8;
            if (ly >= 10 && ly < 10 + B_COUNT * 58) { brush = (ly - 10) / 58; stroking = 0; return 1; }
            if (ly >= by && ly < by + 4 * 34) {
                switch ((ly - by) / 34) {
                case 0: pop_undo(); break;
                case 1: push_undo(); clear_canvas(); break;
                case 2: paper_idx = (paper_idx + 1) % 3; paper = papers[paper_idx]; push_undo(); clear_canvas(); break;
                case 3: save_canvas(); break;
                }
            }
            stroking = 0;
            return 1;
        }
        if (ly >= w->h - FOOT_H) { foot_click(w, lx, ly); stroking = 0; return 1; }
        if (lx >= cx0 && lx < cx0 + cw && ly >= cy0 && ly < cy0 + ch) {
            push_undo();
            stroking = 1;
            last_x = lx - cx0;
            last_y = ly - cy0;
            last_ms = now_ms();
            dirty_x0 = dirty_y0 = 1 << 30;
            dirty_x1 = dirty_y1 = -(1 << 30);
            mark(last_x, last_y, 0, 1, 0);
            /* Only if the mark touched something: the empty box is a pair of
               sentinels, and handing those to shell_repaint overflows into a
               nonsense rectangle that then becomes the clip. */
            if (dirty_x1 > dirty_x0)
                shell_repaint(w->x + cx0 + dirty_x0, w->y + cy0 + dirty_y0,
                              dirty_x1 - dirty_x0, dirty_y1 - dirty_y0);
            return 0;                                            /* the repaint is asked for above */
        }
        return 1;
    }
    if (e->type == EV_MOUSE_UP) { stroking = 0; return 0; }     /* the finger has lifted */
    if (e->type == EV_MOUSE_MOVE && stroking && (mouse_buttons & 1)) {
        int x = e->a - cx0, y = e->b - cy0;
        if (x < 0) x = 0; if (y < 0) y = 0;
        if (x >= cw) x = cw - 1; if (y >= ch) y = ch - 1;
        dirty_x0 = dirty_y0 = 1 << 30;
        dirty_x1 = dirty_y1 = -(1 << 30);
        stroke_to(x, y);
        if (dirty_x1 > dirty_x0)
            shell_repaint(w->x + cx0 + dirty_x0, w->y + cy0 + dirty_y0, dirty_x1 - dirty_x0, dirty_y1 - dirty_y0);
        return 0;
    }
    if (e->type == EV_KEY) {
        int k = e->a & ~K_SHIFT;
        if (e->b == 'u' || e->b == 'U') { pop_undo(); return 1; }
        if (e->b == '[' && size > 2) { size -= 2; return 1; }
        if (e->b == ']' && size < 80) { size += 2; return 1; }
        if (k >= 0x02 && k <= 0x09) { brush = k - 0x02; return 1; }   /* 1..8 pick a brush */
    }
    return 0;
}

void paint_closed(int id) { if (id == win_id) win_id = -1; }

void app_paint(void)
{
    int ww, wh, i;
    if (win_id >= 0) return;
    ww = scr_w * 86 / 100;
    wh = scr_h * 82 / 100;
    if (!canvas || cw != ww - RAIL_W - 20 || ch != wh - FOOT_H - 20) {
        if (canvas) free(canvas);
        for (i = 0; i < UNDO_LEVELS; i++) if (undo[i]) { free(undo[i]); undo[i] = 0; }
        cw = ww - RAIL_W - 20;
        ch = wh - FOOT_H - 20;
        canvas = malloc((size_t)cw * ch * 4);
        if (!canvas) return;
        for (i = 0; i < UNDO_LEVELS; i++) undo[i] = malloc((size_t)cw * ch * 4);
        undo_n = 0;
        clear_canvas();
    }
    status[0] = 0;
    win_id = win_open("Paint", ww, wh, paint_draw, paint_event);
    {
        struct window *w = win_at(win_id);
        w->x = (scr_w - ww) / 2;
        w->y = 40 + 30;
        w->fixed = 1;
    }
}

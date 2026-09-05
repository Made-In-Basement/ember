/* crystal.c - the ember crystal at the top of the screen, and its menu.
 *
 * The crystal is a faceted gem with an E cut into it.  Click it and it
 * breaks along its middle: the two halves slide apart and the menu falls
 * from the gap between them.  Click again and it closes back up.
 *
 * The gem is drawn, not pictured: a handful of polygons whose brightness
 * follows the facet's angle, so it catches the light differently on each
 * face and stays crisp at any size.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"

#define AMBER       0xF0A020
#define AMBER_HOT   0xFFC65A
#define AMBER_PALE  0xFFE0A8
#define AMBER_DEEP  0x8A5610
#define AMBER_DARK  0x4A2E08
#define PANEL       0x17120C
#define PANEL_HI    0x241C12
#define EDGE        0x3A2C18
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68

#define CRYSTAL_W   92          /* the whole gem */
#define CRYSTAL_H   86
#define MENU_W      260
#define SPLIT_GAP   10          /* clearance between a half and the menu */
/* each half ends up just clear of the menu, so the menu occupies the gap */
#define SPLIT_MAX   (MENU_W + SPLIT_GAP * 2)
#define ANIM_MS     300         /* how long the break takes */

int crystal_x, crystal_y = 6;

static int open_state;          /* 0 shut, 1 opening, 2 open, 3 closing */
static unsigned anim_start;
static int hovered;

static const char *items[] = {
    "Files", "Music", "Prompt", "Calculator", "Doom", "Help", "About",
    "Exit to DOS", 0
};
#define ROW_H    34
static int menu_sel = -1;

/* how far through the animation, 0..255 */
static int progress(void)
{
    unsigned dt;
    if (open_state == 0) return 0;
    if (open_state == 2) return 255;
    dt = now_ms() - anim_start;
    if (dt >= ANIM_MS) {
        open_state = (open_state == 1) ? 2 : 0;
        return open_state == 2 ? 255 : 0;
    }
    {
        int t = (int)(dt * 255 / ANIM_MS);
        /* ease out, so it arrives gently rather than stopping dead */
        t = 255 - ((255 - t) * (255 - t)) / 255;
        return open_state == 1 ? t : 255 - t;
    }
}

int crystal_busy(void)
{
    return open_state == 1 || open_state == 3;
}

int crystal_is_open(void)
{
    return open_state == 1 || open_state == 2;
}

/* ---------------------------------------------------------------- the gem */
/* One half of the gem: side = -1 for the left, +1 for the right.  The shape
   is a hexagon cut down the middle, so the two halves make a whole. */
static void draw_half(int cx, int cy, int side, int lift)
{
    int hw = CRYSTAL_W / 2, hh = CRYSTAL_H / 2;
    int x = cx + side * lift;
    int p[12];
    uint32_t face_top = mix(AMBER_HOT, AMBER_PALE, 90);
    uint32_t face_mid = AMBER;
    uint32_t face_low = AMBER_DEEP;

    /* The silhouette: flat along the seam, faceted and pointed outwards, so
       the two halves together make one gem and the break reads as a break. */
    p[0] = x;                       p[1] = cy - hh;         /* seam, top */
    p[2] = x + side * hw * 2 / 3;   p[3] = cy - hh * 3 / 4; /* shoulder */
    p[4] = x + side * hw;           p[5] = cy;              /* the outer point */
    p[6] = x + side * hw * 2 / 3;   p[7] = cy + hh * 3 / 4;
    p[8] = x;                       p[9] = cy + hh;         /* seam, bottom */
    poly_fill(p, 5, face_mid);

    /* the upper facet takes the light */
    p[0] = x;                       p[1] = cy - hh;
    p[2] = x + side * hw * 2 / 3;   p[3] = cy - hh * 3 / 4;
    p[4] = x + side * hw;           p[5] = cy;
    p[6] = x;                       p[7] = cy - hh / 6;
    poly_fill(p, 4, face_top);

    /* the lower one is in shade */
    p[0] = x;                       p[1] = cy + hh;
    p[2] = x + side * hw * 2 / 3;   p[3] = cy + hh * 3 / 4;
    p[4] = x + side * hw;           p[5] = cy;
    p[6] = x;                       p[7] = cy + hh / 6;
    poly_fill(p, 4, face_low);

    /* a bright cut along the seam, and a rim that catches the light */
    fill(x - (side < 0 ? 1 : 0), cy - hh, 1, CRYSTAL_H, AMBER_PALE);
    {
        int i;
        for (i = 0; i <= hh; i++) {             /* the two outer edges */
            int ex = x + side * (hw * 2 / 3 + (hw / 3) * i / hh);
            pixel_blend(ex, cy - hh * 3 / 4 + (hh * 3 / 4) * i / hh, AMBER_PALE, 150);
            pixel_blend(ex, cy + hh * 3 / 4 - (hh * 3 / 4) * i / hh, AMBER_PALE, 110);
        }
    }
}

/* the E, built from bars the way the logo is */
static void draw_E(int cx, int cy, int alpha)
{
    int w = 26, h = 34;
    int x = cx - w / 2, y = cy - h / 2;
    int bar = 7;
    fill_alpha(x, y, bar, h, AMBER_DARK, alpha);            /* the spine */
    fill_alpha(x, y, w, bar, AMBER_DARK, alpha);            /* top */
    fill_alpha(x, y + (h - bar) / 2, w - 5, bar, AMBER_DARK, alpha);
    fill_alpha(x, y + h - bar, w, bar, AMBER_DARK, alpha);  /* bottom */
}

/* ---------------------------------------------------------------- the menu */
static void draw_menu_panel(int t)
{
    int n = 0, i, h, mx, my, shown;
    while (items[n]) n++;
    h = n * ROW_H + 18;
    mx = crystal_x - MENU_W / 2;
    my = crystal_y + CRYSTAL_H - 4;
    shown = h * t / 255;                        /* it falls out of the gap */
    if (shown < 4) return;

    {
        int ox, oy, ow, oh;
        clip_get(&ox, &oy, &ow, &oh);
        clip_set(mx - 12, my, MENU_W + 24, shown);
        shadow(mx, my, MENU_W, h, 6, 7);
        round_fill(mx, my, MENU_W, h, 6, PANEL);
        vgradient(mx + 1, my + 1, MENU_W - 2, 40, PANEL_HI, PANEL);
        round_frame(mx, my, MENU_W, h, 6, AMBER_DEEP);
        for (i = 0; i < n; i++) {
            int ry = my + 10 + i * ROW_H;
            uint32_t c = (i == n - 1) ? TEXT_DIM : TEXT;
            if (i == menu_sel) {
                fill(mx + 4, ry - 2, MENU_W - 8, ROW_H, 0x2E2010);
                fill(mx + 4, ry - 2, 3, ROW_H, AMBER);
                c = AMBER_HOT;
            }
            text(F_NORMAL, mx + 24, ry + (ROW_H - text_height(F_NORMAL)) / 2 - 2,
                 items[i], c);
        }
        clip_set(ox, oy, ow, oh);
    }
}

void crystal_draw(void)
{
    int t = progress();
    int lift = SPLIT_MAX * t / 255 / 2;
    int cx = crystal_x, cy = crystal_y + CRYSTAL_H / 2;

    if (t > 0)
        draw_menu_panel(t);

    /* a warm pool of light behind the gem, like the artwork */
    soft_ellipse(cx, cy, 190, 118, AMBER, 42);
    soft_ellipse(cx, cy, 110, 76, AMBER_HOT, 34);

    draw_half(cx, cy, -1, lift);
    draw_half(cx, cy, +1, lift);
    if (t < 200)
        draw_E(cx, cy, 255 - t);
    if (hovered && t == 0)
        glow(cx - CRYSTAL_W / 2, cy - CRYSTAL_H / 2, CRYSTAL_W, CRYSTAL_H,
             8, AMBER_HOT, 3);
    damage(cx - CRYSTAL_W / 2 - SPLIT_MAX, crystal_y - 40,
           CRYSTAL_W + SPLIT_MAX * 2, CRYSTAL_H + 80);
}

/* which row is under a point, or -1 */
static int menu_hit(int x, int y)
{
    int n = 0, mx, my;
    while (items[n]) n++;
    mx = crystal_x - MENU_W / 2;
    my = crystal_y + CRYSTAL_H - 4;
    if (x < mx || x >= mx + MENU_W) return -1;
    if (y < my + 10 || y >= my + 10 + n * ROW_H) return -1;
    return (y - my - 10) / ROW_H;
}

int crystal_hit(int x, int y)
{
    return x >= crystal_x - CRYSTAL_W / 2 && x < crystal_x + CRYSTAL_W / 2 &&
           y >= crystal_y && y < crystal_y + CRYSTAL_H;
}

void crystal_toggle(void)
{
    anim_start = now_ms();
    open_state = crystal_is_open() ? 3 : 1;
    menu_sel = -1;
}

void crystal_close(void)
{
    if (crystal_is_open()) {
        anim_start = now_ms();
        open_state = 3;
    }
}

/* returns 1 if it swallowed the event */
int crystal_event(struct event *e)
{
    if (e->type == EV_MOUSE_MOVE) {
        int was = hovered, wsel = menu_sel;
        hovered = crystal_hit(e->a, e->b);
        menu_sel = crystal_is_open() ? menu_hit(e->a, e->b) : -1;
        return was != hovered || wsel != menu_sel;
    }
    if (e->type == EV_MOUSE_DOWN) {
        if (crystal_hit(e->a, e->b)) { crystal_toggle(); return 1; }
        if (crystal_is_open()) {
            int item = menu_hit(e->a, e->b);
            crystal_close();
            if (item >= 0) shell_run_menu(item);
            return 1;
        }
    }
    return 0;
}

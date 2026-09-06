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
#include "guiart.h"

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

#define CRYSTAL_W   (art_crystal.w)     /* the whole gem, as drawn */
#define CRYSTAL_H   (art_crystal.h)
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

/* how far down the screen the gem and its menu can reach */
int crystal_reach(void)
{
    int n = 0, h;
    while (items[n]) n++;
    h = n * ROW_H + 18;
    return crystal_y + CRYSTAL_H + h + 40;
}

/* the region the gem, its glow and its menu can touch */
void crystal_rect(int *x, int *y, int *w, int *h)
{
    int half = SPLIT_MAX / 2 + CRYSTAL_W + 48;
    *x = crystal_x - half;
    *y = 0;
    *w = half * 2;
    *h = crystal_reach();
}

int crystal_busy(void)
{
    return open_state == 1 || open_state == 3;
}

int crystal_is_open(void)
{
    return open_state == 1 || open_state == 2;
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

    {
        /* the two halves of the gem, drawn from the one picture so the
           break falls exactly where the artist put the middle */
        int half = art_crystal.w / 2;
        int top = crystal_y;
        image_draw_part(&art_crystal, 0, half, cx - half - lift, top);
        image_draw_part(&art_crystal, half, art_crystal.w - half,
                        cx + lift, top);
    }
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
    if (e->type == EV_KEY && crystal_is_open()) {
        int n = 0;
        while (items[n]) n++;
        if (e->a == K_DOWN) {
            menu_sel = (menu_sel + 1) % n;
            return 1;
        }
        if (e->a == K_UP) {
            menu_sel = (menu_sel <= 0 ? n : menu_sel) - 1;
            return 1;
        }
        if (e->a == K_ENTER) {
            int item = menu_sel;
            crystal_close();
            if (item >= 0) shell_run_menu(item);
            return 1;
        }
        if (e->a == K_ESC) { crystal_close(); return 1; }
        return 0;
    }
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

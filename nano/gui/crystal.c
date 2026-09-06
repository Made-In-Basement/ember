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
#define MENU_W      360
#define SPLIT_GAP   10          /* clearance between a half and the menu */
/* each half ends up just clear of the menu, so the menu occupies the gap */
#define SPLIT_MAX   (MENU_W + SPLIT_GAP * 2)
#define ANIM_MS     300         /* how long the break takes */

int crystal_x, crystal_y = 6;

static int open_state;          /* 0 shut, 1 opening, 2 open, 3 closing */
static unsigned anim_start;
static int hovered;

/* The menu in groups: an entry either does something or opens a group;
   the first entry of a group goes back.  Big rows, for fingers. */
struct entry { const char *label; int action, group; const struct image *icon; };
static const struct entry main_menu[] = {
    { "Files", A_FILES, -1, &art_folder }, { "Write", A_WRITE, -1, &art_note },
    { "Music", A_MUSIC, -1, &art_music }, { "Programs", -1, 1, &art_gear },
    { "System", -1, 2, &art_wrench }, { "Exit to DOS", A_EXIT, -1, &art_lock }, { 0, 0, 0, 0 }
};
static const struct entry programs_menu[] = {
    { "Back", -1, 0, 0 }, { "Prompt", A_PROMPT, -1, &art_terminal }, { "Calculator", A_CALC, -1, &art_calc },
    { "Pictures", A_VIEWER, -1, &art_paint }, { "Clock", A_CLOCK, -1, &art_clock },
    { "Calendar", A_CALENDAR, -1, &art_book }, { "Notes", A_NOTES, -1, &art_note },
    { "Screenshot", A_SHOT, -1, &art_computer }, { "Doom", A_DOOM, -1, &art_chip }, { 0, 0, 0, 0 }
};
static const struct entry system_menu[] = {
    { "Back", -1, 0, 0 }, { "Monitor", A_MONITOR, -1, &art_computer }, { "Keyboard", A_KEYBOARD, -1, &art_globe },
    { "Help", A_HELP, -1, &art_info }, { "About", A_ABOUT, -1, &art_crystal }, { 0, 0, 0, 0 }
};
static const struct entry *groups[] = { main_menu, programs_menu, system_menu };
static const struct entry *cur = main_menu;
static unsigned slide_ms;                       /* when the group last changed */
static int slide_dir;                           /* 1 deeper, -1 back */
#define ROW_H    50
#define SLIDE_MS 160
static int menu_sel = -1;

static void choose(int item)
{
    const struct entry *e = &cur[item];
    if (e->group >= 0) {                        /* into a group, or back out */
        slide_dir = e->group == 0 ? -1 : 1;
        cur = groups[e->group];
        menu_sel = -1;
        slide_ms = now_ms();
        return;
    }
    crystal_close();
    shell_run_menu(e->action);
}

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
    while (cur[n].label) n++;
    h = n * ROW_H + 20;
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
    return open_state == 1 || open_state == 3 ||
           (crystal_is_open() && now_ms() - slide_ms < SLIDE_MS);  /* a group sliding in */
}

int crystal_is_open(void)
{
    return open_state == 1 || open_state == 2;
}

/* ---------------------------------------------------------------- the menu */
static void draw_menu_panel(int t)
{
    int n = 0, i, h, mx, my, shown, slide = 0;
    unsigned since = now_ms() - slide_ms;
    while (cur[n].label) n++;
    h = n * ROW_H + 20;
    mx = crystal_x - MENU_W / 2;
    my = crystal_y + CRYSTAL_H - 4;
    shown = h * t / 255;                        /* it falls out of the gap */
    if (shown < 4) return;
    if (since < SLIDE_MS)                       /* the new group slides in from the side */
        slide = slide_dir * (int)((SLIDE_MS - since) * 48 / SLIDE_MS);

    {
        int ox, oy, ow, oh;
        clip_get(&ox, &oy, &ow, &oh);
        clip_shrink(mx - 12, my, MENU_W + 24, shown);
        shadow(mx, my, MENU_W, h, 8, 9);
        round_fill(mx, my, MENU_W, h, 8, PANEL);
        vgradient(mx + 1, my + 1, MENU_W - 2, 48, PANEL_HI, PANEL);
        round_frame(mx, my, MENU_W, h, 8, AMBER_DEEP);
        for (i = 0; i < n; i++) {
            int ry = my + 10 + i * ROW_H, rx = mx + slide;
            int back = (cur[i].group == 0), dim = (cur[i].action == A_EXIT);
            uint32_t c = dim ? TEXT_DIM : TEXT;
            if (i == menu_sel) {
                vgradient(rx + 6, ry, MENU_W - 12, ROW_H - 2, 0x3A2A12, 0x2A1E10);
                round_frame(rx + 6, ry, MENU_W - 12, ROW_H - 2, 5, AMBER_DEEP);
                fill(rx + 6, ry + 4, 3, ROW_H - 10, AMBER);
                c = AMBER_HOT;
            }
            if (cur[i].icon)
                image_draw_scaled(cur[i].icon, rx + 22, ry + (ROW_H - 2 - 30) / 2, 30, 30);
            if (back) {                         /* a chevron pointing back */
                int pts[6] = { rx + 40, ry + ROW_H / 2 - 10, rx + 28, ry + ROW_H / 2 - 1, rx + 40, ry + ROW_H / 2 + 8 };
                poly_fill(pts, 3, c);
            }
            text(F_BOLD, rx + 66, ry + (ROW_H - 2 - text_height(F_BOLD)) / 2, cur[i].label, c);
            if (cur[i].group > 0) {             /* it opens a group: a chevron onward */
                int cx = rx + MENU_W - 34, cy = ry + ROW_H / 2 - 1;
                int pts[6] = { cx - 6, cy - 9, cx + 4, cy, cx - 6, cy + 9 };
                poly_fill(pts, 3, i == menu_sel ? AMBER_HOT : AMBER_DEEP);
            }
            if (i < n - 1) fill(rx + 20, ry + ROW_H - 2, MENU_W - 40, 1, 0x231A10);
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
    while (cur[n].label) n++;
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
    cur = main_menu;
}

void crystal_close(void)
{
    if (crystal_is_open()) {
        anim_start = now_ms();
        open_state = 3;
    }
    cur = main_menu;                            /* next time, from the top */
}

/* returns 1 if it swallowed the event */
int crystal_event(struct event *e)
{
    if (e->type == EV_KEY && crystal_is_open()) {
        int n = 0;
        while (cur[n].label) n++;
        if (e->a == K_DOWN) {
            menu_sel = (menu_sel + 1) % n;
            return 1;
        }
        if (e->a == K_UP) {
            menu_sel = (menu_sel <= 0 ? n : menu_sel) - 1;
            return 1;
        }
        if (e->a == K_ENTER) {
            if (menu_sel >= 0) choose(menu_sel);
            else crystal_close();
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
            if (item >= 0) choose(item);
            else crystal_close();
            return 1;
        }
    }
    return 0;
}

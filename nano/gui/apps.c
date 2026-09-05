/* apps.c - what the shell's menu opens.
 *
 * Each application is a window with a draw routine and an event routine;
 * the shell owns the frame, the title bar and the stacking, so an
 * application only ever draws inside its own rectangle.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"

#define PANEL       0x17120C
#define PANEL_HI    0x241C12
#define EDGE        0x3A2C18
#define AMBER       0xF0A020
#define AMBER_DIM   0x8A5E16
#define AMBER_HOT   0xFFC65A
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68
#define ROW_H       24

/* ---------------------------------------------------------------- about */
static const char *about_lines[] = {
    "A small operating system written from scratch",
    "in x86 assembly and C: its own boot sector,",
    "filesystem, DOS-compatible interrupts, sound",
    "driver and this graphical shell.",
    "",
    "It runs real DOS programs, plays music, and",
    "boots a laptop from a USB stick.",
    0
};

static void about_draw(struct window *w)
{
    int y = w->y + 18, i;
    text(F_TITLE, w->x + 22, y, "Ember", AMBER_HOT);
    y += text_height(F_TITLE) + 2;
    text(F_SMALL, w->x + 22, y, "version 1.2", TEXT_DIM);
    y += text_height(F_SMALL) + 14;
    fill(w->x + 22, y, w->w - 44, 1, EDGE);
    y += 14;
    for (i = 0; about_lines[i]; i++) {
        text(F_NORMAL, w->x + 22, y, about_lines[i], TEXT);
        y += text_height(F_NORMAL) + 1;
    }
}

void app_about(void)
{
    win_open("About Ember", 430, 300, about_draw, 0);
}

/* ---------------------------------------------------------------- help */
static const char *help_lines[] = {
    "Windows key      open the crystal",
    "Escape           close the menu, or the window in front",
    "Tab              bring the window behind to the front",
    "F1               this",
    "F10              leave the desktop",
    "",
    "Click the crystal at the top to open the menu.  Click a",
    "desktop icon once to pick it, again to open it.  Drag a",
    "window by its title bar; the cross at the right closes it.",
    "",
    "In the music player: click a track to pick it, again to",
    "play.  Space pauses.  The bar under the display seeks,",
    "and + and - change the volume.",
    0
};

static void help_draw(struct window *w)
{
    int y = w->y + 16, i;
    text(F_TITLE, w->x + 22, y, "Getting around", AMBER_HOT);
    y += text_height(F_TITLE) + 8;
    fill(w->x + 22, y, w->w - 44, 1, EDGE);
    y += 12;
    for (i = 0; help_lines[i]; i++) {
        text(F_NORMAL, w->x + 22, y, help_lines[i],
             help_lines[i][0] && help_lines[i][17] == ' ' ? TEXT : TEXT);
        y += text_height(F_NORMAL) + 1;
    }
}

void app_help(void)
{
    win_open("Help", 470, 400, help_draw, 0);
}

/* ---------------------------------------------------------------- stubs */
static void soon_draw(struct window *w)
{
    text(F_NORMAL, w->x + 20, w->y + 24, "Not built yet.", TEXT_DIM);
}

void app_text(void)  { win_open("Text Viewer", 460, 300, soon_draw, 0); }
void app_doom(void)  { win_open("Doom", 380, 160, soon_draw, 0); }

/* ---------------------------------------------------------------- pictures */
/* A short list of the BMP files on the disk, so a background can be picked
   without typing a path.  It looks in \WALL first, then the root. */
#define MAX_PICS 64
static char pic_names[MAX_PICS][40];
static char pic_dir[40];
static int pic_count;

static void pics_scan(const char *where)
{
    struct dos_find f;
    char pattern[80];
    int rc;
    pic_count = 0;
    strncpy(pic_dir, where, sizeof pic_dir - 1);
    pic_dir[sizeof pic_dir - 1] = 0;
    strcpy(pattern, pic_dir);
    if (pattern[0] && pattern[strlen(pattern) - 1] != '\\') strcat(pattern, "\\");
    strcat(pattern, "*.*");
    for (rc = sys_findfirst(pattern, &f); rc == 0 && pic_count < MAX_PICS;
         rc = sys_findnext(&f)) {
        char longname[84];
        const char *use = f.name, *dot;
        if (f.attr & 0x18) continue;
        if (sys_long_name(longname, sizeof longname) > 0) use = longname;
        dot = strrchr(use, '.');
        if (!dot) continue;
        if (strcasecmp(dot, ".BMP") && strcasecmp(dot, ".JPG") &&
            strcasecmp(dot, ".JPEG"))
            continue;
        strncpy(pic_names[pic_count], use, sizeof pic_names[0] - 1);
        pic_names[pic_count][sizeof pic_names[0] - 1] = 0;
        pic_count++;
    }
}

static void pics_draw(struct window *w)
{
    int rows = (w->h - 44) / ROW_H, i;
    text(F_SMALL, w->x + 16, w->y + 10,
         pic_count ? "Pick a picture for the background"
                   : "No .JPG or .BMP files in \\WALL or the root", TEXT_DIM);
    fill(w->x + 12, w->y + 32, w->w - 24, 1, EDGE);
    if (w->sel < w->top) w->top = w->sel;
    if (w->sel >= w->top + rows) w->top = w->sel - rows + 1;
    for (i = 0; i < rows; i++) {
        int idx = w->top + i, ry = w->y + 40 + i * ROW_H;
        if (idx >= pic_count) break;
        if (idx == w->sel) {
            fill(w->x + 8, ry - 2, w->w - 16, ROW_H, 0x2A1D0C);
            fill(w->x + 8, ry - 2, 3, ROW_H, AMBER);
        }
        text_clipped(F_NORMAL, w->x + 22, ry - 2, w->w - 40, pic_names[idx],
                     idx == w->sel ? AMBER_HOT : TEXT);
    }
}

static void pics_use(struct window *w)
{
    char path[128];
    if (w->sel < 0 || w->sel >= pic_count) return;
    path[0] = 0;
    if (pic_dir[0]) {
        strcpy(path, pic_dir);
        if (path[strlen(path) - 1] != '\\') strcat(path, "\\");
    }
    strcat(path, pic_names[w->sel]);
    if (wall_load(path) == 0) {
        wall_save();
        damage_all();
    }
}

static int pics_event(struct window *w, struct event *e)
{
    if (e->type == EV_KEY) {
        if (e->a == K_UP && w->sel > 0) w->sel--;
        else if (e->a == K_DOWN && w->sel + 1 < pic_count) w->sel++;
        else if (e->a == K_ENTER) pics_use(w);
        else return 0;
        return 1;
    }
    if (e->type == EV_MOUSE_DOWN && e->b >= 40) {
        int idx = w->top + (e->b - 40) / ROW_H;
        if (idx < pic_count) {
            if (idx == w->sel) pics_use(w);
            else w->sel = idx;
        }
        return 1;
    }
    return 0;
}

void app_pictures(void)
{
    struct window *w;
    int id;
    pics_scan("\\WALL");
    if (pic_count == 0) pics_scan("");
    id = win_open("Background", 420, 340, pics_draw, pics_event);
    if (id < 0) return;
    w = win_at(id);
    w->sel = 0;
    w->top = 0;
}

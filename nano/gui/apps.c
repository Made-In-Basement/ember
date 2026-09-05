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

/* ---------------------------------------------------------------- files */
#define MAX_FILES 256
struct file_row { char name[40]; uint32_t size; int is_dir; };
static struct file_row rows[MAX_WINDOWS][MAX_FILES];

static void files_read(struct window *w)
{
    struct file_row *r = rows[0];
    struct dos_find f;
    char pattern[128];
    int n = 0, rc;
    void *self = w->data;
    r = (struct file_row *)self;

    strcpy(pattern, w->path);
    if (pattern[0] && pattern[strlen(pattern) - 1] != '\\')
        strcat(pattern, "\\");
    strcat(pattern, "*.*");
    for (rc = sys_findfirst(pattern, &f); rc == 0 && n < MAX_FILES;
         rc = sys_findnext(&f)) {
        char longname[84];
        if (f.name[0] == '.' && f.name[1] == 0) continue;
        if (f.attr & 0x08) continue;                    /* the volume label */
        if (sys_long_name(longname, sizeof longname) > 0)
            strncpy(r[n].name, longname, sizeof r[n].name - 1);
        else
            strncpy(r[n].name, f.name, sizeof r[n].name - 1);
        r[n].name[sizeof r[n].name - 1] = 0;
        r[n].size = f.size;
        r[n].is_dir = (f.attr & 0x10) != 0;
        n++;
    }
    /* directories first, then by name */
    {
        int i, j;
        for (i = 1; i < n; i++) {
            struct file_row key = r[i];
            for (j = i; j > 0; j--) {
                struct file_row *p = &r[j - 1];
                int after;
                if (p->is_dir != key.is_dir) after = !p->is_dir;
                else after = strcasecmp(p->name, key.name) > 0;
                if (!after) break;
                r[j] = *p;
            }
            r[j] = key;
        }
    }
    w->count = n;
    if (w->sel >= n) w->sel = n ? n - 1 : 0;
    if (w->top > w->sel) w->top = w->sel;
}

static void files_draw(struct window *w)
{
    struct file_row *r = (struct file_row *)w->data;
    int rows_shown = (w->h - 46) / ROW_H, i;
    char buf[64];

    /* the path, along the top */
    fill(w->x, w->y, w->w, 30, PANEL_HI);
    fill(w->x, w->y + 29, w->w, 1, EDGE);
    text(F_SMALL, w->x + 14, w->y + 6,
         w->path[0] ? w->path : "\\", AMBER);

    if (w->sel < w->top) w->top = w->sel;
    if (w->sel >= w->top + rows_shown) w->top = w->sel - rows_shown + 1;

    for (i = 0; i < rows_shown; i++) {
        int idx = w->top + i;
        int ry = w->y + 36 + i * ROW_H;
        if (idx >= w->count) break;
        if (idx == w->sel) {
            fill(w->x + 6, ry - 3, w->w - 12, ROW_H, 0x2A1D0C);
            fill(w->x + 6, ry - 3, 2, ROW_H, AMBER);
        }
        text_clipped(F_NORMAL, w->x + 18, ry, w->w - 130, r[idx].name,
                     r[idx].is_dir ? AMBER : (idx == w->sel ? AMBER_HOT : TEXT));
        if (r[idx].is_dir) {
            text(F_SMALL, w->x + w->w - 60, ry + 2, "folder", TEXT_DIM);
        } else {
            if (r[idx].size >= 1024)
                snprintf(buf, sizeof buf, "%u KB", (unsigned)(r[idx].size / 1024));
            else
                snprintf(buf, sizeof buf, "%u B", (unsigned)r[idx].size);
            text(F_SMALL, w->x + w->w - 20 - text_width(F_SMALL, buf), ry + 2,
                 buf, TEXT_DIM);
        }
    }
    /* a scroll indicator when there is more than fits */
    if (w->count > rows_shown) {
        int track = w->h - 46;
        int bar = track * rows_shown / w->count;
        int pos = track * w->top / w->count;
        if (bar < 20) bar = 20;
        fill(w->x + w->w - 6, w->y + 36 + pos, 3, bar, AMBER_DIM);
    }
}

static void files_enter(struct window *w)
{
    struct file_row *r = (struct file_row *)w->data;
    if (w->sel >= w->count) return;
    if (!r[w->sel].is_dir) return;
    if (!strcmp(r[w->sel].name, "..")) {
        char *p = strrchr(w->path, '\\');
        if (p && p != w->path) *p = 0;
        else w->path[0] = 0;
    } else {
        if (w->path[0] && w->path[strlen(w->path) - 1] != '\\')
            strcat(w->path, "\\");
        strcat(w->path, r[w->sel].name);
    }
    w->sel = 0;
    w->top = 0;
    files_read(w);
}

static int files_event(struct window *w, struct event *e)
{
    int rows_shown = (w->h - 46) / ROW_H;
    if (e->type == EV_KEY) {
        if (e->a == K_UP && w->sel > 0) w->sel--;
        else if (e->a == K_DOWN && w->sel + 1 < w->count) w->sel++;
        else if (e->a == K_ENTER) files_enter(w);
        return 1;
    }
    if (e->type == EV_MOUSE_DOWN && e->b >= 36) {
        int idx = w->top + (e->b - 36) / ROW_H;
        if (idx < w->count) {
            if (idx == w->sel) files_enter(w);
            else w->sel = idx;
        }
        return 1;
    }
    (void)rows_shown;
    return 0;
}

void app_files(void)
{
    static struct file_row storage[MAX_FILES];
    int id = win_open("Files", 520, 380, files_draw, files_event);
    struct window *w;
    if (id < 0) return;
    w = win_at(id);
    w->data = storage;
    w->path[0] = 0;
    files_read(w);
}

/* ---------------------------------------------------------------- stubs */
static void soon_draw(struct window *w)
{
    text(F_NORMAL, w->x + 20, w->y + 24, "Not built yet.", TEXT_DIM);
}

void app_music(void) { win_open("Music", 380, 160, soon_draw, 0); }
void app_text(void)  { win_open("Text Viewer", 460, 300, soon_draw, 0); }
void app_doom(void)  { win_open("Doom", 380, 160, soon_draw, 0); }

#ifndef SHELL_H
#define SHELL_H
#include "input.h"

#define MAX_WINDOWS 12

struct window {
    char title[40];
    int x, y, w, h;
    int open;
    void (*draw)(struct window *w);
    int (*event)(struct window *w, struct event *e);
    /* whatever the app keeps for itself */
    int sel, top, count, scroll;
    void *data;
    char path[96];
    /* the frame's own business */
    int minimized, maxed, fixed;        /* fixed: neither resized nor maximized */
    int app;                            /* the menu action that opened it, for the settings file */
    int sx, sy, sw, sh;                 /* where it was before being maximized */
};

void win_no_app(void);                  /* the next window is not a program of its own */
int  win_open(const char *title, int w, int h, void (*draw)(struct window *),
              int (*event)(struct window *, struct event *));
void win_close(int id);
struct window *win_at(int id);
int  win_count(void);
int  win_focused(void);
void shell_quit(void);
void shell_run_menu(int item);
void shell_launch(const char *path);    /* a DOS program, and the desktop again after */
int  shell_runnable(const char *name);

/* the crystal at the top of the screen */
extern int crystal_x, crystal_y;
void crystal_draw(void);
int  crystal_event(struct event *e);
int  crystal_hit(int x, int y);
int  crystal_busy(void);
void crystal_rect(int *x, int *y, int *w, int *h);
int  crystal_reach(void);
int  crystal_is_open(void);
void crystal_toggle(void);
void crystal_close(void);

/* the applications the menu offers */
/* the background */
enum { WALL_EMBER, WALL_CHARCOAL, WALL_BLUE, WALL_FOREST, WALL_PLUM,
       WALL_PICTURE = 100 };
extern int wall_kind;
extern char wall_file[96];
void wall_draw(void);
void wall_set(int kind);
int  wall_line(char *buf, int n);       /* the background, as a line of the settings file */

/* the settings file, \EMBER.CFG: one line to a setting, all of it rewritten
   whenever anything changes, so it can still be read at the DOS prompt */
const char *cfg_get(const char *key);   /* the rest of the line, or 0 */
const char *cfg_next(const char *key, const char *after);   /* for repeated keys */
void cfg_write(void);

void app_icons(void);                   /* choose which icons the desktop shows */
void icons_closed(int id);
int  icons_catalogue_n(void);           /* every program that may sit on the wall */
const char *icons_cat_label(int i);
const struct image *icons_cat_art(int i);
int  icons_has(int cat);
void icons_toggle(int cat);
int  wall_load(const char *path);
int  wall_choice_count(void);
const char *wall_name(int kind);
void wall_save(void);
void wall_load_config(void);

/* the menu that appears where you right-click */
void popup_open(int x, int y, const char **items, const int *ticks, int n,
                void (*on_choice)(int));
void popup_close(void);
void popup_draw(void);
int  popup_is_open(void);
void popup_rect(int *x, int *y, int *w, int *h);
int  popup_event(struct event *e);

/* what the menu and the icons can do */
enum { A_FILES, A_MUSIC, A_PROMPT, A_CALC, A_WRITE, A_DOOM, A_MONITOR, A_KEYBOARD, A_HELP,
       A_ABOUT, A_EXIT, A_VIEWER, A_CLOCK, A_CALENDAR, A_NOTES, A_SHOT, A_PAINT, A_DISK, A_SCENE3D,
       A_ICONS };

void app_pictures(void);                /* choose a picture for the background */
uint32_t *wall_decode(const char *path);  /* a picture, screen-sized; free it */
void app_notice(const char *title, const char *line1, const char *line2);
void app_viewer(void);
void app_viewer_open(const char *path);
void viewer_closed(int id);
int  viewer_tick(void);
void app_clock(void);
int  clock_tick(void);
int  clock_window(void);
void clock_closed(int id);
void app_calendar(void);
void calendar_closed(int id);
void app_notes(void);
void app_notes_new(void);
void notes_closed(int id);
void app_screenshot(void);
int  screenshot_save(char *out, int out_size);
int  bmp_write(const char *name, const uint32_t *px, int w, int h);   /* 24-bit, top row first */
int  png_write(const char *name, const uint32_t *px, int w, int h);   /* compressed; a tenth the size */
extern int png_effort;                  /* how hard it looks for matches; low is fast */
uint32_t *png_decode(const char *path, int *w, int *h);              /* the picture at its own size */
void app_paint(void);
void paint_closed(int id);
void app_scene3d(void);
int  scene3d_tick(void);
int  scene3d_window(void);
void scene3d_closed(int id);
void shell_repaint(int x, int y, int w, int h);  /* a region an application changed */
void app_calc(void);
void app_prompt(void);
void app_about(void);
void app_help(void);
void app_files(void);
void app_music(void);
int  music_tick(void);                  /* keeps the sound fed; 1 = redraw */
void music_chime(void);                 /* the sound it makes on opening */
int  music_active(void);                /* sound is playing */
void music_feed(void);                  /* top the ring up; no analyser, no redraw */
int  music_window(void);                /* its window, or -1 */
void music_display_rect(int *x, int *y, int *w, int *h);
void music_closed(int id);
void app_text(void);
void app_doom(void);
void app_write(void);                   /* the word processor */
void app_write_open(const char *path);
void write_closed(int id);

/* the keyboard on the screen */
extern int osk_visible;
void osk_toggle(void);
void osk_draw(void);
int  osk_event(struct event *e);
int  osk_tick(void);
void osk_rect(int *x, int *y, int *w, int *h);

/* where the time goes, counted by the main loop for the monitor */
struct shell_stats {
    unsigned frames, loops;
    unsigned draw_us, present_us, idle_us;
    unsigned long present_bytes;
};
extern struct shell_stats shell_stats;
void app_monitor(void);
int  monitor_tick(void);                /* 1 = its window wants repainting */
int  monitor_window(void);
void monitor_closed(int id);

#endif

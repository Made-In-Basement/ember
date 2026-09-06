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
};

int  win_open(const char *title, int w, int h, void (*draw)(struct window *),
              int (*event)(struct window *, struct event *));
void win_close(int id);
struct window *win_at(int id);
int  win_count(void);
int  win_focused(void);
void shell_quit(void);
void shell_run_menu(int item);

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

void app_pictures(void);                /* choose a picture for the background */
void app_calc(void);
void app_prompt(void);
void app_about(void);
void app_help(void);
void app_files(void);
void app_music(void);
int  music_tick(void);                  /* keeps the sound fed; 1 = redraw */
void music_chime(void);                 /* the sound it makes on opening */
int  music_active(void);                /* sound is playing: do not idle */
int  music_window(void);                /* its window, or -1 */
void music_closed(int id);
void app_text(void);
void app_doom(void);

#endif

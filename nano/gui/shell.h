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
int  crystal_is_open(void);
void crystal_toggle(void);
void crystal_close(void);

/* the applications the menu offers */
void app_about(void);
void app_files(void);
void app_music(void);
void app_text(void);
void app_doom(void);

#endif

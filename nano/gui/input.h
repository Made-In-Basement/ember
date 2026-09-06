#ifndef INPUT_H
#define INPUT_H

enum {
    EV_MOUSE_MOVE = 1, EV_MOUSE_DOWN, EV_MOUSE_UP,
    EV_RIGHT_DOWN, EV_RIGHT_UP, EV_KEY
};

struct event { int type, a, b, dbl; };   /* mouse: a,b = x,y (dbl: a double-click).  key: a = scan, b = char */

extern int mouse_x, mouse_y, mouse_buttons, mouse_present, mouse_via_bios;
extern int mouse_max_x, mouse_max_y;

int  input_open(int width, int height, int passive);
void input_inject_mouse(int dx, int dy, int buttons);   /* dy positive = down */
void input_inject_absolute(int x, int y, int down);     /* a finger on a screen */
void input_start_keyboard(void);
void input_close(void);
int  next_event(struct event *e);

/* keys we care about, in scancodes */
#define K_ESC   0x01
#define K_ENTER 0x1C
#define K_UP    0x148
#define K_DOWN  0x150
#define K_LEFT  0x14B
#define K_RIGHT 0x14D
#define K_F10   0x44
#define K_TAB   0x0F
#define K_WIN   0x15B                   /* the Windows key, left */
#define K_WIN_R 0x15C
#define K_MENU  0x15D                   /* the menu key beside it */
#define K_F1    0x3B

#endif

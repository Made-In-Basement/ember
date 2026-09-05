#ifndef DRAW_H
#define DRAW_H
#include <stdint.h>

/* Colours are 0xRRGGBB.  Everything is composed into a 32-bit buffer in
   ordinary memory and only the changed parts are pushed to the screen, so
   the display never shows a half-drawn frame. */

extern int scr_w, scr_h;
extern uint32_t *back;                  /* scr_w * scr_h pixels */

int  draw_open(int want_w, int want_h);         /* 0 on success */
void draw_close(void);
void draw_present(void);                        /* damaged areas -> the screen */
void damage(int x, int y, int w, int h);
void damage_all(void);

/* clipping: every primitive is confined to this rectangle */
void clip_set(int x, int y, int w, int h);
void clip_none(void);
void clip_get(int *x, int *y, int *w, int *h);

void fill(int x, int y, int w, int h, uint32_t c);
void fill_alpha(int x, int y, int w, int h, uint32_t c, int alpha);
void vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void hgradient(int x, int y, int w, int h, uint32_t left, uint32_t right);
void frame(int x, int y, int w, int h, uint32_t c);
void round_fill(int x, int y, int w, int h, int r, uint32_t c);
void round_frame(int x, int y, int w, int h, int r, uint32_t c);
void round_frame_alpha(int x, int y, int w, int h, int r, uint32_t c, int alpha);
void round_fill_alpha(int x, int y, int w, int h, int r, uint32_t c, int alpha);
void shadow(int x, int y, int w, int h, int r, int spread);
void pixel_blend(int x, int y, uint32_t c, int alpha);

uint32_t mix(uint32_t a, uint32_t b, int t);    /* t = 0..255 */
void poly_fill(const int *pts, int n, uint32_t c);      /* pts = x,y pairs */
void poly_fill_alpha(const int *pts, int n, uint32_t c, int alpha);
void glow(int x, int y, int w, int h, int r, uint32_t c, int rings);
void soft_ellipse(int cx, int cy, int rx, int ry, uint32_t c, int max_alpha);
void inset(int x, int y, int w, int h, int r, uint32_t light, uint32_t dark);

/* a clock for animation, in milliseconds since the shell started */
void clock_start(void);
unsigned now_ms(void);

/* text */
enum { F_SMALL, F_NORMAL, F_BOLD, F_TITLE };
int  text_width(int face, const char *s);
int  text_height(int face);
void text(int face, int x, int y, const char *s, uint32_t c);   /* y = top */
void text_clipped(int face, int x, int y, int max_w, const char *s, uint32_t c);

#endif

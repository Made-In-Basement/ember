#ifndef PGFX_H
#define PGFX_H
#include <stdint.h>

enum {
    PAL_BLACK, PAL_BACK, PAL_PANEL, PAL_PANEL_HI, PAL_PANEL_LO,
    PAL_WHITE, PAL_TEXT, PAL_DIM, PAL_ACCENT, PAL_ACCENT_HI, PAL_TROUGH,
    PAL_VU1, PAL_VU2, PAL_VU3, PAL_VU4, PAL_VU5,
    PAL_COUNT
};

extern int gfx_w, gfx_h, gfx_pitch, gfx_font_h;
extern uint8_t *gfx_fb;

int  gfx_init(void);
void gfx_done(void);
void gfx_clear(int c);
void gfx_rect(int x, int y, int w, int h, int c);
void gfx_bevel(int x, int y, int w, int h, int topleft, int bottomright);
void gfx_panel(int x, int y, int w, int h);
void gfx_char(int x, int y, int ch, int fg, int bg);
void gfx_text(int x, int y, const char *s, int fg, int bg);
void gfx_textn(int x, int y, const char *s, int n, int fg, int bg);

#endif

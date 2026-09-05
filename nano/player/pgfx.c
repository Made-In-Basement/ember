/* pgfx.c - a small 256-colour drawing layer for the player.
 *
 * Asks the card for 640x480 with a linear framebuffer, which a 32-bit
 * program can write to directly, and falls back to plain VGA 320x200 when
 * the card will not offer one.  Text is drawn with the font already in the
 * BIOS ROM, so no font has to be carried around.
 */
#include <nanolibc.h>
#include "nano.h"
#include "pgfx.h"

int gfx_w, gfx_h, gfx_pitch, gfx_font_h = 16;
uint8_t *gfx_fb;
static int gfx_mode;                    /* the VESA mode in use, 0 = 13h */
static const uint8_t *font;

/* red, green, blue in 0..63, the shades the interface is drawn from */
static const uint8_t palette[PAL_COUNT][3] = {
    {  0,  0,  0 },                     /* PAL_BLACK    */
    {  4,  8, 18 },                     /* PAL_BACK     */
    { 14, 20, 34 },                     /* PAL_PANEL    */
    { 24, 32, 48 },                     /* PAL_PANEL_HI */
    {  7, 11, 20 },                     /* PAL_PANEL_LO */
    { 63, 63, 63 },                     /* PAL_WHITE    */
    { 44, 48, 56 },                     /* PAL_TEXT     */
    { 26, 30, 40 },                     /* PAL_DIM      */
    { 20, 44, 60 },                     /* PAL_ACCENT   */
    { 34, 58, 63 },                     /* PAL_ACCENT_HI*/
    {  8, 16, 28 },                     /* PAL_TROUGH   */
    { 10, 46, 24 },                     /* PAL_VU1      */
    { 22, 56, 26 },                     /* PAL_VU2      */
    { 46, 60, 22 },                     /* PAL_VU3      */
    { 60, 44, 14 },                     /* PAL_VU4      */
    { 60, 20, 16 },                     /* PAL_VU5      */
};

static void set_palette(void)
{
    int i;
    outb(0x3C8, 0);
    for (i = 0; i < PAL_COUNT; i++) {
        outb(0x3C9, palette[i][0]);
        outb(0x3C9, palette[i][1]);
        outb(0x3C9, palette[i][2]);
    }
}

/* the 8x16 glyphs the BIOS keeps in its own ROM */
static void find_font(void)
{
    struct rmcall r;
    memset(&r, 0, sizeof r);
    r.ax = 0x1130;
    r.bx = 0x0600;                      /* the 8x16 set */
    r.intno = 0x10;
    sys_bios(&r);
    font = (const uint8_t *)((uint32_t)r.es * 16 + r.bp);
    gfx_font_h = 16;
    if (!r.es && !r.bp) {               /* no answer: try the 8x8 set */
        memset(&r, 0, sizeof r);
        r.ax = 0x1130;
        r.bx = 0x0300;
        r.intno = 0x10;
        sys_bios(&r);
        font = (const uint8_t *)((uint32_t)r.es * 16 + r.bp);
        gfx_font_h = 8;
    }
}

int gfx_init(void)
{
    static const int wanted[] = { 0x101, 0x103, 0 };   /* 640x480, 800x600 */
    struct vbe_mode m;
    int i;
    for (i = 0; wanted[i]; i++) {
        if (sys_vbe_mode(wanted[i], &m) != 0) continue;
        if (m.bpp != 8 || !m.framebuffer) continue;
        if (sys_set_vbe_mode(wanted[i], 1) != 0) continue;
        gfx_mode = wanted[i];
        gfx_w = m.width;
        gfx_h = m.height;
        gfx_pitch = m.pitch;
        gfx_fb = (uint8_t *)m.framebuffer;
        set_palette();
        find_font();
        return 0;
    }
    sys_set_video_mode(0x13);           /* the mode every card has */
    gfx_mode = 0;
    gfx_w = 320;
    gfx_h = 200;
    gfx_pitch = 320;
    gfx_fb = (uint8_t *)0xA0000;
    set_palette();
    find_font();
    return 0;
}

void gfx_done(void)
{
    sys_set_video_mode(3);
}

void gfx_rect(int x, int y, int w, int h, int c)
{
    int i;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gfx_w) w = gfx_w - x;
    if (y + h > gfx_h) h = gfx_h - y;
    if (w <= 0 || h <= 0) return;
    for (i = 0; i < h; i++)
        memset(gfx_fb + (uint32_t)(y + i) * gfx_pitch + x, c, w);
}

void gfx_clear(int c)
{
    gfx_rect(0, 0, gfx_w, gfx_h, c);
}

/* a raised or sunken edge, the way this sort of interface has always looked */
void gfx_bevel(int x, int y, int w, int h, int topleft, int bottomright)
{
    gfx_rect(x, y, w, 1, topleft);
    gfx_rect(x, y, 1, h, topleft);
    gfx_rect(x, y + h - 1, w, 1, bottomright);
    gfx_rect(x + w - 1, y, 1, h, bottomright);
}

void gfx_panel(int x, int y, int w, int h)
{
    gfx_rect(x, y, w, h, PAL_PANEL);
    gfx_bevel(x, y, w, h, PAL_PANEL_HI, PAL_PANEL_LO);
}

void gfx_char(int x, int y, int ch, int fg, int bg)
{
    const uint8_t *g = font + (ch & 0xFF) * gfx_font_h;
    int row, col;
    if (x < 0 || y < 0 || x + 8 > gfx_w || y + gfx_font_h > gfx_h) return;
    for (row = 0; row < gfx_font_h; row++) {
        uint8_t bits = g[row];
        uint8_t *p = gfx_fb + (uint32_t)(y + row) * gfx_pitch + x;
        for (col = 0; col < 8; col++) {
            if (bits & 0x80) *p = (uint8_t)fg;
            else if (bg >= 0) *p = (uint8_t)bg;
            bits <<= 1;
            p++;
        }
    }
}

void gfx_text(int x, int y, const char *s, int fg, int bg)
{
    while (*s) {
        gfx_char(x, y, (uint8_t)*s++, fg, bg);
        x += 8;
    }
}

void gfx_textn(int x, int y, const char *s, int n, int fg, int bg)
{
    int i, end = 0;
    for (i = 0; i < n; i++) {
        if (!end && !s[i]) end = 1;
        gfx_char(x, y, end ? ' ' : (uint8_t)s[i], fg, bg);
        x += 8;
    }
}

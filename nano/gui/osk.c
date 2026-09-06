/* osk.c - a keyboard on the screen.
 *
 * For a laptop folded flat, where the real keyboard is face down and
 * switched off.  It is not a window: it lies over everything at the
 * bottom of the screen, takes taps before any window sees them, and
 * feeds what is tapped into the same queue the real keyboard fills, so
 * every window is typed into exactly as before.  Shift is a one-shot,
 * Caps a toggle, and the key that was just tapped lights for a moment so
 * a finger gets an answer.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"

#define PANEL     0x1A140E
#define KEY       0x2A2114
#define KEY_LIT   0x4A3618
#define KEY_HOT   0xF0A020
#define EDGE      0x4A3618
#define TEXT      0xD8C8B0
#define TEXT_DIM  0x8A7C68
#define AMBER_HOT 0xFFC65A

#define ROWS      5
#define KEY_H     50
#define GAP       6
#define PAD       10
#define FLASH_MS  130

/* one key: what it shows, what it sends, how wide (in half-units).  A
   scan of -1 is Caps, -2 Shift, -3 hides the keyboard. */
struct key { const char *label, *shifted; int scan, ch, sch, w; };

static const struct key row0[] = {
    { "`", "~", 0x29, '`', '~', 2 }, { "1", "!", 0x02, '1', '!', 2 }, { "2", "@", 0x03, '2', '@', 2 },
    { "3", "#", 0x04, '3', '#', 2 }, { "4", "$", 0x05, '4', '$', 2 }, { "5", "%", 0x06, '5', '%', 2 },
    { "6", "^", 0x07, '6', '^', 2 }, { "7", "&", 0x08, '7', '&', 2 }, { "8", "*", 0x09, '8', '*', 2 },
    { "9", "(", 0x0A, '9', '(', 2 }, { "0", ")", 0x0B, '0', ')', 2 }, { "-", "_", 0x0C, '-', '_', 2 },
    { "=", "+", 0x0D, '=', '+', 2 }, { "Backspace", 0, 0x0E, 8, 8, 4 }, { 0 }
};
static const struct key row1[] = {
    { "Tab", 0, 0x0F, 9, 9, 3 }, { "q", "Q", 0x10, 'q', 'Q', 2 }, { "w", "W", 0x11, 'w', 'W', 2 },
    { "e", "E", 0x12, 'e', 'E', 2 }, { "r", "R", 0x13, 'r', 'R', 2 }, { "t", "T", 0x14, 't', 'T', 2 },
    { "y", "Y", 0x15, 'y', 'Y', 2 }, { "u", "U", 0x16, 'u', 'U', 2 }, { "i", "I", 0x17, 'i', 'I', 2 },
    { "o", "O", 0x18, 'o', 'O', 2 }, { "p", "P", 0x19, 'p', 'P', 2 }, { "[", "{", 0x1A, '[', '{', 2 },
    { "]", "}", 0x1B, ']', '}', 2 }, { "\\", "|", 0x2B, '\\', '|', 3 }, { 0 }
};
static const struct key row2[] = {
    { "Caps", 0, -1, 0, 0, 4 }, { "a", "A", 0x1E, 'a', 'A', 2 }, { "s", "S", 0x1F, 's', 'S', 2 },
    { "d", "D", 0x20, 'd', 'D', 2 }, { "f", "F", 0x21, 'f', 'F', 2 }, { "g", "G", 0x22, 'g', 'G', 2 },
    { "h", "H", 0x23, 'h', 'H', 2 }, { "j", "J", 0x24, 'j', 'J', 2 }, { "k", "K", 0x25, 'k', 'K', 2 },
    { "l", "L", 0x26, 'l', 'L', 2 }, { ";", ":", 0x27, ';', ':', 2 }, { "'", "\"", 0x28, '\'', '"', 2 },
    { "Enter", 0, 0x1C, 13, 13, 4 }, { 0 }
};
static const struct key row3[] = {
    { "Shift", 0, -2, 0, 0, 5 }, { "z", "Z", 0x2C, 'z', 'Z', 2 }, { "x", "X", 0x2D, 'x', 'X', 2 },
    { "c", "C", 0x2E, 'c', 'C', 2 }, { "v", "V", 0x2F, 'v', 'V', 2 }, { "b", "B", 0x30, 'b', 'B', 2 },
    { "n", "N", 0x31, 'n', 'N', 2 }, { "m", "M", 0x32, 'm', 'M', 2 }, { ",", "<", 0x33, ',', '<', 2 },
    { ".", ">", 0x34, '.', '>', 2 }, { "/", "?", 0x35, '/', '?', 2 }, { "Shift", 0, -2, 0, 0, 5 }, { 0 }
};
static const struct key row4[] = {
    { "Esc", 0, 0x01, 27, 27, 3 }, { "Hide", 0, -3, 0, 0, 3 }, { " ", 0, 0x39, ' ', ' ', 14 },
    { "<", 0, K_LEFT, 0, 0, 2 }, { "^", 0, K_UP, 0, 0, 2 }, { "v", 0, K_DOWN, 0, 0, 2 },
    { ">", 0, K_RIGHT, 0, 0, 2 }, { 0 }
};
static const struct key *rows[ROWS] = { row0, row1, row2, row3, row4 };

int osk_visible;
static int shift, caps, hover_row = -1, hover_col = -1;
static int flash_row = -1, flash_col = -1;
static unsigned flash_ms;
static int px, py, pw, ph, unit;        /* the panel, and a half-key in pixels */

static int row_units(const struct key *r)
{
    int u = 0;
    while (r->label) u += r->w, r++;
    return u;
}

static void layout(void)
{
    int i, most = 0;
    for (i = 0; i < ROWS; i++) {
        int u = row_units(rows[i]);
        if (u > most) most = u;
    }
    /* keys big enough for a finger, the panel no wider than the screen */
    unit = (scr_w * 2 / 3) / most;
    if (unit * 2 > 64) unit = 32;
    if (unit < 14) unit = 14;
    pw = most * unit + PAD * 2;
    ph = ROWS * (KEY_H + GAP) - GAP + PAD * 2;
    px = (scr_w - pw) / 2;
    py = scr_h - ph - 12;
}

void osk_rect(int *x, int *y, int *w, int *h)
{
    if (!unit) layout();
    *x = px - 8; *y = py - 8; *w = pw + 16; *h = ph + 16;
}

void osk_toggle(void)
{
    layout();
    osk_visible = !osk_visible;
    shift = 0;
    hover_row = hover_col = -1;
}

/* which key is under a point: row and column, or -1 */
static int hit(int x, int y, int *col)
{
    int r = (y - py - PAD) / (KEY_H + GAP);
    int kx;
    const struct key *k;
    if (x < px || x >= px + pw || y < py + PAD || r < 0 || r >= ROWS) return -1;
    if ((y - py - PAD) % (KEY_H + GAP) >= KEY_H) return -1;     /* in a gap */
    kx = px + PAD + (pw - PAD * 2 - row_units(rows[r]) * unit) / 2;
    for (k = rows[r], *col = 0; k->label; k++, (*col)++) {
        if (x >= kx && x < kx + k->w * unit - GAP) return r;
        kx += k->w * unit;
    }
    return -1;
}

void osk_draw(void)
{
    int r;
    if (!osk_visible) return;
    shadow(px + 3, py + 5, pw, ph, 8, 10);
    round_fill(px, py, pw, ph, 8, PANEL);
    round_frame(px, py, pw, ph, 8, EDGE);
    for (r = 0; r < ROWS; r++) {
        const struct key *k;
        int c, kx = px + PAD + (pw - PAD * 2 - row_units(rows[r]) * unit) / 2;
        int ky = py + PAD + r * (KEY_H + GAP);
        for (k = rows[r], c = 0; k->label; k++, c++) {
            int kw = k->w * unit - GAP;
            int lit = (k->scan == -2 && shift) || (k->scan == -1 && caps);
            int flashing = (r == flash_row && c == flash_col);
            int hovering = (r == hover_row && c == hover_col);
            const char *label = k->label;
            uint32_t bg = flashing ? KEY_HOT : lit ? KEY_LIT : hovering ? KEY_LIT : KEY;
            uint32_t fg = flashing ? 0x1A140E : lit ? AMBER_HOT : TEXT;
            if (k->shifted && k->ch >= 'a' && k->ch <= 'z' && (shift ^ caps)) label = k->shifted;
            else if (k->shifted && shift && !(k->ch >= 'a' && k->ch <= 'z')) label = k->shifted;
            round_fill(kx, ky, kw, KEY_H, 5, bg);
            fill(kx + 1, ky + 1, kw - 2, 1, flashing ? AMBER_HOT : 0x3A2E1E);   /* the lit lip */
            round_frame(kx, ky, kw, KEY_H, 5, flashing ? AMBER_HOT : EDGE);
            text(F_NORMAL, kx + (kw - text_width(F_NORMAL, label)) / 2,
                 ky + (KEY_H - text_height(F_NORMAL)) / 2, label, fg);
            kx += k->w * unit;
        }
    }
    damage(px - 8, py - 8, pw + 16, ph + 16);
}

static void press(const struct key *k)
{
    int ch;
    if (k->scan == -1) { caps = !caps; return; }
    if (k->scan == -2) { shift = !shift; return; }
    if (k->scan == -3) { osk_visible = 0; return; }
    ch = k->ch;
    if (k->ch >= 'a' && k->ch <= 'z') ch = (shift ^ caps) ? k->sch : k->ch;
    else if (shift && k->sch) ch = k->sch;
    input_inject_key(k->scan, ch);
    shift = 0;
}

/* 1 when the event was the keyboard's */
int osk_event(struct event *e)
{
    int col, r;
    if (!osk_visible) return 0;
    if (e->type == EV_MOUSE_MOVE) {
        int was_r = hover_row, was_c = hover_col;
        r = hit(e->a, e->b, &col);
        hover_row = r;
        hover_col = r >= 0 ? col : -1;
        return was_r != hover_row || was_c != hover_col;       /* repaint if the highlight moved */
    }
    if (e->type == EV_MOUSE_DOWN || e->type == EV_RIGHT_DOWN) {
        r = hit(e->a, e->b, &col);
        if (r < 0) {
            /* a tap on the panel's frame is ours too, so it never reaches
               a window underneath */
            return e->a >= px && e->a < px + pw && e->b >= py && e->b < py + ph;
        }
        press(&rows[r][col]);
        flash_row = r;
        flash_col = col;
        flash_ms = now_ms();
        return 1;
    }
    if (e->type == EV_MOUSE_UP) {
        return e->a >= px && e->a < px + pw && e->b >= py && e->b < py + ph;
    }
    return 0;
}

/* from the main loop: 1 when the lit key should go out */
int osk_tick(void)
{
    if (!osk_visible || flash_row < 0) return 0;
    if (now_ms() - flash_ms < FLASH_MS) return 0;
    flash_row = flash_col = -1;
    return 1;
}

/* notes.c - sticky notes.
 *
 * Small pages of plain text that stay where you leave them.  Each note is
 * a window; typing goes to the note in front, Backspace takes back, Enter
 * starts a line.  Every change is written to \NOTES.TXT at once, notes
 * separated by a line of dashes, so nothing is lost and the file reads
 * fine anywhere else.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"

#define MAX_NOTES   12
#define NOTE_LEN    480
#define NOTE_W      280
#define NOTE_H      220
#define PAGE        0xE8D27A
#define PAGE_EDGE   0xB89A48
#define INK         0x2A2010
#define INK_DIM     0x7A6A40
#define AMBER       0xF0A020

struct note { char text[NOTE_LEN]; int win; int used; };
static struct note notes[MAX_NOTES];
static int loaded;

static const char *file_name = "\\NOTES.TXT";

/* ------------------------------------------------------------ the file */
static void save_all(void)
{
    int h = sys_create(file_name), i, first = 1;
    if (h < 0) return;
    for (i = 0; i < MAX_NOTES; i++) {
        if (!notes[i].used) continue;
        if (!first) sys_write(h, "\r\n-----\r\n", 9);
        first = 0;
        {
            const char *p = notes[i].text;
            while (*p) {
                const char *q = p;
                while (*q && *q != '\n') q++;
                sys_write(h, p, (int)(q - p));
                if (*q == '\n') { sys_write(h, "\r\n", 2); q++; }
                p = q;
            }
        }
    }
    sys_close(h);
}

static void load_all(void)
{
    char *raw;
    int h, got, i = 0, n = 0, k;
    loaded = 1;
    h = sys_open(file_name);
    if (h < 0) return;
    raw = malloc(MAX_NOTES * NOTE_LEN + 64);
    if (!raw) { sys_close(h); return; }
    got = sys_read(h, raw, MAX_NOTES * NOTE_LEN);
    sys_close(h);
    if (got <= 0) { free(raw); return; }
    raw[got] = 0;
    notes[0].used = 1;
    while (i < got && n < MAX_NOTES) {
        if (!memcmp(raw + i, "-----", 5) && (i == 0 || raw[i - 1] == '\n')) {
            i += 5;
            while (i < got && (raw[i] == '\r' || raw[i] == '\n')) i++;
            if (++n < MAX_NOTES) notes[n].used = 1;
            continue;
        }
        if (raw[i] != '\r') {
            k = (int)strlen(notes[n].text);
            if (k < NOTE_LEN - 1) { notes[n].text[k] = raw[i]; notes[n].text[k + 1] = 0; }
        }
        i++;
    }
    /* a trailing newline belongs to the file, not the note */
    for (n = 0; n < MAX_NOTES; n++) {
        k = (int)strlen(notes[n].text);
        while (k > 0 && notes[n].text[k - 1] == '\n') notes[n].text[--k] = 0;
    }
    free(raw);
}

/* ------------------------------------------------------------ windows */
static int note_of(struct window *w)
{
    int i;
    for (i = 0; i < MAX_NOTES; i++)
        if (notes[i].used && notes[i].win >= 0 && win_at(notes[i].win) == w) return i;
    return -1;
}

static void note_draw(struct window *w)
{
    int i = note_of(w), x = w->x, y = w->y, lx, ly, n;
    char line[64];
    const char *p;
    if (i < 0) return;
    round_fill(x + 8, y + 8, w->w - 16, w->h - 16, 4, PAGE);
    round_frame(x + 8, y + 8, w->w - 16, w->h - 16, 4, PAGE_EDGE);
    /* the two small buttons: another note, and this one gone */
    text(F_BOLD, x + w->w - 54, y + 12, "+", INK_DIM);
    text(F_BOLD, x + w->w - 30, y + 12, "x", INK_DIM);

    /* the text, wrapped to the page */
    lx = x + 18;
    ly = y + 34;
    p = notes[i].text;
    n = 0;
    while (ly < y + w->h - 24) {
        int c = *p;
        int done = (c == 0);
        if (c == '\n' || done || text_width(F_NORMAL, line) > w->w - 44) {
            if (c != '\n' && !done) {
                /* back up to the last space, if there is one on the line */
                int k = n;
                while (k > 0 && line[k - 1] != ' ') k--;
                if (k > 0) { p -= n - k; n = k; }
            }
            line[n] = 0;
            text(F_NORMAL, lx, ly, line, INK);
            ly += text_height(F_NORMAL) + 3;
            n = 0;
            if (done) {
                if (win_focused() == notes[i].win)
                    fill(lx + text_width(F_NORMAL, line), ly - text_height(F_NORMAL) - 3, 2,
                         text_height(F_NORMAL), AMBER);
                break;
            }
            if (c == '\n') p++;
            continue;
        }
        if (n < (int)sizeof line - 2) { line[n++] = (char)c; line[n] = 0; }
        p++;
    }
}

static int note_event(struct window *w, struct event *e)
{
    int i = note_of(w), k;
    if (i < 0) return 0;
    if (e->type == EV_MOUSE_DOWN) {
        if (e->b >= 8 && e->b < 34) {
            if (e->a >= w->w - 58 && e->a < w->w - 36) { app_notes_new(); return 1; }
            if (e->a >= w->w - 34 && e->a < w->w - 12) {
                notes[i].used = 0;
                notes[i].text[0] = 0;
                save_all();
                win_close(notes[i].win);
                notes[i].win = -1;
                return 1;
            }
        }
        return 1;
    }
    if (e->type != EV_KEY) return 0;
    k = (int)strlen(notes[i].text);
    if ((e->a & ~K_SHIFT) == 0x0E) {                    /* Backspace */
        if (k) notes[i].text[k - 1] = 0;
    } else if ((e->a & ~K_SHIFT) == K_ENTER) {
        if (k < NOTE_LEN - 1) { notes[i].text[k] = '\n'; notes[i].text[k + 1] = 0; }
    } else if (e->b >= 32 && e->b < 127) {
        if (k < NOTE_LEN - 1) { notes[i].text[k] = (char)e->b; notes[i].text[k + 1] = 0; }
    } else {
        return 0;
    }
    save_all();
    return 1;
}

static void open_note(int i)
{
    if (notes[i].win >= 0) return;
    notes[i].win = win_open("Note", NOTE_W, NOTE_H, note_draw, note_event);
}

void notes_closed(int id)
{
    int i;
    for (i = 0; i < MAX_NOTES; i++)
        if (notes[i].win == id) notes[i].win = -1;
}

void app_notes_new(void)
{
    int i;
    if (!loaded) { for (i = 0; i < MAX_NOTES; i++) notes[i].win = -1; load_all(); }
    for (i = 0; i < MAX_NOTES; i++)
        if (!notes[i].used) {
            notes[i].used = 1;
            notes[i].text[0] = 0;
            open_note(i);
            save_all();
            return;
        }
}

/* all the notes there are, or a first one */
void app_notes(void)
{
    int i, any = 0;
    if (!loaded) { for (i = 0; i < MAX_NOTES; i++) notes[i].win = -1; load_all(); }
    for (i = 0; i < MAX_NOTES; i++)
        if (notes[i].used) { open_note(i); any = 1; }
    if (!any) app_notes_new();
}

/* write.c - Write: a small word processor.
 *
 * More than a notepad, much less than Word: paragraphs that wrap to the
 * window, four sizes of type, a handful of colours, bullet lists and
 * centred lines, a selection made with the pointer or Shift and the
 * arrows, cut, copy and paste, and files that come back exactly as they
 * were saved.  Saved as .EMW in a plain format of its own; saved as .TXT
 * it is plain text any DOS program can read.
 *
 * The document is one array of cells, a character and its style each,
 * with a newline cell ending every paragraph and carrying that
 * paragraph's own settings (bullet, centred).  Layout is recomputed from
 * scratch whenever it is needed; documents are small and the machine is
 * fast, and it keeps the editing code honest.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"

#define PANEL       0x1A140D
#define PANEL_LIT   0x2A2114
#define EDGE        0x4A3618
#define AMBER       0xF0A020
#define AMBER_HOT   0xFFC65A
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68
#define SELECT      0x5A4218
#define CARET       0xFFC65A

/* the page: its colour and the ink for text left in the default colour */
struct page_style { const char *name; uint32_t bg, ink, edge, select, caret; };
static const struct page_style pages[] = {
    { "Dark",  0x211A12, 0xD8C8B0, 0x4A3618, 0x5A4218, 0xFFC65A },
    { "Paper", 0xF4EEE2, 0x1E1812, 0xB8AC98, 0xF0D090, 0x8A4E10 },
    { "Sepia", 0xE6D5B8, 0x3A2A18, 0xB09A78, 0xE8BC78, 0x8A4E10 },
    { "Night", 0x0C0A08, 0xC8B89A, 0x3A2E1E, 0x4A3618, 0xFFC65A },
};
#define NPAGES ((int)(sizeof pages / sizeof pages[0]))
static int page_idx;
#define PAGE   (pages[page_idx].bg)

#define MAX_CELLS   32768
#define MAX_LINES   3000
#define TOOL_H      44
#define STATUS_H    26
#define MARGIN      18
#define INDENT      26
#define NCOLOURS    6

struct cell { unsigned char ch, face, color, flags; };  /* flags on a newline: 1 bullet, 2 centred */
struct line { int start, end, y, h, x, w; };            /* end is exclusive; x is where it starts */

static const uint32_t colours[NCOLOURS] = {
    0xD8C8B0, 0xFFC65A, 0xF0602A, 0x9BD27A, 0x6FB7E8, 0xC79BE8
};

static struct cell *cells;
static int ncells;                      /* always ends with a newline cell */
static struct line lines[MAX_LINES];
static int nlines, layout_w;

static int cursor, anchor = -1;         /* anchor >= 0: a selection from anchor to cursor */
static int cur_face = F_NORMAL, cur_color;
static int scroll_y;
static int dragging, dirty;
static char filename[64] = "\\NOTE.EMW";
static int named;                       /* the file has a name of its own: Save need not ask */
static int naming;                      /* 1 typing a name to open, 2 to save */
static char name_buf[64];
static struct cell *clip;
static int nclip;
static int win_id = -1;
static char status[80];

/* ------------------------------------------------------------ measure */
static short widths[F_COUNT][128];

static int cw(int face, int ch)
{
    char s[2];
    if (ch < 32 || ch > 126) return 0;
    if (!widths[face][ch]) {
        s[0] = (char)ch;
        s[1] = 0;
        widths[face][ch] = (short)text_width(face, s);
        if (!widths[face][ch]) widths[face][ch] = 1;
    }
    return widths[face][ch];
}

static int fh(int face) { return text_height(face) + 5; }

/* ------------------------------------------------------------ the document */
static void doc_new(void)
{
    ncells = 1;
    cells[0].ch = '\n';
    cells[0].face = F_NORMAL;
    cells[0].color = 0;
    cells[0].flags = 0;
    cursor = 0;
    anchor = -1;
    scroll_y = 0;
    dirty = 0;
}

static int para_end(int at)             /* the newline cell of the paragraph holding at */
{
    while (at < ncells - 1 && cells[at].ch != '\n') at++;
    return at;
}

static int para_start(int at)
{
    while (at > 0 && cells[at - 1].ch != '\n') at--;
    return at;
}

static void insert_cells(int at, const struct cell *src, int n)
{
    if (ncells + n > MAX_CELLS) return;
    memmove(cells + at + n, cells + at, (size_t)(ncells - at) * sizeof *cells);
    memcpy(cells + at, src, (size_t)n * sizeof *cells);
    ncells += n;
    dirty = 1;
}

static void delete_cells(int at, int n)
{
    if (at + n > ncells - 1) n = ncells - 1 - at;       /* never the final newline */
    if (n <= 0) return;
    memmove(cells + at, cells + at + n, (size_t)(ncells - at - n) * sizeof *cells);
    ncells -= n;
    dirty = 1;
}

static void sel_range(int *a, int *b)
{
    if (anchor < 0 || anchor == cursor) { *a = *b = cursor; return; }
    *a = anchor < cursor ? anchor : cursor;
    *b = anchor < cursor ? cursor : anchor;
}

static int delete_selection(void)
{
    int a, b;
    sel_range(&a, &b);
    if (a == b) return 0;
    delete_cells(a, b - a);
    cursor = a;
    anchor = -1;
    return 1;
}

static void insert_char(int ch)
{
    struct cell c;
    delete_selection();
    c.ch = (unsigned char)ch;
    c.face = (unsigned char)cur_face;
    c.color = (unsigned char)cur_color;
    c.flags = 0;
    if (ch == '\n') {
        int end = para_end(cursor);
        c.flags = cells[end].flags;                 /* the new paragraph keeps the settings */
        c.face = F_NORMAL;
    }
    insert_cells(cursor, &c, 1);
    cursor++;
}

/* the style of what is selected, or of what will be typed */
static void apply_face(int face)
{
    int a, b, i;
    sel_range(&a, &b);
    cur_face = face;
    for (i = a; i < b; i++) if (cells[i].ch != '\n') cells[i].face = (unsigned char)face;
    if (a != b) dirty = 1;
}

static void apply_color(int color)
{
    int a, b, i;
    sel_range(&a, &b);
    cur_color = color;
    for (i = a; i < b; i++) cells[i].color = (unsigned char)color;
    if (a != b) dirty = 1;
}

static void toggle_flag(int flag)
{
    int a, b, i;
    sel_range(&a, &b);
    i = para_start(a);
    for (;;) {
        int end = para_end(i);
        cells[end].flags ^= flag;
        if (end >= b || end >= ncells - 1) break;
        i = end + 1;
    }
    dirty = 1;
}

static void copy_selection(void)
{
    int a, b;
    sel_range(&a, &b);
    if (a == b) return;
    if (!clip) clip = malloc(MAX_CELLS * sizeof *clip);
    if (!clip) return;
    nclip = b - a;
    memcpy(clip, cells + a, (size_t)nclip * sizeof *clip);
}

static void paste(void)
{
    if (!clip || !nclip) return;
    delete_selection();
    insert_cells(cursor, clip, nclip);
    cursor += nclip;
}

/* ------------------------------------------------------------ layout */
static void layout(int width)
{
    int i = 0, y = 0;
    nlines = 0;
    layout_w = width;
    while (i < ncells && nlines < MAX_LINES) {
        int end = para_end(i), flags = cells[end].flags;
        int avail = width - (flags & 1 ? INDENT : 0);
        int at = i;
        do {
            int x = 0, last_space = -1, j = at, h = fh(F_NORMAL);
            struct line *L = &lines[nlines];
            while (j < end) {
                int w = cw(cells[j].face, cells[j].ch);
                if (x + w > avail && j > at) break;
                if (cells[j].ch == ' ') last_space = j;
                x += w;
                if (fh(cells[j].face) > h) h = fh(cells[j].face);
                j++;
            }
            if (j < end && last_space >= at && last_space + 1 < j) j = last_space + 1;
            L->start = at;
            L->end = j;
            L->y = y;
            L->h = h;
            L->w = 0;
            {
                int k;
                for (k = at; k < j; k++) L->w += cw(cells[k].face, cells[k].ch);
            }
            L->x = (flags & 1 ? INDENT : 0) + ((flags & 2) ? (avail - L->w) / 2 : 0);
            y += h;
            nlines++;
            at = j;
        } while (at < end && nlines < MAX_LINES);
        i = end + 1;                                /* past the newline */
    }
}

static int line_of(int pos)
{
    int i;
    for (i = 0; i < nlines; i++)
        if (pos >= lines[i].start && pos <= lines[i].end) {
            /* a position at a line's end that is also the next line's
               start belongs to the next line, unless the line ends the
               paragraph */
            if (pos == lines[i].end && i + 1 < nlines && lines[i + 1].start == pos &&
                cells[pos].ch != '\n')
                continue;
            return i;
        }
    return nlines - 1;
}

static int x_of(int line, int pos)
{
    int k, x = lines[line].x;
    for (k = lines[line].start; k < pos && k < lines[line].end; k++)
        x += cw(cells[k].face, cells[k].ch);
    return x;
}

static int pos_at(int line, int x)
{
    int k, cx = lines[line].x;
    if (line < 0) return 0;
    for (k = lines[line].start; k < lines[line].end; k++) {
        int w = cw(cells[k].face, cells[k].ch);
        if (x < cx + w / 2) return k;
        cx += w;
    }
    return lines[line].end > lines[line].start && cells[lines[line].end - 1].ch == '\n'
           ? lines[line].end - 1 : lines[line].end;
}

static void keep_cursor_visible(int view_h)
{
    int L = line_of(cursor);
    if (L < 0) return;
    if (lines[L].y < scroll_y) scroll_y = lines[L].y;
    if (lines[L].y + lines[L].h > scroll_y + view_h) scroll_y = lines[L].y + lines[L].h - view_h;
    if (scroll_y < 0) scroll_y = 0;
}

/* ------------------------------------------------------------ files */
static int save_file(const char *name)
{
    int h, i, plain = 0, ok = 1;
    const char *dot = strrchr(name, '.');
    char buf[512];
    int n = 0;
    if (dot && !strcmp(dot, ".TXT")) plain = 1;
    h = sys_create(name);
    if (h < 0) return -1;
    if (!plain) { memcpy(buf, "EMBERWRITE1\n", 12); n = 12; }
    for (i = 0; i < ncells; i++) {
        int start = (i == 0 || cells[i - 1].ch == '\n');
        if (!plain && start) {
            int end = para_end(i);
            buf[n++] = (char)('0' + cells[end].flags);
        }
        if (!plain && cells[i].ch != '\n' &&
            (start || cells[i].face != cells[i - 1].face || cells[i].color != cells[i - 1].color ||
             cells[i - 1].ch == '\n')) {
            buf[n++] = 1;
            buf[n++] = (char)('0' + cells[i].face);
            buf[n++] = (char)('0' + cells[i].color);
        }
        if (cells[i].ch == '\n' && plain) buf[n++] = '\r';
        buf[n++] = (char)cells[i].ch;
        if (n > (int)sizeof buf - 8) {
            if (sys_write(h, buf, n) != n) ok = 0;
            n = 0;
        }
    }
    if (n && sys_write(h, buf, n) != n) ok = 0;
    sys_close(h);
    if (ok) dirty = 0;
    return ok ? 0 : -1;
}

static int load_file(const char *name)
{
    int h, i, got, rich = 0, face = F_NORMAL, color = 0, at_start = 1;
    unsigned char *raw;
    h = sys_open(name);
    if (h < 0) return -1;
    raw = malloc(MAX_CELLS);
    if (!raw) { sys_close(h); return -1; }
    got = sys_read(h, raw, MAX_CELLS - 1);
    sys_close(h);
    if (got < 0) got = 0;
    doc_new();
    i = 0;
    if (got >= 12 && !memcmp(raw, "EMBERWRITE1\n", 12)) { rich = 1; i = 12; }
    ncells = 0;
    while (i < got && ncells < MAX_CELLS - 1) {
        unsigned char c = raw[i++];
        struct cell *k;
        if (rich && at_start) {
            /* the paragraph's settings, kept until its newline */
            int f = c - '0';
            at_start = 0;
            cells[ncells].flags = (unsigned char)(f >= 0 && f <= 3 ? f : 0);     /* parked here */
            continue;
        }
        if (rich && c == 1 && i + 1 < got) {
            face = raw[i++] - '0';
            color = raw[i++] - '0';
            if (face < 0 || face >= F_COUNT) face = F_NORMAL;
            if (color < 0 || color >= NCOLOURS) color = 0;
            continue;
        }
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (c != '\n' && (c < 32 || c > 126)) continue;
        k = &cells[ncells];
        k->ch = c;
        k->face = (unsigned char)(c == '\n' ? F_NORMAL : face);
        k->color = (unsigned char)color;
        if (c == '\n') {
            int s = para_start(ncells);
            k->flags = rich ? cells[s].flags : 0;
            at_start = 1;
        } else if (!rich || ncells != para_start(ncells)) {
            k->flags = 0;
        }
        ncells++;
    }
    /* the parked flags of a paragraph's first cell must not stay on it */
    for (i = 0; i < ncells; i++) if (cells[i].ch != '\n') cells[i].flags = 0;
    if (ncells == 0 || cells[ncells - 1].ch != '\n') {
        cells[ncells].ch = '\n';
        cells[ncells].face = F_NORMAL;
        cells[ncells].color = 0;
        cells[ncells].flags = 0;
        ncells++;
    }
    free(raw);
    cursor = 0;
    anchor = -1;
    scroll_y = 0;
    dirty = 0;
    return 0;
}

/* ------------------------------------------------------------ the toolbar */
struct tool { const char *label; int w; int id; };
enum { T_NEW = 1, T_OPEN, T_SAVE, T_SAVEAS, T_SMALL, T_NORMAL, T_BOLD, T_TITLE, T_COLOR, T_BULLET, T_CENTER,
       T_CUT, T_COPY, T_PASTE, T_PAGE };
static const struct tool tools[] = {
    { "New", 46, T_NEW }, { "Open", 50, T_OPEN }, { "Save", 50, T_SAVE }, { "Save as", 66, T_SAVEAS }, { 0, 10, 0 },
    { "Small", 52, T_SMALL }, { "Text", 46, T_NORMAL }, { "Bold", 46, T_BOLD }, { "Title", 48, T_TITLE }, { 0, 10, 0 },
    { "", 22 * NCOLOURS + 4, T_COLOR }, { 0, 10, 0 },
    { "List", 46, T_BULLET }, { "Centre", 58, T_CENTER }, { 0, 10, 0 },
    { "Cut", 42, T_CUT }, { "Copy", 48, T_COPY }, { "Paste", 52, T_PASTE }, { 0, 10, 0 },
    { "Page", 50, T_PAGE }, { 0, 0, -1 }
};

static int tool_at(int x, int *sub)
{
    int tx = 10, i;
    for (i = 0; tools[i].id >= 0; i++) {
        if (tools[i].id && x >= tx && x < tx + tools[i].w) {
            if (sub) *sub = (x - tx - 2) / 22;
            return tools[i].id;
        }
        tx += tools[i].w + (tools[i].id ? 4 : 0);
    }
    return 0;
}

static void draw_toolbar(struct window *w)
{
    int tx = w->x + 10, i, ty = w->y + 6;
    vgradient(w->x, w->y, w->w, TOOL_H, PANEL_LIT, PANEL);
    fill(w->x, w->y + TOOL_H - 1, w->w, 1, EDGE);
    for (i = 0; tools[i].id >= 0; i++) {
        const struct tool *t = &tools[i];
        if (!t->id) { tx += t->w; continue; }
        if (t->id == T_COLOR) {
            int c;
            for (c = 0; c < NCOLOURS; c++) {
                int sx = tx + 2 + c * 22;
                round_fill(sx, ty + 6, 18, 20, 3, c == 0 ? pages[page_idx].ink : colours[c]);
                if (c == cur_color) round_frame(sx - 1, ty + 5, 20, 22, 4, AMBER_HOT);
            }
        } else {
            int lit = (t->id == T_SMALL && cur_face == F_SMALL) || (t->id == T_NORMAL && cur_face == F_NORMAL) ||
                      (t->id == T_BOLD && cur_face == F_BOLD) || (t->id == T_TITLE && cur_face == F_TITLE);
            round_fill(tx, ty, t->w, 32, 4, lit ? 0x4A3618 : 0x2A2114);
            round_frame(tx, ty, t->w, 32, 4, lit ? AMBER : EDGE);
            text(F_SMALL, tx + (t->w - text_width(F_SMALL, t->label)) / 2,
                 ty + (32 - text_height(F_SMALL)) / 2, t->label, lit ? AMBER_HOT : TEXT);
        }
        tx += t->w + 4;
    }
}

/* ------------------------------------------------------------ drawing */
static void write_draw(struct window *w)
{
    int px = w->x + MARGIN, py = w->y + TOOL_H + 8, pw = w->w - MARGIN * 2;
    int view_h = w->h - TOOL_H - 8 - STATUS_H - 8, i, a, b;
    char buf[200];

    draw_toolbar(w);
    layout(pw - 8);
    keep_cursor_visible(view_h);
    sel_range(&a, &b);

    /* the page */
    round_fill(px - 6, py - 4, pw + 12, view_h + 8, 4, PAGE);
    round_frame(px - 6, py - 4, pw + 12, view_h + 8, 4, pages[page_idx].edge);
    {
        int ox, oy, ow, oh;
        clip_get(&ox, &oy, &ow, &oh);
        clip_set(px - 4, py - 2, pw + 8, view_h + 4);
        for (i = 0; i < nlines; i++) {
            struct line *L = &lines[i];
            int ly = py + L->y - scroll_y, k, x;
            if (ly + L->h < py - 2) continue;
            if (ly > py + view_h) break;
            /* the bullet */
            if (L->start == para_start(L->start) && (cells[para_end(L->start)].flags & 1))
                round_fill(px + 8, ly + L->h / 2 - 3, 7, 7, 3, page_idx == 0 || page_idx == 3 ? AMBER : 0x8A4E10);
            /* the selection behind the words */
            if (b > a && a < L->end && b > L->start) {
                int sa = a > L->start ? a : L->start, sb = b < L->end ? b : L->end;
                int x0 = px + x_of(i, sa), x1 = px + x_of(i, sb);
                if (sb == L->end && b > L->end) x1 = px + L->x + L->w + 6;
                fill(x0, ly, x1 - x0, L->h, pages[page_idx].select);
            }
            /* the words, in runs of one style */
            x = px + L->x;
            k = L->start;
            while (k < L->end) {
                int face = cells[k].face, color = cells[k].color, n = 0, run_w = 0;
                while (k < L->end && cells[k].face == face && cells[k].color == color &&
                       cells[k].ch != '\n' && n < (int)sizeof buf - 1) {
                    buf[n++] = (char)cells[k].ch;
                    run_w += cw(face, cells[k].ch);
                    k++;
                }
                buf[n] = 0;
                if (n) text(face, x, ly + L->h - fh(face) + 2, buf,
                            color == 0 ? pages[page_idx].ink : colours[color]);
                x += run_w;
                if (k < L->end && cells[k].ch == '\n') k++;
            }
            /* the caret */
            if (cursor >= L->start && cursor <= L->end && i == line_of(cursor) && !naming)
                fill(px + x_of(i, cursor), ly + 2, 2, L->h - 4, pages[page_idx].caret);
        }
        clip_set(ox, oy, ow, oh);
    }

    /* the status line: the file, or the name being typed */
    {
        int sy = w->y + w->h - STATUS_H;
        fill(w->x, sy, w->w, 1, EDGE);
        if (naming) {
            snprintf(buf, sizeof buf, "%s as: %s_", naming == 1 ? "Open" : "Save", name_buf);
            text(F_SMALL, w->x + MARGIN, sy + 6, buf, AMBER_HOT);
        } else {
            snprintf(buf, sizeof buf, "%s%s   %s", filename, dirty ? " *" : "", status);
            text(F_SMALL, w->x + MARGIN, sy + 6, buf, TEXT_DIM);
        }
    }
}

/* ------------------------------------------------------------ events */
static void do_tool(int id, int sub)
{
    switch (id) {
    case T_NEW:    doc_new(); strcpy(filename, "\\NOTE.EMW"); named = 0; status[0] = 0; break;
    case T_OPEN:   naming = 1; strncpy(name_buf, filename, sizeof name_buf - 1); break;
    case T_SAVE:
        if (named) {                                /* straight back to where it came from */
            if (save_file(filename) == 0) strcpy(status, "saved");
            else strcpy(status, "could not save it");
            break;
        }
        /* fall through: a new document needs a name */
    case T_SAVEAS: naming = 2; strncpy(name_buf, filename, sizeof name_buf - 1); break;
    case T_SMALL:  apply_face(F_SMALL); break;
    case T_NORMAL: apply_face(F_NORMAL); break;
    case T_BOLD:   apply_face(F_BOLD); break;
    case T_TITLE:  apply_face(F_TITLE); break;
    case T_COLOR:  if (sub >= 0 && sub < NCOLOURS) apply_color(sub); break;
    case T_BULLET: toggle_flag(1); break;
    case T_CENTER: toggle_flag(2); break;
    case T_CUT:    copy_selection(); delete_selection(); break;
    case T_COPY:   copy_selection(); break;
    case T_PASTE:  paste(); break;
    case T_PAGE:   page_idx = (page_idx + 1) % NPAGES; break;
    }
}

static void finish_naming(void)
{
    char *p;
    for (p = name_buf; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    if (naming == 1) {
        if (load_file(name_buf) == 0) { strcpy(filename, name_buf); named = 1; strcpy(status, "opened"); }
        else strcpy(status, "could not open it");
    } else {
        if (save_file(name_buf) == 0) { strcpy(filename, name_buf); named = 1; strcpy(status, "saved"); }
        else strcpy(status, "could not save it");
    }
    naming = 0;
}

static void move_cursor(int to, int extend)
{
    if (to < 0) to = 0;
    if (to > ncells - 1) to = ncells - 1;
    if (extend) { if (anchor < 0) anchor = cursor; }
    else anchor = -1;
    cursor = to;
}

static int write_event(struct window *w, struct event *e)
{
    int pw = w->w - MARGIN * 2, view_h = w->h - TOOL_H - 8 - STATUS_H - 8;

    if (e->type == EV_MOUSE_DOWN) {
        if (e->b < TOOL_H) {
            int sub = -1, id = tool_at(e->a, &sub);
            if (id) do_tool(id, sub);
            return 1;
        }
        if (e->b >= TOOL_H + 8 && e->b < TOOL_H + 8 + view_h) {
            int i, L = -1, y = e->b - TOOL_H - 8 + scroll_y;
            layout(pw - 8);
            for (i = 0; i < nlines; i++)
                if (y >= lines[i].y && y < lines[i].y + lines[i].h) { L = i; break; }
            if (L < 0 && nlines) L = y < 0 ? 0 : nlines - 1;
            if (L >= 0) {
                int pos = pos_at(L, e->a - MARGIN);
                if (e->dbl) {                       /* a word */
                    int s = pos, t = pos;
                    while (s > 0 && cells[s - 1].ch != ' ' && cells[s - 1].ch != '\n') s--;
                    while (t < ncells - 1 && cells[t].ch != ' ' && cells[t].ch != '\n') t++;
                    anchor = s;
                    cursor = t;
                } else if (input_shift_held()) {       /* Shift-click: out to here */
                    if (anchor < 0) anchor = cursor;
                    cursor = pos;
                } else {
                    cursor = anchor = pos;
                    dragging = 1;
                }
            }
            naming = 0;
            return 1;
        }
        return 1;
    }
    if (e->type == EV_MOUSE_MOVE) {
        if (dragging && (mouse_buttons & 1)) {
            int i, L = -1, y = e->b - TOOL_H - 8 + scroll_y;
            for (i = 0; i < nlines; i++)
                if (y >= lines[i].y && y < lines[i].y + lines[i].h) { L = i; break; }
            if (L < 0 && nlines) L = y < 0 ? 0 : nlines - 1;
            if (L >= 0) cursor = pos_at(L, e->a - MARGIN);
            return 1;
        }
        if (dragging) {
            dragging = 0;
            if (anchor == cursor) anchor = -1;
        }
        return 0;
    }
    if (e->type != EV_KEY) return 0;

    /* a name being typed */
    if (naming) {
        int n = (int)strlen(name_buf);
        int key = e->a & ~K_SHIFT;
        if (key == K_ESC) { naming = 0; return 1; }
        if (key == K_ENTER) { finish_naming(); return 1; }
        if (key == 0x0E) { if (n) name_buf[n - 1] = 0; return 1; }
        if (e->b >= 32 && e->b < 127 && n < (int)sizeof name_buf - 1) {
            name_buf[n] = (char)e->b;
            name_buf[n + 1] = 0;
        }
        return 1;
    }

    {
        int key = e->a & ~K_SHIFT, extend = (e->a & K_SHIFT) != 0;
        layout(pw - 8);
        switch (key) {
        case K_LEFT:  move_cursor(cursor - 1, extend); return 1;
        case K_RIGHT: move_cursor(cursor + 1, extend); return 1;
        case K_UP:
        case K_DOWN: {
            int L = line_of(cursor), x = x_of(L, cursor);
            int to = key == K_UP ? L - 1 : L + 1;
            if (to < 0 || to >= nlines) return 1;
            move_cursor(pos_at(to, x), extend);
            return 1;
        }
        case K_HOME: move_cursor(lines[line_of(cursor)].start, extend); return 1;
        case K_END: {
            int L = line_of(cursor), end = lines[L].end;
            if (end > lines[L].start && cells[end - 1].ch == '\n') end--;
            move_cursor(end, extend);
            return 1;
        }
        case K_PGUP:
        case K_PGDN: {
            int L = line_of(cursor), x = x_of(L, cursor), to = L, moved = 0;
            while (to > 0 && to < nlines - 1 && moved < view_h) {
                to += key == K_PGUP ? -1 : 1;
                moved += lines[to].h;
            }
            move_cursor(pos_at(to, x), extend);
            return 1;
        }
        case K_ESC:   anchor = -1; return 1;
        case 0x3C:    do_tool(T_SAVE, 0); return 1;      /* F2: save */
        case 0x3D:    do_tool(T_OPEN, 0); return 1;      /* F3: open */
        case 0x3E:    do_tool(T_SAVEAS, 0); return 1;    /* F4: save as */
        case 0x0E:                                      /* Backspace */
            if (!delete_selection() && cursor > 0) { cursor--; delete_cells(cursor, 1); }
            return 1;
        case K_DEL:
            if (!delete_selection() && cursor < ncells - 1) delete_cells(cursor, 1);
            return 1;
        case K_ENTER: insert_char('\n'); return 1;
        case K_TAB:   insert_char(' '); insert_char(' '); insert_char(' '); insert_char(' '); return 1;
        default:
            if (e->b >= 32 && e->b < 127) { insert_char(e->b); return 1; }
            return 0;
        }
    }
}

/* ------------------------------------------------------------ opening */
void write_closed(int id)
{
    if (id == win_id) win_id = -1;
}

static int ensure_document(void)
{
    if (!cells) {
        cells = malloc(MAX_CELLS * sizeof *cells);
        if (!cells) return -1;
        doc_new();
    }
    return 0;
}

void app_write(void)
{
    if (ensure_document() != 0) return;
    if (win_id >= 0) return;
    win_id = win_open("Write", 960, 560, write_draw, write_event);
}

void app_write_open(const char *path)
{
    if (ensure_document() != 0) return;
    if (load_file(path) == 0) {
        strncpy(filename, path, sizeof filename - 1);
        named = 1;
        strcpy(status, "opened");
    } else {
        strcpy(status, "could not open it");
    }
    if (win_id < 0) win_id = win_open("Write", 960, 560, write_draw, write_event);
}

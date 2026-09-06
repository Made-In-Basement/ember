/* files.c - the file window: a tree of folders on the left, their contents
 * on the right.
 *
 * The tree is a flat list of rows, each knowing how deep it sits and what
 * its full path is.  Opening a folder splices its subfolders in after it
 * and closing one takes them out again, so nothing is read from the disk
 * until somebody asks to see it.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"

#define PANEL_LIT   0x241C12
#define WELL        0x120D08
#define EDGE        0x3A2C18
#define AMBER       0xF0A020
#define AMBER_DIM   0x8A5E16
#define AMBER_HOT   0xFFC65A
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68

#define ROW_H       22
#define TREE_W      210
#define HEAD_H      34
#define MAX_TREE    192
#define MAX_LIST    320
#define PATH_MAX    96

struct tree_row {
    char name[40];
    char path[PATH_MAX];
    short depth;
    short open;
    short has_kids;
};

struct list_row {
    char name[52];
    uint32_t size;
    uint16_t date, time;
    short is_dir;
};

static struct tree_row tree[MAX_TREE];
static int tree_count, tree_sel, tree_top;
static struct list_row list[MAX_LIST];
static int list_count, list_sel, list_top;
static char cur_path[PATH_MAX];
static int focus_tree = 1;

/* ---------------------------------------------------------------- disk */
static void join(char *out, const char *dir, const char *name)
{
    out[0] = 0;
    if (dir && dir[0]) {
        strncpy(out, dir, PATH_MAX - 1);
        out[PATH_MAX - 1] = 0;
        if (out[strlen(out) - 1] != '\\')
            strcat(out, "\\");
    }
    strcat(out, name);
}

/* does this folder hold any folders of its own? */
static int has_subfolders(const char *path)
{
    struct dos_find f;
    char pattern[PATH_MAX + 8];
    int rc;
    join(pattern, path, "*.*");
    for (rc = sys_findfirst(pattern, &f); rc == 0; rc = sys_findnext(&f)) {
        if (!(f.attr & 0x10) || (f.attr & 0x08)) continue;
        if (f.name[0] == '.') continue;
        return 1;
    }
    return 0;
}

static void list_read(const char *path)
{
    struct dos_find f;
    char pattern[PATH_MAX + 8];
    int rc, i, j;
    list_count = 0;
    strncpy(cur_path, path, PATH_MAX - 1);
    cur_path[PATH_MAX - 1] = 0;
    join(pattern, path, "*.*");
    for (rc = sys_findfirst(pattern, &f); rc == 0 && list_count < MAX_LIST;
         rc = sys_findnext(&f)) {
        char longname[84];
        const char *use = f.name;
        if (f.attr & 0x08) continue;                    /* the volume label */
        if (f.name[0] == '.' && f.name[1] == 0) continue;
        if (f.name[0] == '.' && f.name[1] == '.' && !path[0]) continue;
        if (sys_long_name(longname, sizeof longname) > 0) use = longname;
        strncpy(list[list_count].name, use, sizeof list[0].name - 1);
        list[list_count].name[sizeof list[0].name - 1] = 0;
        list[list_count].size = f.size;
        list[list_count].date = f.date;
        list[list_count].time = f.time;
        list[list_count].is_dir = (f.attr & 0x10) != 0;
        list_count++;
    }
    /* folders first, then by name */
    for (i = 1; i < list_count; i++) {
        struct list_row key = list[i];
        for (j = i; j > 0; j--) {
            struct list_row *p = &list[j - 1];
            int after = (p->is_dir != key.is_dir) ? !p->is_dir
                      : strcasecmp(p->name, key.name) > 0;
            if (!after) break;
            list[j] = *p;
        }
        list[j] = key;
    }
    list_sel = 0;
    list_top = 0;
}

/* Put the folders of tree[i] into the list just after it. */
static void tree_expand(int i)
{
    struct dos_find f;
    char pattern[PATH_MAX + 8];
    int rc, at = i + 1, depth = tree[i].depth + 1, n = 0;
    struct tree_row kids[64];

    join(pattern, tree[i].path, "*.*");
    for (rc = sys_findfirst(pattern, &f); rc == 0 && n < 64; rc = sys_findnext(&f)) {
        char longname[84];
        const char *use = f.name;
        if (!(f.attr & 0x10) || (f.attr & 0x08)) continue;
        if (f.name[0] == '.') continue;
        if (sys_long_name(longname, sizeof longname) > 0) use = longname;
        strncpy(kids[n].name, use, sizeof kids[0].name - 1);
        kids[n].name[sizeof kids[0].name - 1] = 0;
        join(kids[n].path, tree[i].path, kids[n].name);
        kids[n].depth = (short)depth;
        kids[n].open = 0;
        kids[n].has_kids = 0;
        n++;
    }
    /* Only now ask which of them have folders of their own.  A directory
       search keeps its place in one shared area, so starting a second one
       inside the first would lose the first's place and cut it short. */
    {
        int c;
        for (c = 0; c < n; c++)
            kids[c].has_kids = (short)has_subfolders(kids[c].path);
    }
    /* by name, so the tree reads the way the list does */
    {
        int a, b;
        for (a = 1; a < n; a++) {
            struct tree_row key = kids[a];
            for (b = a; b > 0 && strcasecmp(kids[b - 1].name, key.name) > 0; b--)
                kids[b] = kids[b - 1];
            kids[b] = key;
        }
    }
    if (tree_count + n > MAX_TREE) n = MAX_TREE - tree_count;
    if (n > 0) {
        int k;
        for (k = tree_count - 1; k >= at; k--)
            tree[k + n] = tree[k];
        for (k = 0; k < n; k++)
            tree[at + k] = kids[k];
        tree_count += n;
    }
    tree[i].open = 1;
    tree[i].has_kids = (short)(n > 0);
}

static void tree_collapse(int i)
{
    int end = i + 1;
    while (end < tree_count && tree[end].depth > tree[i].depth) end++;
    if (end > i + 1) {
        int k;
        for (k = end; k < tree_count; k++)
            tree[k - (end - i - 1)] = tree[k];
        tree_count -= end - i - 1;
    }
    tree[i].open = 0;
}

static void tree_init(void)
{
    memset(tree, 0, sizeof tree);
    strcpy(tree[0].name, "Disk C");
    tree[0].path[0] = 0;
    tree[0].depth = 0;
    tree[0].has_kids = 1;
    tree_count = 1;
    tree_sel = 0;
    tree_top = 0;
    tree_expand(0);
    list_read("");
}

/* ---------------------------------------------------------------- drawing */
static void draw_head(struct window *w)
{
    char buf[PATH_MAX + 16];
    fill(w->x, w->y, w->w, HEAD_H, PANEL_LIT);
    fill(w->x, w->y + HEAD_H - 1, w->w, 1, EDGE);
    snprintf(buf, sizeof buf, "C:\\%s", cur_path);
    text(F_SMALL, w->x + 14, w->y + 8, buf, AMBER);
    snprintf(buf, sizeof buf, "%d item%s", list_count, list_count == 1 ? "" : "s");
    text(F_SMALL, w->x + w->w - 14 - text_width(F_SMALL, buf), w->y + 8,
         buf, TEXT_DIM);
}

/* a small triangle: right when shut, down when open */
static void twisty(int x, int y, int open, uint32_t c)
{
    int p[6];
    if (open) {
        p[0] = x;     p[1] = y - 2;
        p[2] = x + 8; p[3] = y - 2;
        p[4] = x + 4; p[5] = y + 3;
    } else {
        p[0] = x + 1; p[1] = y - 4;
        p[2] = x + 6; p[3] = y;
        p[4] = x + 1; p[5] = y + 4;
    }
    poly_fill(p, 3, c);
}

static void files_draw(struct window *w)
{
    int rows = (w->h - HEAD_H) / ROW_H, i;
    int lx = w->x + TREE_W + 1;
    int lw = w->w - TREE_W - 1;
    char buf[48];

    draw_head(w);

    /* the tree, in its own slightly sunken column */
    fill(w->x, w->y + HEAD_H, TREE_W, w->h - HEAD_H, WELL);
    fill(w->x + TREE_W, w->y + HEAD_H, 1, w->h - HEAD_H, EDGE);
    if (tree_sel < tree_top) tree_top = tree_sel;
    if (tree_sel >= tree_top + rows) tree_top = tree_sel - rows + 1;
    for (i = 0; i < rows; i++) {
        int idx = tree_top + i, ry = w->y + HEAD_H + 4 + i * ROW_H;
        int tx;
        uint32_t c = TEXT;
        if (idx >= tree_count) break;
        tx = w->x + 8 + tree[idx].depth * 14;
        if (idx == tree_sel) {
            fill(w->x + 2, ry - 2, TREE_W - 4, ROW_H, 0x2A1D0C);
            fill(w->x + 2, ry - 2, 3, ROW_H, AMBER);
            c = AMBER_HOT;
        }
        if (tree[idx].has_kids)
            twisty(tx, ry + ROW_H / 2 - 3, tree[idx].open,
                   idx == tree_sel ? AMBER_HOT : AMBER_DIM);
        text_clipped(F_SMALL, tx + 14, ry, TREE_W - (tx - w->x) - 20,
                     tree[idx].name, c);
    }

    /* and the contents */
    if (list_sel < list_top) list_top = list_sel;
    if (list_sel >= list_top + rows) list_top = list_sel - rows + 1;
    for (i = 0; i < rows; i++) {
        int idx = list_top + i, ry = w->y + HEAD_H + 4 + i * ROW_H;
        uint32_t c;
        if (idx >= list_count) break;
        c = list[idx].is_dir ? AMBER : TEXT;
        if (idx == list_sel && !focus_tree) {
            fill(lx + 2, ry - 2, lw - 4, ROW_H, 0x2A1D0C);
            fill(lx + 2, ry - 2, 3, ROW_H, AMBER);
            c = AMBER_HOT;
        }
        text_clipped(F_NORMAL, lx + 16, ry - 2, lw - 190, list[idx].name, c);
        if (list[idx].is_dir) {
            text(F_SMALL, lx + lw - 170, ry, "folder", TEXT_DIM);
        } else {
            if (list[idx].size >= 1024)
                snprintf(buf, sizeof buf, "%u KB", (unsigned)(list[idx].size / 1024));
            else
                snprintf(buf, sizeof buf, "%u B", (unsigned)list[idx].size);
            text(F_SMALL, lx + lw - 100 - text_width(F_SMALL, buf), ry, buf,
                 TEXT_DIM);
        }
        /* the date, as the directory records it */
        snprintf(buf, sizeof buf, "%02d-%02d-%04d",
                 (list[idx].date >> 5) & 0x0F, list[idx].date & 0x1F,
                 1980 + ((list[idx].date >> 9) & 0x7F));
        text(F_SMALL, lx + lw - 90, ry, buf, TEXT_DIM);
    }
}

/* ---------------------------------------------------------------- events */
static void enter_list_row(void)
{
    char path[PATH_MAX];
    if (list_sel >= list_count || !list[list_sel].is_dir) return;
    if (!strcmp(list[list_sel].name, "..")) {
        char *p = strrchr(cur_path, '\\');
        if (p) *p = 0;
        else cur_path[0] = 0;
        list_read(cur_path);
    } else {
        join(path, cur_path, list[list_sel].name);
        list_read(path);
    }
}

static int files_event(struct window *w, struct event *e)
{
    int rows = (w->h - HEAD_H) / ROW_H;
    if (e->type == EV_MOUSE_DOWN) {
        int row = (e->b - HEAD_H - 4) / ROW_H;
        if (e->b < HEAD_H) return 0;
        if (e->a < TREE_W) {                            /* the tree */
            int idx = tree_top + row;
            focus_tree = 1;
            if (idx >= 0 && idx < tree_count) {
                int tx = 8 + tree[idx].depth * 14;
                tree_sel = idx;
                if (tree[idx].has_kids && e->a >= tx && e->a < tx + 14) {
                    if (tree[idx].open) tree_collapse(idx);
                    else tree_expand(idx);
                } else {
                    list_read(tree[idx].path);
                }
            }
            return 1;
        }
        {                                               /* the contents */
            int idx = list_top + row;
            focus_tree = 0;
            if (idx >= 0 && idx < list_count) {
                if (e->dbl && idx == list_sel) enter_list_row();    /* a double-click opens */
                else list_sel = idx;
            }
        }
        return 1;
    }
    if (e->type == EV_KEY) {
        if (e->a == K_TAB) { focus_tree = !focus_tree; return 1; }
        if (focus_tree) {
            if (e->a == K_UP && tree_sel > 0) tree_sel--;
            else if (e->a == K_DOWN && tree_sel + 1 < tree_count) tree_sel++;
            else if (e->a == K_RIGHT) {
                if (tree[tree_sel].has_kids && !tree[tree_sel].open)
                    tree_expand(tree_sel);
            } else if (e->a == K_LEFT) {
                if (tree[tree_sel].open) tree_collapse(tree_sel);
            } else if (e->a == K_ENTER) {
                list_read(tree[tree_sel].path);
                focus_tree = 0;
            } else return 0;
            return 1;
        }
        if (e->a == K_UP && list_sel > 0) list_sel--;
        else if (e->a == K_DOWN && list_sel + 1 < list_count) list_sel++;
        else if (e->a == K_ENTER) enter_list_row();
        else return 0;
        return 1;
    }
    (void)rows;
    return 0;
}

void app_files(void)
{
    int id = win_open("Files", 760, 460, files_draw, files_event);
    if (id < 0) return;
    tree_init();
}

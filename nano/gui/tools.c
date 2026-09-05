/* tools.c - the calculator and the command prompt.
 *
 * The prompt is a real one: the commands it runs go through the same DOS
 * calls the shell outside uses, so what it shows is what is actually on
 * the disk.  It cannot start a program yet -- that needs the kernel to be
 * able to hand a real-mode program control and take it back -- so it says
 * so rather than pretending.
 */
#include <nanolibc.h>
#include "nano.h"
#include "draw.h"
#include "input.h"
#include "shell.h"

#define WELL        0x0C0906
#define WELL_EDGE   0x33271A
#define PANEL_LIT   0x241C12
#define EDGE        0x3A2C18
#define AMBER       0xF0A020
#define AMBER_DIM   0x8A5E16
#define AMBER_HOT   0xFFC65A
#define TEXT        0xD8C8B0
#define TEXT_DIM    0x8A7C68

/* ---------------------------------------------------------------- calculator */
#define KEY_W 56
#define KEY_H 42
#define KEY_GAP 6
#define PAD_X 16
#define PAD_Y 76

static char calc_display[32] = "0";
static long calc_acc;
static char calc_op;
static int calc_fresh = 1;
static int calc_down = -1;

static const char *keys[20] = {
    "7", "8", "9", "/",
    "4", "5", "6", "*",
    "1", "2", "3", "-",
    "0", ".", "=", "+",
    "C", "\x7F", "%", "\x1A"        /* clear, backspace, percent, sign */
};

static void calc_show(long v)
{
    snprintf(calc_display, sizeof calc_display, "%ld", v);
}

static long calc_value(void)
{
    long v = 0;
    const char *s = calc_display;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static void calc_apply(char op)
{
    long v = calc_value();
    if (calc_op == '+') calc_acc += v;
    else if (calc_op == '-') calc_acc -= v;
    else if (calc_op == '*') calc_acc *= v;
    else if (calc_op == '/') calc_acc = v ? calc_acc / v : 0;
    else calc_acc = v;
    calc_op = op;
    calc_show(calc_acc);
    calc_fresh = 1;
}

static void calc_key(int k)
{
    const char *label = keys[k];
    char c = label[0];
    int len = (int)strlen(calc_display);

    if (c >= '0' && c <= '9') {
        if (calc_fresh) { calc_display[0] = 0; len = 0; calc_fresh = 0; }
        if (len < 15) { calc_display[len] = c; calc_display[len + 1] = 0; }
        if (!strcmp(calc_display, "00")) strcpy(calc_display, "0");
        return;
    }
    switch (c) {
    case '+': case '-': case '*': case '/':
        calc_apply(c);
        break;
    case '=':
        calc_apply(0);
        calc_op = 0;
        break;
    case 'C':
        strcpy(calc_display, "0");
        calc_acc = 0;
        calc_op = 0;
        calc_fresh = 1;
        break;
    case 0x7F:                                  /* backspace */
        if (len > 1) calc_display[len - 1] = 0;
        else strcpy(calc_display, "0");
        break;
    case '%':
        calc_show(calc_value() / 100);
        calc_fresh = 1;
        break;
    case 0x1A:                                  /* change the sign */
        calc_show(-calc_value());
        calc_fresh = 0;
        break;
    default:
        break;
    }
}

static void calc_draw(struct window *w)
{
    int i;
    /* the readout, sunk in */
    round_fill(w->x + PAD_X, w->y + 16, w->w - PAD_X * 2, 48, 3, WELL);
    round_frame(w->x + PAD_X, w->y + 16, w->w - PAD_X * 2, 48, 3, WELL_EDGE);
    text(F_CLOCK, w->x + w->w - PAD_X - 14 - text_width(F_CLOCK, calc_display),
         w->y + 14, calc_display, AMBER_HOT);

    for (i = 0; i < 20; i++) {
        int col = i % 4, row = i / 4;
        int bx = w->x + PAD_X + col * (KEY_W + KEY_GAP);
        int by = w->y + PAD_Y + row * (KEY_H + KEY_GAP);
        const char *label = keys[i];
        char shown[4];
        int is_op = (label[0] < '0' || label[0] > '9') && label[0] != '.';
        uint32_t face = (i == calc_down) ? 0x3A2A12 : (is_op ? 0x241C12 : 0x1F1810);
        round_fill(bx, by, KEY_W, KEY_H, 4, face);
        round_frame(bx, by, KEY_W, KEY_H, 4, i == calc_down ? AMBER : EDGE);
        fill(bx + 4, by + 1, KEY_W - 8, 1, 0x4A3A20);       /* a lit lip */
        shown[0] = label[0];
        shown[1] = 0;
        if (label[0] == 0x7F) strcpy(shown, "<");
        else if (label[0] == 0x1A) strcpy(shown, "+-");
        text(F_BOLD, bx + KEY_W / 2 - text_width(F_BOLD, shown) / 2,
             by + (KEY_H - text_height(F_BOLD)) / 2,
             shown, is_op ? AMBER : TEXT);
    }
}

static int calc_hit(struct window *w, int x, int y)
{
    int col, row;
    (void)w;
    if (x < PAD_X || y < PAD_Y) return -1;
    col = (x - PAD_X) / (KEY_W + KEY_GAP);
    row = (y - PAD_Y) / (KEY_H + KEY_GAP);
    if (col < 0 || col > 3 || row < 0 || row > 4) return -1;
    if ((x - PAD_X) % (KEY_W + KEY_GAP) > KEY_W) return -1;
    if ((y - PAD_Y) % (KEY_H + KEY_GAP) > KEY_H) return -1;
    return row * 4 + col;
}

static int calc_event(struct window *w, struct event *e)
{
    if (e->type == EV_MOUSE_DOWN) {
        int k = calc_hit(w, e->a, e->b);
        if (k >= 0) { calc_down = k; calc_key(k); return 1; }
        return 0;
    }
    if (e->type == EV_MOUSE_UP) {
        if (calc_down >= 0) { calc_down = -1; return 1; }
        return 0;
    }
    if (e->type == EV_KEY) {
        int c = e->b, i;
        if (c == 13) c = '=';
        if (c == 8) c = 0x7F;
        if (c == 27) c = 'C';
        for (i = 0; i < 20; i++)
            if (keys[i][0] == c) { calc_key(i); return 1; }
        return 0;
    }
    return 0;
}

void app_calc(void)
{
    win_open("Calculator", 4 * KEY_W + 3 * KEY_GAP + PAD_X * 2,
             PAD_Y + 5 * (KEY_H + KEY_GAP) + 12, calc_draw, calc_event);
}

/* ---------------------------------------------------------------- prompt */
#define TERM_ROWS 200
#define TERM_COLS 96
#define LINE_H    18

static char term[TERM_ROWS][TERM_COLS];
static int term_count, term_view;
static char input_line[80];
static int input_len;
static char term_dir[96];

static void say(const char *s)
{
    int i;
    if (term_count >= TERM_ROWS) {              /* scroll the oldest away */
        for (i = 1; i < TERM_ROWS; i++)
            memcpy(term[i - 1], term[i], TERM_COLS);
        term_count = TERM_ROWS - 1;
    }
    strncpy(term[term_count], s, TERM_COLS - 1);
    term[term_count][TERM_COLS - 1] = 0;
    term_count++;
}

static void sayf(const char *fmt, ...)
{
    char buf[TERM_COLS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    say(buf);
}

static void term_join(char *out, const char *name)
{
    out[0] = 0;
    if (name[0] == '\\') { strcpy(out, name + 1); return; }
    if (term_dir[0]) {
        strcpy(out, term_dir);
        strcat(out, "\\");
    }
    strcat(out, name);
}

static void cmd_dir(const char *arg)
{
    struct dos_find f;
    char pattern[128], path[112];
    int rc, files = 0, dirs = 0;
    unsigned long bytes = 0;
    if (arg[0]) term_join(path, arg);
    else strcpy(path, term_dir);
    if (path[0]) {
        strcpy(pattern, path);
        strcat(pattern, "\\*.*");
    } else {
        strcpy(pattern, "*.*");
    }
    sayf(" Directory of C:\\%s", path);
    say("");
    for (rc = sys_findfirst(pattern, &f); rc == 0; rc = sys_findnext(&f)) {
        char longname[84];
        const char *use = f.name;
        if (f.attr & 0x08) continue;
        if (sys_long_name(longname, sizeof longname) > 0) use = longname;
        if (f.attr & 0x10) {
            sayf("  %-40s   <DIR>", use);
            dirs++;
        } else {
            sayf("  %-40s %8u", use, (unsigned)f.size);
            files++;
            bytes += f.size;
        }
    }
    say("");
    sayf("  %d file(s), %lu bytes,  %d director%s", files, bytes, dirs,
         dirs == 1 ? "y" : "ies");
}

static void cmd_type(const char *arg)
{
    char path[112], buf[512];
    int fd, n, i, col = 0;
    char line[TERM_COLS];
    if (!arg[0]) { say("type what?"); return; }
    term_join(path, arg);
    fd = sys_open(path);
    if (fd < 0) { sayf("%s: not found", arg); return; }
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n' || col >= TERM_COLS - 2) {
                line[col] = 0;
                say(line);
                col = 0;
                if (c != '\n') line[col++] = c;
            } else if (c == '\t') {
                while (col < TERM_COLS - 2 && (col % 8)) line[col++] = ' ';
            } else if ((unsigned char)c >= 32) {
                line[col++] = c;
            }
        }
    }
    if (col) { line[col] = 0; say(line); }
    sys_close(fd);
}

static void cmd_cd(const char *arg)
{
    struct dos_find f;
    char path[112], pattern[128];
    if (!arg[0]) { sayf("C:\\%s", term_dir); return; }
    if (!strcmp(arg, "..")) {
        char *p = strrchr(term_dir, '\\');
        if (p) *p = 0;
        else term_dir[0] = 0;
        return;
    }
    if (!strcmp(arg, "\\")) { term_dir[0] = 0; return; }
    term_join(path, arg);
    strcpy(pattern, path);
    strcat(pattern, "\\*.*");
    if (sys_findfirst(pattern, &f) != 0) { sayf("%s: no such folder", arg); return; }
    strncpy(term_dir, path, sizeof term_dir - 1);
    term_dir[sizeof term_dir - 1] = 0;
}

static const char *skip_word(const char *s)
{
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    return s;
}

static void run_command(const char *line)
{
    char word[32];
    const char *arg;
    int i = 0;
    while (line[i] && line[i] != ' ' && i < 30) {
        word[i] = (line[i] >= 'a' && line[i] <= 'z') ? line[i] - 32 : line[i];
        i++;
    }
    word[i] = 0;
    arg = skip_word(line);

    if (!word[0]) return;
    if (!strcmp(word, "DIR")) cmd_dir(arg);
    else if (!strcmp(word, "TYPE")) cmd_type(arg);
    else if (!strcmp(word, "CD")) cmd_cd(arg);
    else if (!strcmp(word, "CLS")) { term_count = 0; term_view = 0; }
    else if (!strcmp(word, "VER")) say("Ember version 1.2");
    else if (!strcmp(word, "MKDIR") || !strcmp(word, "MD")) {
        char path[112];
        term_join(path, arg);
        sayf(sys_mkdir(path) == 0 ? "made %s" : "could not make %s", arg);
    } else if (!strcmp(word, "DEL")) {
        char path[112];
        term_join(path, arg);
        sayf(sys_unlink(path) == 0 ? "deleted %s" : "could not delete %s", arg);
    } else if (!strcmp(word, "HELP")) {
        say("DIR, CD, TYPE, DEL, MKDIR, CLS, VER, HELP");
        say("Running programs needs the DOS prompt outside the desktop:");
        say("leave with F10, or Exit to DOS in the menu.");
    } else {
        sayf("%s: not a command here.  HELP lists what is.", word);
    }
}

static void term_draw(struct window *w)
{
    int rows = (w->h - 40) / LINE_H, i, first;
    char prompt[TERM_COLS];

    fill(w->x, w->y, w->w, w->h, WELL);
    first = term_count - rows + 1 - term_view;
    if (first < 0) first = 0;
    for (i = 0; i < rows - 1; i++) {
        int idx = first + i;
        if (idx >= term_count) break;
        text(F_SMALL, w->x + 12, w->y + 8 + i * LINE_H, term[idx], TEXT);
    }
    snprintf(prompt, sizeof prompt, "C:\\%s> %s", term_dir, input_line);
    text(F_SMALL, w->x + 12, w->y + 8 + (rows - 1) * LINE_H, prompt, AMBER_HOT);
    /* a block where the next letter goes */
    fill(w->x + 12 + text_width(F_SMALL, prompt) + 2,
         w->y + 8 + (rows - 1) * LINE_H + 2, 8, text_height(F_SMALL) - 4, AMBER_DIM);
}

static int term_event(struct window *w, struct event *e)
{
    (void)w;
    if (e->type != EV_KEY) return 0;
    if (e->a == K_UP) { term_view++; return 1; }
    if (e->a == K_DOWN) { if (term_view > 0) term_view--; return 1; }
    if (e->b == 13) {
        char line[80];
        strcpy(line, input_line);
        sayf("C:\\%s> %s", term_dir, line);
        input_line[0] = 0;
        input_len = 0;
        term_view = 0;
        run_command(line);
        return 1;
    }
    if (e->b == 8) {
        if (input_len > 0) input_line[--input_len] = 0;
        return 1;
    }
    if (e->b >= 32 && e->b < 127 && input_len < (int)sizeof input_line - 1) {
        input_line[input_len++] = (char)e->b;
        input_line[input_len] = 0;
        return 1;
    }
    return 0;
}

void app_prompt(void)
{
    if (term_count == 0) {
        say("Ember command prompt.  HELP lists the commands.");
        say("");
    }
    win_open("Prompt", 720, 440, term_draw, term_event);
}

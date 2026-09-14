/* REVERSI.N32 - Othello for Ember, ported from borkit/reversi (game.js).
 *
 * The engine is the browser game's, move for move: the same positional
 * weights, the same evaluation (mobility early, discs late), the same
 * alpha-beta search with corner-first move ordering.  Only the front end is
 * new: VGA mode 13h instead of the DOM, drawn into a back buffer and copied
 * to 0xA0000 a frame at a time, with the BIOS 8x8 font for the panel.
 */
#include <nanolibc.h>
#include "nano.h"
#include "engine.h"

#define SCRW 320
#define SCRH 200

/* palette slots */
#define C_BG      0
#define C_FELT    1
#define C_FELT2   2
#define C_GRID    3
#define C_BLACK   4
#define C_WHITE   5
#define C_HINT    6
#define C_CURSOR  7
#define C_TEXT    8
#define C_DIM     9
#define C_PANEL  10
#define C_EMBER  11
#define C_LAST   12
#define C_SHADOW 13


/* ===== game state ===== */
static Board board;
static int turn = BLACK;
static int mode_ai = 1;                 /* 1 = vs computer, 0 = two players */
static int difficulty = 3;              /* search depth; 1 = random-ish */
static int human_color = BLACK;
static int show_hints = 1;
static int game_over;
static int cur_r = 2, cur_c = 3;        /* keyboard cursor */
static int last_r = -1, last_c = -1;
static int passed_msg;

#define MAX_HIST 80
static struct { Board b; int turn; int lr, lc, log_n; } hist[MAX_HIST];
static int hist_n;

#define MAX_LOG 64
static struct { signed char r, c, color, pass; int flips; } movelog[MAX_LOG];
static int log_n;

/* ===== VGA mode 13h ===== */
static unsigned char fb[SCRW * SCRH];
static unsigned char *vga = (unsigned char *)0xA0000;
static const unsigned char *rom_font;    /* 8x8, 256 glyphs */

static void set_dac(int i, int r, int g, int b)
{
    outb(0x3C8, (unsigned char)i);
    outb(0x3C9, (unsigned char)(r >> 2));
    outb(0x3C9, (unsigned char)(g >> 2));
    outb(0x3C9, (unsigned char)(b >> 2));
}

static void palette(void)
{
    set_dac(C_BG,      12,  14,  20);
    set_dac(C_FELT,    22,  78,  52);
    set_dac(C_FELT2,   28,  92,  62);
    set_dac(C_GRID,    10,  44,  30);
    set_dac(C_BLACK,   24,  26,  32);
    set_dac(C_WHITE,  238, 240, 245);
    set_dac(C_HINT,   250, 190,  60);
    set_dac(C_CURSOR, 255, 110,  40);
    set_dac(C_TEXT,   228, 233, 240);
    set_dac(C_DIM,    118, 128, 145);
    set_dac(C_PANEL,   22,  26,  34);
    set_dac(C_EMBER,  242, 145,  42);
    set_dac(C_LAST,    90, 200, 255);
    set_dac(C_SHADOW,   8,  30,  20);
}

static void get_rom_font(void)
{
    struct rmcall rc;
    memset(&rc, 0, sizeof rc);
    rc.ax = 0x1130;                      /* get font pointer */
    rc.bx = 0x0300;                      /* BH=3: the 8x8 double-dot font */
    rc.intno = 0x10;
    sys_bios(&rc);
    rom_font = (const unsigned char *)((unsigned)rc.es * 16 + rc.bp);
}

static void clear(int color) { memset(fb, color, sizeof fb); }

static void hline(int x, int y, int w, int c)
{
    if (y < 0 || y >= SCRH) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > SCRW) w = SCRW - x;
    if (w > 0) memset(fb + y * SCRW + x, c, w);
}

static void fill(int x, int y, int w, int h, int c)
{
    int i;
    for (i = 0; i < h; i++) hline(x, y + i, w, c);
}

static void frame(int x, int y, int w, int h, int c)
{
    int i;
    hline(x, y, w, c);
    hline(x, y + h - 1, w, c);
    for (i = 0; i < h; i++) {
        if (x >= 0 && x < SCRW && y + i >= 0 && y + i < SCRH) fb[(y + i) * SCRW + x] = c;
        if (x + w - 1 >= 0 && x + w - 1 < SCRW && y + i >= 0 && y + i < SCRH)
            fb[(y + i) * SCRW + x + w - 1] = c;
    }
}

static void disc(int cx, int cy, int rad, int c)
{
    int y, x;
    for (y = -rad; y <= rad; y++) {
        int py = cy + y;
        if (py < 0 || py >= SCRH) continue;
        for (x = -rad; x <= rad; x++) {
            int px = cx + x;
            if (px < 0 || px >= SCRW) continue;
            if (x * x + y * y <= rad * rad) fb[py * SCRW + px] = c;
        }
    }
}

static void ring(int cx, int cy, int rad, int c)
{
    int y, x, r2 = rad * rad, ri2 = (rad - 1) * (rad - 1);
    for (y = -rad; y <= rad; y++) {
        int py = cy + y;
        if (py < 0 || py >= SCRH) continue;
        for (x = -rad; x <= rad; x++) {
            int px = cx + x, d = x * x + y * y;
            if (px < 0 || px >= SCRW) continue;
            if (d <= r2 && d > ri2) fb[py * SCRW + px] = c;
        }
    }
}

static void glyph(int x, int y, unsigned char ch, int c)
{
    const unsigned char *g = rom_font + ch * 8;
    int row, col;
    for (row = 0; row < 8; row++) {
        unsigned char bits = g[row];
        int py = y + row;
        if (py < 0 || py >= SCRH) continue;
        for (col = 0; col < 8; col++)
            if (bits & (0x80 >> col)) {
                int px = x + col;
                if (px >= 0 && px < SCRW) fb[py * SCRW + px] = c;
            }
    }
}

static void text(int x, int y, const char *s, int c)
{
    for (; *s; s++, x += 8) glyph(x, y, (unsigned char)*s, c);
}

static void num(int x, int y, int v, int c)
{
    char buf[12];
    int i = 0, j;
    if (v == 0) buf[i++] = '0';
    while (v > 0) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    for (j = 0; j < i; j++) glyph(x + j * 8, y, (unsigned char)buf[i - 1 - j], c);
}

static void present(void) { memcpy(vga, fb, sizeof fb); }

/* ===== board drawing ===== */
#define CELL 20
#define BX   6
#define BY   18
#define PX   (BX + 8 * CELL + 6)         /* panel x */

static void draw_board(void)
{
    int r, c, bl, wh;
    struct move mv[32];
    int n = game_over ? 0 : legal_moves(board, turn, mv);

    clear(C_BG);

    /* title bar */
    text(BX, 6, "REVERSI", C_EMBER);
    text(BX + 72, 6, "for Ember", C_DIM);

    /* felt */
    fill(BX - 2, BY - 2, 8 * CELL + 4, 8 * CELL + 4, C_SHADOW);
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++)
            fill(BX + c * CELL, BY + r * CELL, CELL, CELL,
                 ((r + c) & 1) ? C_FELT : C_FELT2);
    for (r = 0; r <= 8; r++) {
        hline(BX, BY + r * CELL, 8 * CELL, C_GRID);
        fill(BX + r * CELL, BY, 1, 8 * CELL, C_GRID);
    }
    /* star points, as in the web board */
    for (r = 2; r <= 5; r += 3)
        for (c = 2; c <= 5; c += 3)
            disc(BX + c * CELL, BY + r * CELL, 1, C_GRID);

    /* discs */
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++) {
            int cx = BX + c * CELL + CELL / 2, cy = BY + r * CELL + CELL / 2;
            int v = board[r * 8 + c];
            if (v == EMPTY) continue;
            disc(cx + 1, cy + 1, 8, C_SHADOW);
            disc(cx, cy, 8, v == BLACK ? C_BLACK : C_WHITE);
            if (v == WHITE) ring(cx, cy, 8, C_DIM);
        }

    /* hints for the side to move */
    if (show_hints && !game_over) {
        int i;
        for (i = 0; i < n; i++)
            disc(BX + mv[i].c * CELL + CELL / 2, BY + mv[i].r * CELL + CELL / 2,
                 2, C_HINT);
    }

    /* last move */
    if (last_r >= 0)
        ring(BX + last_c * CELL + CELL / 2, BY + last_r * CELL + CELL / 2, 10, C_LAST);

    /* cursor */
    if (!game_over)
        frame(BX + cur_c * CELL, BY + cur_r * CELL, CELL + 1, CELL + 1, C_CURSOR);

    /* ---- panel ---- */
    fill(PX - 4, BY - 2, SCRW - PX + 2, 8 * CELL + 4, C_PANEL);

    count_discs(board, &bl, &wh);
    disc(PX + 4, BY + 6, 5, C_BLACK);
    ring(PX + 4, BY + 6, 5, C_DIM);
    num(PX + 14, BY + 2, bl, C_TEXT);
    text(PX + 14 + (bl > 9 ? 16 : 8), BY + 2, "blk", C_DIM);

    disc(PX + 4, BY + 24, 5, C_WHITE);
    num(PX + 14, BY + 20, wh, C_TEXT);
    text(PX + 14 + (wh > 9 ? 16 : 8), BY + 20, "wht", C_DIM);

    if (game_over) {
        text(PX, BY + 42, bl > wh ? "BLACK WINS" : wh > bl ? "WHITE WINS" : "A DRAW",
             C_EMBER);
        text(PX, BY + 52, "N: new game", C_DIM);
    } else {
        text(PX, BY + 42, turn == BLACK ? "Black to move" : "White to move", C_TEXT);
        if (mode_ai && turn != human_color) text(PX, BY + 52, "thinking...", C_HINT);
        else if (passed_msg) text(PX, BY + 52, "no move: passed", C_HINT);
        else { num(PX, BY + 52, n, C_DIM); text(PX + 16, BY + 52, "moves", C_DIM); }
    }

    /* settings */
    text(PX, BY + 70, mode_ai ? "vs computer" : "two players", C_DIM);
    if (mode_ai) {
        text(PX, BY + 80, "level", C_DIM);
        num(PX + 48, BY + 80, difficulty, C_TEXT);
    }
    text(PX, BY + 90, show_hints ? "hints on" : "hints off", C_DIM);

    /* move log: the last six, in the web game's notation */
    {
        int i, y = BY + 106, shown = 0;
        text(PX, y, "moves", C_EMBER);
        y += 10;
        for (i = log_n - 1; i >= 0 && shown < 5; i--, shown++) {
            char s[10];
            int j = 0;
            s[j++] = movelog[i].color == BLACK ? 'B' : 'W';
            s[j++] = ' ';
            if (movelog[i].pass) { s[j++] = 'p'; s[j++] = 'a'; s[j++] = 's'; s[j++] = 's'; }
            else {
                s[j++] = (char)('A' + movelog[i].c);
                s[j++] = (char)('1' + movelog[i].r);
            }
            s[j] = 0;
            text(PX, y + shown * 9, s, shown == 0 ? C_TEXT : C_DIM);
        }
    }

    /* keys */
    text(BX, BY + 8 * CELL + 5, "arrows or A-H,1-8   ENTER play  U undo", C_DIM);
    text(BX, BY + 8 * CELL + 14, "N new  D level  M mode  H hints  ESC", C_DIM);

    present();
}

/* ===== game flow ===== */
static void reset_game(void)
{
    memset(board, EMPTY, sizeof board);
    board[3 * 8 + 3] = WHITE; board[4 * 8 + 4] = WHITE;
    board[3 * 8 + 4] = BLACK; board[4 * 8 + 3] = BLACK;
    turn = BLACK;
    game_over = 0;
    hist_n = 0;
    log_n = 0;
    last_r = last_c = -1;
    passed_msg = 0;
    cur_r = 2; cur_c = 3;
}

static void push_history(void)
{
    if (hist_n >= MAX_HIST) {
        memmove(&hist[0], &hist[1], sizeof hist[0] * (MAX_HIST - 1));
        hist_n--;
    }
    memcpy(hist[hist_n].b, board, sizeof board);
    hist[hist_n].turn = turn;
    hist[hist_n].lr = last_r;
    hist[hist_n].lc = last_c;
    hist[hist_n].log_n = log_n;
    hist_n++;
}

static void log_move(int r, int c, int color, int flips, int pass)
{
    if (log_n >= MAX_LOG) {
        memmove(&movelog[0], &movelog[1], sizeof movelog[0] * (MAX_LOG - 1));
        log_n--;
    }
    movelog[log_n].r = (signed char)r;
    movelog[log_n].c = (signed char)c;
    movelog[log_n].color = (signed char)color;
    movelog[log_n].flips = flips;
    movelog[log_n].pass = (signed char)pass;
    log_n++;
}

/* after a move: pass when the next side has nothing, end when neither has */
static void advance_turn(void)
{
    int next = opp(turn);
    passed_msg = 0;
    if (legal_moves(board, next, 0)) { turn = next; return; }
    if (legal_moves(board, turn, 0)) {               /* the other side passes */
        log_move(0, 0, next, 0, 1);
        passed_msg = 1;
        return;                                      /* same side moves again */
    }
    game_over = 1;
}

static int play(int r, int c)
{
    int flips;
    if (game_over) return 0;
    push_history();
    flips = apply_move(board, r, c, turn);
    if (!flips) { hist_n--; return 0; }
    log_move(r, c, turn, flips, 0);
    last_r = r; last_c = c;
    advance_turn();
    return 1;
}

static void undo(void)
{
    /* step back over the computer's reply too, so undo returns the human's turn */
    int steps = (mode_ai ? 2 : 1);
    while (steps-- && hist_n > 0) {
        hist_n--;
        memcpy(board, hist[hist_n].b, sizeof board);
        turn = hist[hist_n].turn;
        last_r = hist[hist_n].lr;
        last_c = hist[hist_n].lc;
        log_n = hist[hist_n].log_n;
        game_over = 0;
        if (!mode_ai) break;
        if (turn == human_color) break;
    }
    passed_msg = 0;
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {                      /* REVERSI 2P | 1..5 */
        if (argv[i][0] == '2' && (argv[i][1] == 'p' || argv[i][1] == 'P')) mode_ai = 0;
        else if (argv[i][0] >= '1' && argv[i][0] <= '5') difficulty = argv[i][0] - '0';
        else if (argv[i][0] == 'w' || argv[i][0] == 'W') human_color = WHITE;
        else if (argv[i][0] == 'a' || argv[i][0] == 'A') human_color = EMPTY;
    }

    reversi_set_double_precision();
    get_rom_font();
    sys_set_video_mode(0x13);
    palette();
    reset_game();
    srand(0x1234u);

    for (;;) {
        int key, al, scan;

        draw_board();

        if (!game_over && mode_ai && turn != human_color) {
            int mr, mc;
            if (human_color == EMPTY && sys_kbhit() && (sys_getkey() & 0xFF) == 27)
                break;                               /* Esc leaves the demo */
            if (choose_ai_move(board, turn, difficulty, &mr, &mc)) play(mr, mc);
            else advance_turn();
            continue;
        }

        key = sys_getkey();
        al = key & 0xFF;
        scan = (key >> 8) & 0xFF;

        if (al == 27) break;                          /* Esc */
        if (al == 13 || al == 32) {                   /* Enter / Space */
            play(cur_r, cur_c);
            continue;
        }
        if (al == 0 || al == 0xE0) {                  /* extended: arrows */
            if (scan == 0x48 && cur_r > 0) cur_r--;
            else if (scan == 0x50 && cur_r < 7) cur_r++;
            else if (scan == 0x4B && cur_c > 0) cur_c--;
            else if (scan == 0x4D && cur_c < 7) cur_c++;
            continue;
        }
        if (al >= 'a' && al <= 'h') { cur_c = al - 'a'; continue; }
        if (al >= 'A' && al <= 'H') { cur_c = al - 'A'; continue; }
        if (al >= '1' && al <= '8') { cur_r = al - '1'; continue; }
        switch (al) {
        case 'n': case 'N': reset_game(); break;
        case 'u': case 'U': undo(); break;
        case 'h': case 'H': show_hints = !show_hints; break;
        case 'm': case 'M': mode_ai = !mode_ai; reset_game(); break;
        case 'd': case 'D': difficulty = difficulty >= 5 ? 1 : difficulty + 1; break;
        case 'q': case 'Q': goto done;
        default: break;
        }
    }
done:
    sys_set_video_mode(0x03);
    sys_puts("Thanks for playing Reversi.\r\n");
    return 0;
}

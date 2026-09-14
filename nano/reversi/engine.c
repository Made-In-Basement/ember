/* engine.c - Reversi rules and AI: a transcription of borkit/reversi game.js.
 * The same positional weights, the same evaluation and the same alpha-beta
 * search with corner-first move ordering.  REVERSI_NATIVE builds it against
 * the host libc for that check; otherwise it uses Ember's nanolibc.
 */
#ifdef REVERSI_NATIVE
#include <string.h>
#include <stdlib.h>
#else
#include <nanolibc.h>
#endif
#include "engine.h"

/* JS numbers are float64.  The x87 keeps 80-bit intermediates by default,
   which would round differently, so switch it to 53-bit precision; on a host
   build (SSE2 doubles) there is nothing to do. */
void reversi_set_double_precision(void)
{
#if defined(__i386__) && !defined(REVERSI_NATIVE)
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    cw = (unsigned short)((cw & ~0x0300) | 0x0200);   /* PC = 10b: double */
    __asm__ volatile("fldcw %0" : : "m"(cw));
#endif
}

static const signed char WEIGHTS[64] = {
     120, -20,  20,   5,   5,  20, -20, 120,
     -20, -40,  -5,  -5,  -5,  -5, -40, -20,
      20,  -5,  15,   3,   3,  15,  -5,  20,
       5,  -5,   3,   3,   3,   3,  -5,   5,
       5,  -5,   3,   3,   3,   3,  -5,   5,
      20,  -5,  15,   3,   3,  15,  -5,  20,
     -20, -40,  -5,  -5,  -5,  -5, -40, -20,
     120, -20,  20,   5,   5,  20, -20, 120
};

static const int DR[8] = { -1, -1, -1,  0, 0,  1, 1, 1 };
static const int DC[8] = { -1,  0,  1, -1, 1, -1, 0, 1 };

/* ===== engine (a transcription of game.js) ===== */
int opp(int c) { return c == BLACK ? WHITE : BLACK; }

int flips_for(const Board b, int r, int c, int color, signed char *out)
{
    int d, n = 0;
    if (b[r * 8 + c] != EMPTY) return 0;
    for (d = 0; d < 8; d++) {
        int rr = r + DR[d], cc = c + DC[d], len = 0;
        while (rr >= 0 && rr < 8 && cc >= 0 && cc < 8 && b[rr * 8 + cc] == opp(color)) {
            rr += DR[d]; cc += DC[d]; len++;
        }
        if (len && rr >= 0 && rr < 8 && cc >= 0 && cc < 8 && b[rr * 8 + cc] == color) {
            rr = r + DR[d]; cc = c + DC[d];
            while (len--) { if (out) out[n] = (signed char)(rr * 8 + cc); n++; rr += DR[d]; cc += DC[d]; }
        }
    }
    return n;
}

int legal_moves(const Board b, int color, struct move *mv)
{
    int r, c, n = 0;
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++)
            if (flips_for(b, r, c, color, 0)) {
                if (mv) { mv[n].r = (signed char)r; mv[n].c = (signed char)c;
                          mv[n].weight = WEIGHTS[r * 8 + c]; }
                n++;
            }
    return n;
}

int apply_move(Board b, int r, int c, int color)
{
    signed char f[24];
    int n = flips_for(b, r, c, color, f), i;
    if (!n) return 0;
    b[r * 8 + c] = (signed char)color;
    for (i = 0; i < n; i++) b[(int)f[i]] = (signed char)color;
    return n;
}

void count_discs(const Board b, int *black, int *white)
{
    int i; *black = *white = 0;
    for (i = 0; i < 64; i++) {
        if (b[i] == BLACK) (*black)++;
        else if (b[i] == WHITE) (*white)++;
    }
}

/* game.js evaluate(), arithmetic for arithmetic.  The scores are float64 in
   the browser, so they are double here too; reversi.c puts the x87 into
   53-bit precision first so each operation rounds exactly as JS does. */
double evaluate(const Board b, int color)
{
    int opponent = opp(color), i;
    int mine = 0, theirs = 0, total, my_moves, opp_moves;
    double pos = 0, mobility = 0, disc_score = 0, endgame_factor;

    for (i = 0; i < 64; i++) {
        if (b[i] == color) { pos += WEIGHTS[i]; mine++; }
        else if (b[i] == opponent) { pos -= WEIGHTS[i]; theirs++; }
    }
    my_moves = legal_moves(b, color, 0);
    opp_moves = legal_moves(b, opponent, 0);
    if (my_moves + opp_moves != 0)
        mobility = 100.0 * (my_moves - opp_moves) / (my_moves + opp_moves);
    total = mine + theirs;
    endgame_factor = total / 64.0;
    if (total != 0) disc_score = 100.0 * (mine - theirs) / total;
    return pos + mobility * (1 - endgame_factor) * 2
               + disc_score * endgame_factor * 4;
}

static void sort_by_weight(struct move *mv, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {                        /* n <= 30ish: insertion */
        struct move t = mv[i];
        for (j = i - 1; j >= 0 && mv[j].weight < t.weight; j--) mv[j + 1] = mv[j];
        mv[j + 1] = t;
    }
}

#define INF 1e30

double minimax(const Board b, int color, int depth, double alpha, double beta,
               int maximizing, int root_color, struct move *best_out)
{
    struct move mv[32];
    int n = legal_moves(b, color, mv), i;
    double value;

    if (depth == 0) return evaluate(b, root_color);
    if (!n) {
        if (!legal_moves(b, opp(color), 0)) {        /* terminal */
            int bl, wh, diff;
            count_discs(b, &bl, &wh);
            diff = (root_color == BLACK) ? bl - wh : wh - bl;
            return diff * 10000.0;
        }
        return minimax(b, opp(color), depth - 1, alpha, beta, !maximizing,
                       root_color, 0);
    }
    sort_by_weight(mv, n);
    value = maximizing ? -INF : INF;
    for (i = 0; i < n; i++) {
        Board nb;
        double score;
        memcpy(nb, b, sizeof nb);
        apply_move(nb, mv[i].r, mv[i].c, color);
        score = minimax(nb, opp(color), depth - 1, alpha, beta, !maximizing,
                        root_color, 0);
        if (maximizing) {
            if (score > value) { value = score; if (best_out) *best_out = mv[i]; }
            if (value > alpha) alpha = value;
        } else {
            if (score < value) { value = score; if (best_out) *best_out = mv[i]; }
            if (value < beta) beta = value;
        }
        if (alpha >= beta) break;
    }
    return value;
}

int choose_ai_move(const Board b, int color, int diff, int *mr, int *mc)
{
    struct move mv[32], best;
    int n = legal_moves(b, color, mv);
    if (!n) return 0;
    if (diff <= 1) {                                 /* easy: top-4, at random */
        int pool = n < 4 ? n : 4;
        sort_by_weight(mv, n);
        best = mv[rand() % pool];
    } else {
        best = mv[0];
        minimax(b, color, diff, -INF, INF, 1, color, &best);
    }
    *mr = best.r; *mc = best.c;
    return 1;
}


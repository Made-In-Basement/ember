/* engine.h - the Reversi rules and AI, ported from borkit/reversi game.js.
 * Free of any Ember dependency, so the same code also compiles natively
 * (-DREVERSI_NATIVE) to check it against game.js move for move.
 */
#ifndef REVERSI_ENGINE_H
#define REVERSI_ENGINE_H

#define SIZE   8
#define EMPTY  0
#define BLACK  1
#define WHITE  2

typedef signed char Board[64];

struct move { signed char r, c; int weight; };

int  opp(int c);
int  flips_for(const Board b, int r, int c, int color, signed char *out);
int  legal_moves(const Board b, int color, struct move *mv);
int  apply_move(Board b, int r, int c, int color);
void count_discs(const Board b, int *black, int *white);
double evaluate(const Board b, int color);
double minimax(const Board b, int color, int depth, double alpha, double beta,
               int maximizing, int root_color, struct move *best_out);
void reversi_set_double_precision(void);
int  choose_ai_move(const Board b, int color, int diff, int *mr, int *mc);

#endif

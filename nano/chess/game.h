/*
  Chess for Ember, built on Stockfish 11
  Copyright (C) 2026 ember-contrib authors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.  See nano/chess/COPYING.
*/

// game.h - a game of chess: the moves made so far, what may be played next,
// how the moves are written, and when the game is over.
//
// Legal moves come from Stockfish's move generator.  What Stockfish does not
// need and a game does - algebraic notation, the draw rules as a referee
// applies them, undo, PGN - is here, written to agree with the python-chess
// library, which the tests use as an independent referee.

#ifndef GAME_H_INCLUDED
#define GAME_H_INCLUDED

#include <string>
#include <vector>

#include "position.h"
#include "types.h"

namespace Game {

extern const char* StartFEN;

enum Ending {
  ONGOING,
  CHECKMATE,
  STALEMATE,
  INSUFFICIENT_MATERIAL,   // neither side can ever mate
  FIFTY_MOVES,             // 50 moves each without a capture or pawn move
  THREEFOLD_REPETITION,    // the same position for the third time
  RESIGNATION
};

struct PlayedMove {
  Move move;
  std::string san;         // Nf3, exd6, O-O, e8=Q+
  std::string uci;         // g1f3, e5d6, e1g1, e7e8q
  Piece piece;             // what moved
  Piece captured;          // what was taken, or NO_PIECE
};

class Chess {
public:
  Chess();

  // Start from a FEN.  Returns an empty string, or what is wrong with it.
  std::string reset(const std::string& fen = StartFEN);

  const Position& position() const { return pos; }
  Color side_to_move() const { return pos.side_to_move(); }
  bool in_check() const { return pos.checkers() != 0; }

  std::vector<Move> legal_moves() const;
  Move find_move(Square from, Square to, PieceType promotion = NO_PIECE_TYPE) const;
  bool is_promotion(Square from, Square to) const;   // a pawn reaching the last rank

  std::string san(Move m) const;           // for a move in the current position
  bool play(Move m);                       // false if not legal
  bool undo();                             // take back the last move
  void resign(Color loser);

  Ending ending() const;
  Color winner() const;                    // for CHECKMATE and RESIGNATION
  std::string result() const;              // "1-0", "0-1", "1/2-1/2" or "*"
  std::string ending_text() const;         // "Checkmate", "Draw by repetition", ...

  const std::vector<PlayedMove>& moves() const { return played; }
  const std::string& start_fen() const { return startFen; }
  std::string fen() const { return pos.fen(); }
  std::string uci_position() const;        // "position fen ... moves ..."

  // PGN with the given tags (White, Black, Event...), in the order given.
  std::string pgn(const std::vector<std::pair<std::string, std::string>>& tags) const;

  // Pieces each side has lost, and the material difference in pawns.
  std::vector<Piece> lost(Color c) const;
  int material_balance() const;            // + means White is ahead

  // The position as a referee compares positions for repetition: pieces,
  // side to move, castling rights, and an en passant square only when an
  // en passant capture is actually legal.
  std::string identity() const;

  static bool insufficient(const Position& p, Color c);

private:
  void rebuild();

  std::string startFen;
  std::vector<PlayedMove> played;
  std::vector<std::string> seen;           // identity() after each move, and at the start
  StateListPtr states;
  Position pos;
  Color resigned = COLOR_NB;
};

// CHESS RULES: play reproducible random games and write every position's
// legal moves, the notation of the move chosen and the game's ending, for
// the rules check in ember-contrib (chess/tests/rules_check.py) to referee
// with python-chess.
void rules_check(int games);

} // namespace Game

#endif

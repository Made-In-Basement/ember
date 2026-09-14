/*
  Chess for Ember, built on Stockfish 11
  Copyright (C) 2026 ember-contrib authors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.  See nano/chess/COPYING.
*/

#include <algorithm>

#include "estd.h"
#include "bitboard.h"
#include "movegen.h"
#include "thread.h"
#include "uci.h"

#include "game.h"
#include "platform.h"

namespace Game {

const char* StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

namespace {

const char PieceLetter[PIECE_TYPE_NB] = { ' ', 'P', 'N', 'B', 'R', 'Q', 'K', ' ' };

// Split a FEN into its fields.
std::vector<std::string> fields(const std::string& fen) {
  std::vector<std::string> f;
  estd::istringstream is(fen);
  std::string w;
  while (is >> w) f.push_back(w);
  return f;
}

// Stockfish's Position::set() trusts its input.  A FEN typed by a person is
// checked first, so a mistake is reported instead of confusing the engine.
std::string check_fen(const std::string& fen) {
  std::vector<std::string> f = fields(fen);
  if (f.size() < 4) return "a FEN needs at least 4 fields";
  int rank = 7, file = 0, kings[2] = { 0, 0 }, pawns[2] = { 0, 0 }, count[2] = { 0, 0 };
  for (char c : f[0]) {
      if (c == '/') {
          if (file != 8) return "rank " + std::to_string(rank + 1) + " does not have 8 squares";
          --rank; file = 0;
          if (rank < 0) return "too many ranks";
      } else if (c >= '1' && c <= '8') {
          file += c - '0';
      } else {
          const char* p = strchr("PNBRQKpnbrqk", c);
          if (!p || !c) return std::string("unknown piece '") + c + "'";
          int side = islower((unsigned char)c) ? 1 : 0;
          char lower = char(tolower((unsigned char)c));
          if (lower == 'k') kings[side]++;
          if (lower == 'p') {
              pawns[side]++;
              if (rank == 0 || rank == 7) return "a pawn on the first or last rank";
          }
          count[side]++;
          file++;
      }
      if (file > 8) return "rank " + std::to_string(rank + 1) + " has more than 8 squares";
  }
  if (rank != 0 || file != 8) return "the board does not have 8 full ranks";
  if (kings[0] != 1 || kings[1] != 1) return "each side needs exactly one king";
  if (pawns[0] > 8 || pawns[1] > 8) return "more than 8 pawns";
  if (count[0] > 16 || count[1] > 16) return "more than 16 pieces";
  if (f[1] != "w" && f[1] != "b") return "the side to move must be w or b";
  for (char c : f[2])
      if (!strchr("KQkq-", c)) return "castling rights must be KQkq or -";
  if (f[3] != "-" && (f[3].size() != 2 || f[3][0] < 'a' || f[3][0] > 'h'
                      || (f[3][1] != '3' && f[3][1] != '6')))
      return "the en passant square must be like e3 or -";
  for (size_t i = 4; i < f.size() && i < 6; ++i)
      for (char c : f[i])
          if (c < '0' || c > '9') return "move counters must be numbers";
  return "";
}

bool is_castling(Move m) { return type_of(m) == CASTLING; }

} // namespace


Chess::Chess() { reset(); }


std::string Chess::reset(const std::string& fen) {
  std::string err = check_fen(fen);
  if (!err.empty())
      return err;

  // Castling rights the pieces cannot back up are dropped, as Stockfish's
  // set() would otherwise assume a rook that is not there.
  std::vector<std::string> f = fields(fen);
  std::string rights;
  StateListPtr probe(new std::deque<StateInfo>(1));
  Position p;
  p.set(f[0] + " w - - 0 1", false, &probe->back(), Threads.main());
  auto has = [&](Square s, Piece pc) { return p.piece_on(s) == pc; };
  if (f[2].find('K') != std::string::npos && has(SQ_E1, W_KING) && has(SQ_H1, W_ROOK)) rights += 'K';
  if (f[2].find('Q') != std::string::npos && has(SQ_E1, W_KING) && has(SQ_A1, W_ROOK)) rights += 'Q';
  if (f[2].find('k') != std::string::npos && has(SQ_E8, B_KING) && has(SQ_H8, B_ROOK)) rights += 'k';
  if (f[2].find('q') != std::string::npos && has(SQ_E8, B_KING) && has(SQ_A8, B_ROOK)) rights += 'q';
  if (rights.empty()) rights = "-";

  std::string normal = f[0] + " " + f[1] + " " + rights + " " + f[3] + " "
                     + (f.size() > 4 ? f[4] : "0") + " " + (f.size() > 5 ? f[5] : "1");

  // The side not to move must not be in check: that position cannot arise.
  StateListPtr s2(new std::deque<StateInfo>(1));
  Position q;
  q.set(normal, false, &s2->back(), Threads.main());
  Color them = ~q.side_to_move();
  if (q.attackers_to(q.square<KING>(them)) & q.pieces(q.side_to_move()))
      return "the side that just moved is still in check";

  startFen = normal;
  played.clear();
  resigned = COLOR_NB;
  rebuild();
  return "";
}


void Chess::rebuild() {
  states = StateListPtr(new std::deque<StateInfo>(1));
  pos.set(startFen, false, &states->back(), Threads.main());
  seen.clear();
  seen.push_back(identity());
  for (const auto& pm : played) {
      states->emplace_back();
      pos.do_move(pm.move, states->back());
      seen.push_back(identity());
  }
}


std::vector<Move> Chess::legal_moves() const {
  std::vector<Move> v;
  if (resigned != COLOR_NB) return v;
  for (const auto& m : MoveList<LEGAL>(pos))
      v.push_back(m);
  return v;
}


// The square a player moves the king to when castling is where it lands (g1
// or c1); Stockfish writes castling as the king taking its own rook.
Move Chess::find_move(Square from, Square to, PieceType promotion) const {
  for (Move m : legal_moves()) {
      if (from_sq(m) != from) continue;
      Square dest = to_sq(m);
      if (is_castling(m)) {
          Square kingTo = make_square(dest > from ? FILE_G : FILE_C, rank_of(from));
          if (to != kingTo && to != dest) continue;
      } else if (dest != to)
          continue;
      if (type_of(m) == PROMOTION) {
          if (promotion_type(m) != (promotion == NO_PIECE_TYPE ? QUEEN : promotion)) continue;
      }
      return m;
  }
  return MOVE_NONE;
}


bool Chess::is_promotion(Square from, Square to) const {
  for (Move m : legal_moves())
      if (from_sq(m) == from && to_sq(m) == to && type_of(m) == PROMOTION)
          return true;
  return false;
}


std::string Chess::san(Move m) const {

  std::string s;
  Square from = from_sq(m), to = to_sq(m);
  Piece pc = pos.moved_piece(m);
  PieceType pt = type_of(pc);

  if (is_castling(m))
      s = to > from ? "O-O" : "O-O-O";
  else {
      bool capture = pos.capture(m);
      if (pt == PAWN) {
          if (capture) { s += char('a' + file_of(from)); s += 'x'; }
      } else {
          s += PieceLetter[pt];
          // Name the file, else the rank, else both, when another piece of
          // the same kind could also move to this square.
          bool ambiguous = false, sameFile = false, sameRank = false;
          for (const auto& other : MoveList<LEGAL>(pos)) {
              Square of = from_sq(other);
              if (other == m || to_sq(other) != to || of == from) continue;
              if (type_of(pos.piece_on(of)) != pt || is_castling(other)) continue;
              ambiguous = true;
              sameFile |= file_of(of) == file_of(from);
              sameRank |= rank_of(of) == rank_of(from);
          }
          if (ambiguous) {
              if (!sameFile) s += char('a' + file_of(from));
              else if (!sameRank) s += char('1' + rank_of(from));
              else { s += char('a' + file_of(from)); s += char('1' + rank_of(from)); }
          }
          if (capture) s += 'x';
      }
      s += UCI::square(to);
      if (type_of(m) == PROMOTION) {
          s += '=';
          s += PieceLetter[promotion_type(m)];
      }
  }

  // Check and mate: play the move on a copy and look.
  if (pos.gives_check(m)) {
      StateListPtr st(new std::deque<StateInfo>(1));
      Position p;
      p.set(pos.fen(), false, &st->back(), Threads.main());
      st->emplace_back();
      p.do_move(m, st->back());
      s += MoveList<LEGAL>(p).size() ? '+' : '#';
  }
  return s;
}


bool Chess::play(Move m) {
  if (ending() != ONGOING) return false;
  bool legal = false;
  for (const auto& lm : MoveList<LEGAL>(pos))
      if (lm == m) { legal = true; break; }
  if (!legal) return false;

  PlayedMove pm;
  pm.move = m;
  pm.san = san(m);
  pm.uci = UCI::move(m, false);
  pm.piece = pos.moved_piece(m);
  pm.captured = type_of(m) == ENPASSANT ? make_piece(~pos.side_to_move(), PAWN)
              : is_castling(m) ? NO_PIECE : pos.piece_on(to_sq(m));
  played.push_back(pm);

  states->emplace_back();
  pos.do_move(m, states->back());
  seen.push_back(identity());
  return true;
}


bool Chess::undo() {
  if (played.empty()) return false;
  played.pop_back();
  resigned = COLOR_NB;
  rebuild();
  return true;
}


void Chess::resign(Color loser) {
  if (ending() == ONGOING) resigned = loser;
}


std::string Chess::identity() const {
  std::vector<std::string> f = fields(pos.fen());
  std::string ep = "-";
  if (pos.ep_square() != SQ_NONE)
      for (const auto& m : MoveList<LEGAL>(pos))
          if (type_of(m) == ENPASSANT) { ep = UCI::square(pos.ep_square()); break; }
  return f[0] + " " + f[1] + " " + f[2] + " " + ep;
}


// python-chess's has_insufficient_material(): this side can never checkmate,
// whatever the other side does.
bool Chess::insufficient(const Position& p, Color c) {
  Bitboard us = p.pieces(c), them = p.pieces(~c);
  if (us & p.pieces(PAWN, ROOK) || us & p.pieces(QUEEN))
      return false;
  if (us & p.pieces(KNIGHT))
      // A lone knight, and nothing on the other side a king could be
      // smothered against.
      return popcount(us) <= 2 && !(them & ~p.pieces(KING) & ~p.pieces(QUEEN));
  if (us & p.pieces(BISHOP)) {
      // Bishops that all stand on one colour, with no knights or pawns
      // anywhere and no opposing bishop on the other colour.
      Bitboard bishops = p.pieces(BISHOP);
      bool sameColour = !(bishops & DarkSquares) || !(bishops & ~DarkSquares);
      return sameColour && !p.pieces(PAWN) && !p.pieces(KNIGHT);
  }
  return true;
}


Ending Chess::ending() const {
  if (resigned != COLOR_NB) return RESIGNATION;
  bool anyMove = MoveList<LEGAL>(pos).size() > 0;
  if (!anyMove && pos.checkers()) return CHECKMATE;
  if (insufficient(pos, WHITE) && insufficient(pos, BLACK)) return INSUFFICIENT_MATERIAL;
  if (!anyMove) return STALEMATE;
  if (pos.rule50_count() >= 100) return FIFTY_MOVES;
  const std::string& now = seen.back();
  if (std::count(seen.begin(), seen.end(), now) >= 3) return THREEFOLD_REPETITION;
  return ONGOING;
}


Color Chess::winner() const {
  Ending e = ending();
  if (e == RESIGNATION) return ~resigned;
  if (e == CHECKMATE) return ~pos.side_to_move();
  return COLOR_NB;
}


std::string Chess::result() const {
  switch (ending()) {
  case ONGOING: return "*";
  case CHECKMATE:
  case RESIGNATION: return winner() == WHITE ? "1-0" : "0-1";
  default: return "1/2-1/2";
  }
}


std::string Chess::ending_text() const {
  switch (ending()) {
  case CHECKMATE:             return "Checkmate";
  case STALEMATE:             return "Stalemate";
  case INSUFFICIENT_MATERIAL: return "Insufficient material";
  case FIFTY_MOVES:           return "Fifty-move rule";
  case THREEFOLD_REPETITION:  return "Threefold repetition";
  case RESIGNATION:           return (resigned == WHITE ? "White" : "Black") + std::string(" resigned");
  default:                    return "";
  }
}


std::string Chess::uci_position() const {
  std::string s = "position fen " + startFen;
  if (!played.empty()) {
      s += " moves";
      for (const auto& pm : played) s += " " + pm.uci;
  }
  return s;
}


std::string Chess::pgn(const std::vector<std::pair<std::string, std::string>>& tags) const {
  std::string out;
  for (const auto& t : tags)
      out += "[" + t.first + " \"" + t.second + "\"]\r\n";
  if (startFen != StartFEN)
      out += "[SetUp \"1\"]\r\n[FEN \"" + startFen + "\"]\r\n";
  std::string term;
  switch (ending()) {
  case ONGOING: term = "unterminated"; break;
  case RESIGNATION: term = "normal"; break;
  default: term = "normal"; break;
  }
  out += "[Result \"" + result() + "\"]\r\n";
  if (ending() != ONGOING)
      out += "[Termination \"" + ending_text() + "\"]\r\n";
  out += "\r\n";

  std::vector<std::string> f = fields(startFen);
  int moveNo = f.size() > 5 ? atoi(f[5].c_str()) : 1;
  bool black = f[1] == "b";
  std::string line, text;
  auto add = [&](const std::string& token) {
      if (line.size() + token.size() + 1 > 79) { text += line + "\r\n"; line.clear(); }
      if (!line.empty()) line += ' ';
      line += token;
  };
  for (size_t i = 0; i < played.size(); ++i) {
      if (!black)
          add(std::to_string(moveNo) + ". " + played[i].san);
      else if (i == 0)
          add(std::to_string(moveNo) + "... " + played[i].san);
      else
          add(played[i].san);
      if (black) ++moveNo;
      black = !black;
  }
  add(result());
  text += line + "\r\n";
  return out + text;
}


std::vector<Piece> Chess::lost(Color c) const {
  std::vector<Piece> v;
  for (const auto& pm : played)
      if (pm.captured != NO_PIECE && color_of(pm.captured) == c)
          v.push_back(pm.captured);
  // Promotions: a pawn that became something else is not "lost", but the
  // piece it became should count; material_balance() handles value.
  std::sort(v.begin(), v.end(), [](Piece a, Piece b) { return type_of(a) > type_of(b); });
  return v;
}


int Chess::material_balance() const {
  static const int value[PIECE_TYPE_NB] = { 0, 1, 3, 3, 5, 9, 0, 0 };
  int sum = 0;
  for (PieceType pt = PAWN; pt <= QUEEN; ++pt)
      sum += value[pt] * (popcount(pos.pieces(WHITE, pt)) - popcount(pos.pieces(BLACK, pt)));
  return sum;
}


// ---------------------------------------------------------------- RULES

void rules_check(int games) {

  // Starting positions chosen to reach the rules quickly: castling rights
  // and castling through check, en passant (including captures illegal by
  // pin), promotion, stalemate, insufficient material and repetition.
  static const char* Starts[] = {
    StartFEN,
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "8/8/1k6/2b5/2pP4/8/5K2/8 b - d3 0 1",
    "4k3/1P6/8/8/8/8/K7/8 w - - 0 1",
    "8/P1k5/K7/8/8/8/8/8 w - - 0 1",
    "K1k5/8/P7/8/8/8/8/8 w - - 0 1",
    "7k/8/5KQ1/8/8/8/8/8 w - - 0 1",
    "4k3/8/8/8/8/8/8/4KB2 w - - 0 1",
    "4k3/8/8/2n5/8/8/8/4K3 b - - 0 1",
    "2b1k3/8/8/8/8/8/8/2B1K3 w - - 0 1",
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
    "4k3/8/8/8/8/8/8/R3K2R w KQ - 98 80",
  };

  PRNG rng(20260914);
  std::string out;
  Platform::print("rules check: " + std::to_string(games) + " games");

  for (int g = 0; g < games; ++g) {
      Chess c;
      const char* start = Starts[g % (sizeof Starts / sizeof *Starts)];
      c.reset(start);
      out += "game " + std::to_string(g) + " " + c.start_fen() + "\n";

      for (int ply = 0; ply < 600; ++ply) {
          std::vector<Move> legal = c.legal_moves();
          std::vector<std::string> names;
          for (Move m : legal) names.push_back(UCI::move(m, false));
          std::sort(names.begin(), names.end());

          Ending e = c.ending();
          out += c.fen() + " |";
          for (auto& n : names) out += " " + n;
          out += " | " + std::to_string(int(e));

          if (e != ONGOING) { out += "\n"; break; }

          // Mostly random moves; now and then the same move back and forth,
          // so that repetitions actually happen.
          Move m = legal[rng.rand<unsigned>() % legal.size()];
          const auto& hist = c.moves();
          if (hist.size() >= 4 && rng.rand<unsigned>() % 4 == 0) {
              Move back = hist[hist.size() - 4].move;
              for (Move lm : legal)
                  if (lm == back) { m = lm; break; }
          }
          out += " | " + c.san(m) + "\n";
          c.play(m);
      }
      out += "result " + c.result() + " " + c.ending_text() + "\n";
      out += c.pgn({ { "Event", "rules check" }, { "Round", std::to_string(g) } });
      out += "end\n";
  }
  Platform::write_file("CHESSRUL.TXT", out);
  Platform::print("rules check: wrote CHESSRUL.TXT");
}

} // namespace Game

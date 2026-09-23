/*
  Chess for Ember, built on Stockfish 11
  Copyright (C) 2026 ember-contrib authors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.  See nano/chess/COPYING.
*/

// main.cpp - starting up, and the checks that run without the board.
//
//   CHESS                 play (see app.cpp for the arguments)
//   CHESS BENCH [depth]   Stockfish's bench; writes CHESSBEN.TXT
//   CHESS PERFT [max]     move generation counts; writes CHESSPER.TXT
//   CHESS RULES [games]   rules check games; writes CHESSRUL.TXT
//
// The checks exist so a build on Ember can be compared, line for line, with
// the same code and with the untouched Stockfish on a desktop.

#include <cctype>
#include <cstdio>

#include "bitboard.h"
#include "endgame.h"
#include "estd.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"

#include "app.h"
#include "game.h"
#include "platform.h"

namespace PSQT { void init(); }
namespace UCI { void on_hash_size(const Option&); }

#ifdef EMBER
#define EMBER_TARGET "Ember (32-bit x86, x87)"
#else
#define EMBER_TARGET "Linux (32-bit x86, x87)"
#endif

#ifdef EMBER
extern "C" void ember_run_constructors(void);
#endif

namespace {

std::string Captured;

void capture(const char* text, size_t len) {
  Captured.append(text, len);
}

// Everything the engine says, to a file and to whoever is watching.
struct Report {
  std::string all;
  const char* file;
  explicit Report(const char* f) : file(f) {}
  void flush_engine() {
      size_t start = 0;
      while (start < Captured.size()) {
          size_t end = Captured.find('\n', start);
          if (end == std::string::npos) end = Captured.size();
          line(Captured.substr(start, end - start));
          start = end + 1;
      }
      Captured.clear();
  }
  void line(const std::string& s) {
      all += s;
      all += "\r\n";
      Platform::print(s);
  }
  ~Report() { Platform::write_file(file, all); }
};

std::string upper(std::string s) {
  for (auto& c : s) c = char(toupper((unsigned char)c));
  return s;
}

std::string word(const std::string& s, size_t n) {
  estd::istringstream is(s);
  std::string w;
  for (size_t i = 0; i <= n; ++i)
      if (!(is >> w)) return "";
  return w;
}

void engine_init(int hashMB) {
  UCI::init(Options);
  // The hash size is set before the threads exist, as its default would
  // otherwise be allocated first and only then shrunk.
  Options["Hash"] << UCI::Option(double(hashMB), 1, 2048, UCI::on_hash_size);
  PSQT::init();
  Bitboards::init();
  Position::init();
  Bitbases::init();
  Endgames::init();
  Threads.set(Options["Threads"]);
  Search::clear(); // After threads are up
}

void run_bench(const std::string& depth) {
  Report r("CHESSBEN.TXT");
  estd::cout.sink = capture;
  estd::cerr.sink = capture;
  r.line("Stockfish 11 bench on " + std::string(EMBER_TARGET) + ", depth " + depth);
  UCI::execute("bench 16 1 " + depth + " default depth");
  r.flush_engine();
}

// Perft: counts of every legal move sequence to a given depth, the standard
// test of a move generator.  Expected numbers are Stockfish's own perft test
// (tests/perft.sh) and the Chess Programming Wiki's.
struct PerftCase { const char* fen; int depth; uint64_t nodes; };

const PerftCase PerftCases[] = {
  { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609 },
  { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603 },
  { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624 },
  { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333 },
  { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487 },
  { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594 },
};

const PerftCase PerftDeep[] = {
  { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 5, 193690690 },
  { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 6, 11030083 },
  { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 5, 15833292 },
  { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 5, 89941194 },
  { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 5, 164075551 },
};

uint64_t perft(Position& pos, int depth) {
  StateInfo st;
  uint64_t nodes = 0;
  const bool leaf = (depth == 2);
  for (const auto& m : MoveList<LEGAL>(pos)) {
      if (depth <= 1) { nodes++; continue; }
      pos.do_move(m, st);
      nodes += leaf ? MoveList<LEGAL>(pos).size() : perft(pos, depth - 1);
      pos.undo_move(m);
  }
  return nodes;
}

void run_perft(const std::string& which) {
  Report r("CHESSPER.TXT");
  int failures = 0;
  std::vector<PerftCase> cases(std::begin(PerftCases), std::end(PerftCases));
  if (which == "DEEP")
      cases.insert(cases.end(), std::begin(PerftDeep), std::end(PerftDeep));
  r.line("perft on " + std::string(EMBER_TARGET));
  for (const auto& c : cases) {
      StateListPtr states(new std::deque<StateInfo>(1));
      Position pos;
      pos.set(c.fen, false, &states->back(), Threads.main());
      TimePoint t0 = now();
      uint64_t n = perft(pos, c.depth);
      TimePoint ms = now() - t0;
      estd::ostringstream os;
      os << (n == c.nodes ? "ok   " : "FAIL ") << "depth " << c.depth << " nodes " << n
         << " expected " << c.nodes << " (" << ms << " ms)  " << c.fen;
      failures += n != c.nodes;
      r.line(os.str());
  }
  estd::ostringstream os;
  os << "perft: " << (cases.size() - failures) << " of " << cases.size() << " positions correct";
  r.line(os.str());
}

} // namespace

int main(int argc, char* argv[]) {

#ifdef EMBER
  ember_run_constructors();
#endif
  Platform::init();

  std::string cmd = Platform::command_line(argc, argv);
  std::string first = upper(word(cmd, 0));

  // CHESS INPUT: twenty seconds of every keyboard and mouse event, written
  // to EMBER.LOG - for finding out why a machine's keyboard or mouse does
  // not reach the game.
  if (first == "INPUT") {
      std::string opt = upper(word(cmd, 1));
      if (opt == "PASSIVE") Platform::input_options = Platform::INPUT_PASSIVE;
      if (opt == "NOMOUSE") Platform::input_options = Platform::INPUT_NO_MOUSE;
      if (opt == "NOTIMER") Platform::input_options = Platform::INPUT_NO_FAST_TIMER;
      if (opt == "KEYBOARD") Platform::input_options = Platform::INPUT_NO_MOUSE | Platform::INPUT_NO_FAST_TIMER;
      Platform::log("CHESS INPUT: options " + opt);
      Platform::Screen s;
      if (!Platform::open_screen(s)) { Platform::print("no VESA mode"); return 1; }
      for (int i = 0; i < s.width * s.height; ++i) s.pixels[i] = 0x203040;
      Platform::present(s, 0, 0, s.width, s.height);
      uint64_t end = Platform::now_us() + 20000000;
      int events = 0;
      while (Platform::now_us() < end) {
          Platform::Event e;
          while (Platform::next_event(e)) {
              char buf[96];
              snprintf(buf, sizeof buf, "CHESS INPUT: type %d key %03X ch %d at %d,%d",
                       int(e.type), e.key, e.ch, e.x, e.y);
              Platform::log(buf);
              ++events;
              for (int i = 0; i < 400; ++i) s.pixels[(e.y % s.height) * s.width + (e.x + i) % s.width] = 0xFFFFFF;
              Platform::present(s, 0, 0, s.width, s.height);
          }
          Platform::idle();
      }
      Platform::close_screen();
      Platform::print("CHESS INPUT: " + std::to_string(events) + " events, see EMBER.LOG");
      return 0;
  }

  if (first == "BENCH" || first == "PERFT" || first == "RULES") {
      Platform::print("Chess for Ember - Stockfish 11 by the Stockfish developers (GPLv3)");
      engine_init(16);
      if (first == "BENCH")
          run_bench(word(cmd, 1).empty() ? "13" : word(cmd, 1));
      else if (first == "PERFT")
          run_perft(upper(word(cmd, 1)));
      else
          Game::rules_check(word(cmd, 1).empty() ? 200 : atoi(word(cmd, 1).c_str()));
      Threads.set(0);
      return 0;
  }

  // Stockfish's hash table is 16 MB by default.  Its per-thread tables take
  // about 30 MB more, and the screen two copies of itself, so on a small
  // machine the hash table gives way.  (BENCH keeps 16 MB: it must, to
  // search exactly as the reference does.)
  int mb = Platform::memory_mb();
  engine_init(mb >= 96 ? 16 : mb >= 64 ? 8 : 4);
  int rc = App::run(cmd);
  Threads.set(0);
  return rc;
}

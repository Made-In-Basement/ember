/*
  Chess for Ember, built on Stockfish 11
  Copyright (C) 2026 ember-contrib authors
  Based on Stockfish's uci.cpp, Copyright (C) 2004-2020 The Stockfish developers

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.  See nano/chess/COPYING.
*/

// uci.cpp - replaces Stockfish's uci.cpp.
//
// Stockfish reads UCI commands from standard input.  Ember has no standard
// input, so the game hands commands to UCI::execute() as strings instead.
// "position", "go", "setoption", "ucinewgame" and "bench" do exactly what
// Stockfish's do - the functions below are its own, reading from estd
// streams - and answers ("info ...", "bestmove ...") go to estd::cout.

#include <cassert>
#include <string>

#include "estd.h"
#include "evaluate.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"

using std::string;

extern std::vector<string> setup_bench(const Position&, estd::istream&);

namespace {

  // FEN string of the initial position, normal chess
  const char* StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

  Position UciPos;
  StateListPtr UciStates;


  // position() is called when engine receives the "position" UCI command.

  void position(Position& pos, estd::istringstream& is, StateListPtr& states) {

    Move m;
    string token, fen;

    is >> token;

    if (token == "startpos")
    {
        fen = StartFEN;
        is >> token; // Consume "moves" token if any
    }
    else if (token == "fen")
        while (is >> token && token != "moves")
            fen += token + " ";
    else
        return;

    states = StateListPtr(new std::deque<StateInfo>(1)); // Drop old and create a new one
    pos.set(fen, Options["UCI_Chess960"], &states->back(), Threads.main());

    // Parse move list (if any)
    while (is >> token && (m = UCI::to_move(pos, token)) != MOVE_NONE)
    {
        states->emplace_back();
        pos.do_move(m, states->back());
    }
  }


  // setoption() is called when engine receives the "setoption" UCI command.

  void setoption(estd::istringstream& is) {

    string token, name, value;

    is >> token; // Consume "name" token

    // Read option name (can contain spaces)
    while (is >> token && token != "value")
        name += (name.empty() ? "" : " ") + token;

    // Read option value (can contain spaces)
    while (is >> token)
        value += (value.empty() ? "" : " ") + token;

    if (Options.count(name))
        Options[name] = value;
    else
        sync_cout << "No such option: " << name << sync_endl;
  }


  // go() is called when engine receives the "go" UCI command.

  void go(Position& pos, estd::istringstream& is, StateListPtr& states) {

    Search::LimitsType limits;
    string token;
    bool ponderMode = false;

    limits.startTime = now(); // As early as possible!

    while (is >> token)
        if (token == "searchmoves")
            while (is >> token)
                limits.searchmoves.push_back(UCI::to_move(pos, token));

        else if (token == "wtime")     is >> limits.time[WHITE];
        else if (token == "btime")     is >> limits.time[BLACK];
        else if (token == "winc")      is >> limits.inc[WHITE];
        else if (token == "binc")      is >> limits.inc[BLACK];
        else if (token == "movestogo") is >> limits.movestogo;
        else if (token == "depth")     is >> limits.depth;
        else if (token == "nodes")     is >> limits.nodes;
        else if (token == "movetime")  is >> limits.movetime;
        else if (token == "mate")      is >> limits.mate;
        else if (token == "perft")     is >> limits.perft;
        else if (token == "infinite")  limits.infinite = 1;
        else if (token == "ponder")    ponderMode = true;

    Threads.start_thinking(pos, states, limits, ponderMode);
  }


  // bench() is Stockfish's bench, reporting to estd::cerr.  After each
  // search it also reports the nodes of that search alone, so a run on Ember
  // can be compared with a desktop build position by position.

  void bench(Position& pos, estd::istream& args, StateListPtr& states) {

    string token;
    uint64_t num, nodes = 0, cnt = 1;

    std::vector<string> list = setup_bench(pos, args);
    num = count_if(list.begin(), list.end(), [](string s) { return s.find("go ") == 0 || s.find("eval") == 0; });

    TimePoint elapsed = now();

    for (const auto& cmd : list)
    {
        estd::istringstream is(cmd);
        is >> estd::skipws >> token;

        if (token == "go" || token == "eval")
        {
            estd::cerr << "\nPosition: " << cnt++ << '/' << num << estd::endl;
            if (token == "go")
            {
               go(pos, is, states);
               Threads.main()->wait_for_search_finished();
               nodes += Threads.nodes_searched();
               estd::cerr << "Position nodes: " << Threads.nodes_searched() << estd::endl;
            }
            else
               sync_cout << "\n" << Eval::trace(pos) << sync_endl;
        }
        else if (token == "setoption")  setoption(is);
        else if (token == "position")   position(pos, is, states);
        else if (token == "ucinewgame") { Search::clear(); elapsed = now(); } // Search::clear() may take some while
    }

    elapsed = now() - elapsed + 1; // Ensure positivity to avoid a 'divide by zero'

    dbg_print(); // Just before exiting

    estd::cerr << "\n==========================="
               << "\nTotal time (ms) : " << elapsed
               << "\nNodes searched  : " << nodes
               << "\nNodes/second    : " << 1000 * nodes / elapsed << estd::endl;
  }

} // namespace


/// UCI::execute() runs one command, as UCI::loop() would on reading it.

void UCI::execute(const string& cmd) {

  if (!UciStates)
  {
      UciStates = StateListPtr(new std::deque<StateInfo>(1));
      UciPos.set(StartFEN, false, &UciStates->back(), Threads.main());
  }

  estd::istringstream is(cmd);
  string token;
  is >> estd::skipws >> token;

  if (token == "stop")            Threads.stop = true;
  else if (token == "setoption")  setoption(is);
  else if (token == "go")         go(UciPos, is, UciStates);
  else if (token == "position")   position(UciPos, is, UciStates);
  else if (token == "ucinewgame") Search::clear();
  else if (token == "isready")    sync_cout << "readyok" << sync_endl;
  else if (token == "bench")      bench(UciPos, is, UciStates);
  else if (token == "eval")       sync_cout << Eval::trace(UciPos) << sync_endl;
  else
      sync_cout << "Unknown command: " << cmd << sync_endl;
}


/// UCI::loop() - there is no console to read on Ember.  Arguments, if any,
/// are run as one command, as Stockfish does with its command line.

void UCI::loop(int argc, char* argv[]) {

  string cmd;
  for (int i = 1; i < argc; ++i)
      cmd += string(argv[i]) + " ";
  if (!cmd.empty())
      execute(cmd);
}


/// UCI::value() converts a Value to a string suitable for use with the UCI
/// protocol specification.

string UCI::value(Value v) {

  assert(-VALUE_INFINITE < v && v < VALUE_INFINITE);

  estd::stringstream ss;

  if (abs(v) < VALUE_MATE - MAX_PLY)
      ss << "cp " << v * 100 / PawnValueEg;
  else
      ss << "mate " << (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;

  return ss.str();
}


/// UCI::square() converts a Square to a string in algebraic notation (g1, a7, etc.)

std::string UCI::square(Square s) {
  return std::string{ char('a' + file_of(s)), char('1' + rank_of(s)) };
}


/// UCI::move() converts a Move to a string in coordinate notation (g1f3, a7a8q).
/// The only special case is castling, where we print in the e1g1 notation in
/// normal chess mode, and in e1h1 notation in chess960 mode. Internally all
/// castling moves are always encoded as 'king captures rook'.

string UCI::move(Move m, bool chess960) {

  Square from = from_sq(m);
  Square to = to_sq(m);

  if (m == MOVE_NONE)
      return "(none)";

  if (m == MOVE_NULL)
      return "0000";

  if (type_of(m) == CASTLING && !chess960)
      to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));

  string move = UCI::square(from) + UCI::square(to);

  if (type_of(m) == PROMOTION)
      move += " pnbrqk"[promotion_type(m)];

  return move;
}


/// UCI::to_move() converts a string representing a move in coordinate notation
/// (g1f3, a7a8q) to the corresponding legal Move, if any.

Move UCI::to_move(const Position& pos, string& str) {

  if (str.length() == 5) // Junior could send promotion piece in uppercase
      str[4] = char(tolower(str[4]));

  for (const auto& m : MoveList<LEGAL>(pos))
      if (str == UCI::move(m, pos.is_chess960()))
          return m;

  return MOVE_NONE;
}

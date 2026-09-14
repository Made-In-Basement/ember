/*
  Chess for Ember, built on Stockfish 11
  Copyright (C) 2026 ember-contrib authors
  Based on Stockfish's thread.cpp, Copyright (C) 2004-2020 The Stockfish developers

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.  See nano/chess/COPYING.
*/

// thread.cpp - replaces Stockfish's thread.cpp for a machine with one core
// and no threads.
//
// Stockfish parks each search thread on a condition variable and wakes it
// with a signal.  Ember runs one program on one processor, so there is only
// ever the main thread, and "start searching" simply searches: the call
// returns when the search is over.  Everything that decides what the search
// sees - clearing histories, copying the root position and root moves into
// the thread - is Stockfish's own code, unchanged.

#include <algorithm> // For std::count
#include <cassert>

#include "movegen.h"
#include "search.h"
#include "thread.h"
#include "uci.h"
#include "syzygy/tbprobe.h"
#include "tt.h"

ThreadPool Threads; // Global object


/// Thread constructor.  Stockfish starts an OS thread here and waits for it
/// to park; this thread is parked from the start.

Thread::Thread(size_t n) : idx(n) {
  searching = false;
}


Thread::~Thread() {
  assert(!searching);
}

/// Thread::bestMoveCount(Move move) return best move counter for the given root move

int Thread::best_move_count(Move move) {

  auto rm = std::find(rootMoves.begin() + pvIdx,
                      rootMoves.begin() + pvLast, move);

  return rm != rootMoves.begin() + pvLast ? rm->bestMoveCount : 0;
}

/// Thread::clear() reset histories, usually before a new game

void Thread::clear() {

  counterMoves.fill(MOVE_NONE);
  mainHistory.fill(0);
  captureHistory.fill(0);

  for (bool inCheck : { false, true })
    for (StatsType c : { NoCaptures, Captures })
      for (auto& to : continuationHistory[inCheck][c])
        for (auto& h : to)
          h->fill(0);

  for (bool inCheck : { false, true })
    for (StatsType c : { NoCaptures, Captures })
      continuationHistory[inCheck][c][NO_PIECE][0]->fill(Search::CounterMovePruneThreshold - 1);
}

/// Thread::start_searching() runs the search to the end.

void Thread::start_searching() {

  searching = true;
  search();
  searching = false;
}


/// Thread::wait_for_search_finished() - a search is always finished by the
/// time anyone can ask.

void Thread::wait_for_search_finished() {}


void Thread::idle_loop() {}


/// ThreadPool::set() creates/destroys threads to match the requested number.

void ThreadPool::set(size_t requested) {

  if (size() > 0) { // destroy any existing thread(s)
      main()->wait_for_search_finished();

      while (size() > 0)
          delete back(), pop_back();
  }

  if (requested > 0) { // create new thread(s)
      push_back(new MainThread(0));

      while (size() < requested)
          push_back(new Thread(size()));
      clear();

      // Reallocate the hash with the new threadpool size
      TT.resize(Options["Hash"]);

      // Init thread number dependent search params.
      Search::init();
  }
}

/// ThreadPool::clear() sets threadPool data to initial values.

void ThreadPool::clear() {

  for (Thread* th : *this)
      th->clear();

  main()->callsCnt = 0;
  main()->previousScore = VALUE_INFINITE;
  main()->previousTimeReduction = 1.0;
}

/// ThreadPool::start_thinking() sets up the root and searches.  Stockfish
/// returns at once and searches in the background; here the call returns
/// with the search finished and its best move in rootMoves[0].

void ThreadPool::start_thinking(Position& pos, StateListPtr& states,
                                const Search::LimitsType& limits, bool ponderMode) {

  main()->wait_for_search_finished();

  main()->stopOnPonderhit = stop = false;
  increaseDepth = true;
  main()->ponder = ponderMode;
  Search::Limits = limits;
  Search::RootMoves rootMoves;

  for (const auto& m : MoveList<LEGAL>(pos))
      if (   limits.searchmoves.empty()
          || std::count(limits.searchmoves.begin(), limits.searchmoves.end(), m))
          rootMoves.emplace_back(m);

  if (!rootMoves.empty())
      Tablebases::rank_root_moves(pos, rootMoves);

  // After ownership transfer 'states' becomes empty, so if we stop the search
  // and call 'go' again without setting a new position states.get() == NULL.
  assert(states.get() || setupStates.get());

  if (states.get())
      setupStates = std::move(states); // Ownership transfer, states is now empty

  // We use Position::set() to set root position across threads. But there are
  // some StateInfo fields (previous, pliesFromNull, capturedPiece) that cannot
  // be deduced from a fen string, so set() clears them and to not lose the info
  // we need to backup and later restore setupStates->back(). Note that setupStates
  // is shared by threads but is accessed in read-only mode.
  StateInfo tmp = setupStates->back();

  for (Thread* th : *this)
  {
      th->nodes = th->tbHits = th->nmpMinPly = 0;
      th->rootDepth = th->completedDepth = 0;
      th->rootMoves = rootMoves;
      th->rootPos.set(pos.fen(), pos.is_chess960(), &setupStates->back(), th);
  }

  setupStates->back() = tmp;

  main()->start_searching();
}

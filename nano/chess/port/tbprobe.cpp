/*
  Chess for Ember, built on Stockfish 11
  Copyright (C) 2026 ember-contrib authors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.  See nano/chess/COPYING.
*/

// tbprobe.cpp - replaces Stockfish's Syzygy tablebase prober.
//
// The prober maps gigabytes of endgame files into memory.  Ember has no
// memory mapping and a 32 MB disk, so there are no tablebases: this is what
// Stockfish itself does when SyzygyPath is empty.  MaxCardinality stays 0,
// rank_root_moves() (in search.cpp) never probes, and the search is the same
// as a desktop Stockfish run without tablebase files.

#include "../position.h"
#include "tbprobe.h"

namespace Tablebases {

int MaxCardinality = 0;

void init(const std::string&) { MaxCardinality = 0; }

WDLScore probe_wdl(Position&, ProbeState* result) {
  *result = FAIL;
  return WDLDraw;
}

int probe_dtz(Position&, ProbeState* result) {
  *result = FAIL;
  return 0;
}

bool root_probe(Position&, Search::RootMoves&) { return false; }

bool root_probe_wdl(Position&, Search::RootMoves&) { return false; }

} // namespace Tablebases

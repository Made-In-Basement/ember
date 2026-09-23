/*
  Chess for Ember, built on Stockfish 11
  Copyright (C) 2026 ember-contrib authors
  Based on Stockfish's misc.cpp, Copyright (C) 2004-2020 The Stockfish developers

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.  See nano/chess/COPYING.
*/

// misc.cpp - replaces Stockfish's misc.cpp.  The original identifies the
// build, keeps debug counters, opens a log file and binds threads to Windows
// processor groups.  On Ember there is one processor and no console, so the
// counters print nowhere and the thread binding does nothing.

#include "estd.h"
#include "misc.h"
#include "thread.h"

namespace estd {
  ostream cout;
  ostream cerr;
}

const std::string engine_info(bool to_uci) {
  return to_uci ? "Stockfish 11 for Ember\nid author T. Romstad, M. Costalba, J. Kiiski, G. Linscott"
                : "Stockfish 11 for Ember by the Stockfish developers (see AUTHORS file)";
}

const std::string compiler_info() {
  return "\nCompiled by clang (zig cc) for Ember, 32-bit x86, x87 floating point, no SSE\n";
}

// A hint to the processor's cache; the result of the search is the same
// without it, and x87-era builds of Stockfish omit it too (NO_PREFETCH).
void prefetch(void*) {}

void start_logger(const std::string&) {}

namespace {
  int64_t hits[2], means[2];
}

void dbg_hit_on(bool b) { ++hits[0]; if (b) ++hits[1]; }
void dbg_hit_on(bool c, bool b) { if (c) dbg_hit_on(b); }
void dbg_mean_of(int v) { ++means[0]; means[1] += v; }

void dbg_print() {
  if (hits[0])
      estd::cerr << "Total " << hits[0] << " Hits " << hits[1]
                 << " hit rate (%) " << 100 * hits[1] / hits[0] << estd::endl;
  if (means[0])
      estd::cerr << "Total " << means[0] << " Mean "
                 << (double)means[1] / means[0] << estd::endl;
}

namespace WinProcGroup {
  void bindThisThread(size_t) {}
}

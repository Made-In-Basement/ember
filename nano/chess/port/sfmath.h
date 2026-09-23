/*
  Chess for Ember, built on Stockfish 11
  Copyright (C) 2026 ember-contrib authors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.  See nano/chess/COPYING.
*/

// sfmath.h - the three floating-point functions the search uses.
//
// Stockfish sizes its late-move reductions with log() and spends time with
// exp() and pow().  A reduction is truncated to an integer, so log() must
// round exactly as the reference build's does or the search changes.  The
// build rewrites std::log/std::exp/std::pow in the engine to these.

#ifndef SFMATH_H_INCLUDED
#define SFMATH_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

double sf_log(double x);
double sf_exp(double x);
double sf_pow(double x, double y);

#ifdef __cplusplus
}
#endif

#endif

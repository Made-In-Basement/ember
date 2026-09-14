/*
  Chess for Ember, built on Stockfish 11
  Copyright (C) 2026 ember-contrib authors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.  See nano/chess/COPYING.
*/

/* sflog.c - log() the way 32-bit x86 C libraries compute it.
 *
 * ln(x) = ln(2) * log2(x), done by the x87 in one instruction (FYL2X) and
 * then stored as a double, which rounds it.  This is musl's i386 log
 * instruction for instruction, so an Ember build and a 32-bit Linux build of
 * Stockfish agree on every reduction.  exp() and pow() come from musl's C
 * sources, compiled by the build under the names sf_exp and sf_pow.
 */

double sf_log(double x)
{
    double r;
    __asm__ volatile("fldln2\n"
                     "fldl (%0)\n"
                     "fyl2x\n"
                     "fstpl (%1)"
                     : : "r"(&x), "r"(&r) : "memory");
    return r;
}

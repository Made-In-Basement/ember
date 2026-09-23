/*
  Chess for Ember, built on Stockfish 11
  Copyright (C) 2026 ember-contrib authors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.  See nano/chess/COPYING.
*/

// estd.h - the small part of <sstream>/<iostream>/<iomanip> Stockfish uses.
//
// Ember has no C++ runtime.  libc++'s headers supply the containers, which
// are templates and need nothing compiled, but its streams sit on locales
// and a library Ember does not have.  The build rewrites Stockfish's stream
// names (std::stringstream, std::setw, std::cout, ...) to these, which cover
// exactly the operations the engine performs: reading a FEN a character or
// a number at a time, and formatting integers, strings and a few doubles.

#ifndef ESTD_H_INCLUDED
#define ESTD_H_INCLUDED

#include <cstddef>
#include <cstdint>
#include <string>

namespace estd {

class ios {
public:
  enum { F_SKIPWS = 1, F_FIXED = 2, F_SHOWPOINT = 4, F_SHOWPOS = 8, F_HEX = 16,
         F_UPPER = 32, F_LEFT = 64 };
  int flags = F_SKIPWS;
  int width = 0;
  int precision = 6;
  char fill = ' ';
};

typedef ios& (*manip)(ios&);

inline ios& skipws(ios& s)     { s.flags |= ios::F_SKIPWS; return s; }
inline ios& noskipws(ios& s)   { s.flags &= ~ios::F_SKIPWS; return s; }
inline ios& fixed(ios& s)      { s.flags |= ios::F_FIXED; return s; }
inline ios& showpoint(ios& s)  { s.flags |= ios::F_SHOWPOINT; return s; }
inline ios& noshowpoint(ios& s){ s.flags &= ~ios::F_SHOWPOINT; return s; }
inline ios& showpos(ios& s)    { s.flags |= ios::F_SHOWPOS; return s; }
inline ios& noshowpos(ios& s)  { s.flags &= ~ios::F_SHOWPOS; return s; }
inline ios& hex(ios& s)        { s.flags |= ios::F_HEX; return s; }
inline ios& dec(ios& s)        { s.flags &= ~ios::F_HEX; return s; }
inline ios& uppercase(ios& s)  { s.flags |= ios::F_UPPER; return s; }
inline ios& left(ios& s)       { s.flags |= ios::F_LEFT; return s; }
inline ios& right(ios& s)      { s.flags &= ~ios::F_LEFT; return s; }

struct setw_t { int n; };
struct setfill_t { char c; };
struct setprecision_t { int n; };
inline setw_t setw(int n) { return { n }; }
inline setfill_t setfill(char c) { return { c }; }
inline setprecision_t setprecision(int n) { return { n }; }

// ---------------------------------------------------------------- output

class ostream : public virtual ios {
public:
  typedef void (*Sink)(const char* text, size_t len);

  std::string buf;
  Sink sink = nullptr;       // a console: text goes out at each endl
  bool discard = false;      // a console nobody listens to

  ostream& write(const char* s, size_t n) {
      if (!discard)
          buf.append(s, n);
      return *this;
  }

  void flush_line() {
      if (sink && !buf.empty())
          sink(buf.data(), buf.size());
      if (sink || discard)
          buf.clear();
  }

  // Padding to the width set by setw(), which applies to one item only.
  ostream& padded(const char* s, size_t n) {
      int pad = width > int(n) ? width - int(n) : 0;
      width = 0;
      if (!(flags & F_LEFT))
          for (int i = 0; i < pad; ++i) write(&fill, 1);
      write(s, n);
      if (flags & F_LEFT)
          for (int i = 0; i < pad; ++i) write(&fill, 1);
      return *this;
  }

  ostream& unsigned_number(unsigned long long v, bool negative) {
      char tmp[32];
      int i = sizeof tmp;
      const char* digits = (flags & F_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";
      unsigned base = (flags & F_HEX) ? 16 : 10;
      do { tmp[--i] = digits[v % base]; v /= base; } while (v);
      if (negative) tmp[--i] = '-';
      else if ((flags & F_SHOWPOS) && base == 10) tmp[--i] = '+';
      // A fill of '0' goes between the sign and the digits, as the standard's
      // "internal" would put it; Stockfish only zero-pads unsigned keys.
      return padded(tmp + i, sizeof tmp - i);
  }

  ostream& operator<<(const char* s) { return padded(s, std::char_traits<char>::length(s)); }
  ostream& operator<<(const std::string& s) { return padded(s.data(), s.size()); }
  ostream& operator<<(char c) { return padded(&c, 1); }
  ostream& operator<<(bool b) { return unsigned_number(b, false); }
  ostream& operator<<(int v) { return unsigned_number(v < 0 ? 0ULL - (unsigned long long)(long long)v : v, v < 0); }
  ostream& operator<<(long v) { return unsigned_number(v < 0 ? 0ULL - (unsigned long long)(long long)v : v, v < 0); }
  ostream& operator<<(long long v) { return unsigned_number(v < 0 ? 0ULL - (unsigned long long)v : v, v < 0); }
  ostream& operator<<(unsigned v) { return unsigned_number(v, false); }
  ostream& operator<<(unsigned long v) { return unsigned_number(v, false); }
  ostream& operator<<(unsigned long long v) { return unsigned_number(v, false); }
  ostream& operator<<(float v) { return *this << double(v); }

  ostream& operator<<(double v) {
      // Enough for the evaluation trace's "%.2f"-style columns; nothing in
      // the engine's decisions depends on how a double is printed.
      char tmp[64];
      int i = 0;
      bool neg = v < 0;
      if (neg) v = -v;
      if (neg) tmp[i++] = '-';
      else if (flags & F_SHOWPOS) tmp[i++] = '+';
      int prec = precision < 0 ? 6 : precision > 12 ? 12 : precision;
      double scale = 1;
      for (int k = 0; k < prec; ++k) scale *= 10;
      unsigned long long whole = (unsigned long long)(v * scale + 0.5);
      unsigned long long ip = whole / (unsigned long long)scale;
      unsigned long long fp = whole % (unsigned long long)scale;
      char digits[24];
      int n = 0;
      do { digits[n++] = char('0' + ip % 10); ip /= 10; } while (ip && n < 20);
      while (n) tmp[i++] = digits[--n];
      if (prec > 0) {
          tmp[i++] = '.';
          for (int k = prec - 1; k >= 0; --k) {
              unsigned long long p = 1;
              for (int j = 0; j < k; ++j) p *= 10;
              tmp[i++] = char('0' + (fp / p) % 10);
          }
      }
      return padded(tmp, i);
  }

  ostream& operator<<(manip m) { m(*this); return *this; }
  ostream& operator<<(ostream& (*f)(ostream&)) { return f(*this); }
  ostream& operator<<(setw_t m) { width = m.n; return *this; }
  ostream& operator<<(setfill_t m) { fill = m.c; return *this; }
  ostream& operator<<(setprecision_t m) { precision = m.n; return *this; }
};

inline ostream& endl(ostream& os) { os.write("\n", 1); os.flush_line(); return os; }
inline ostream& flush(ostream& os) { os.flush_line(); return os; }

// ---------------------------------------------------------------- input

class istream : public virtual ios {
public:
  std::string in;
  size_t pos = 0;
  bool failed = false;

  bool good() const { return !failed && pos < in.size(); }
  bool fail() const { return failed; }
  bool eof() const { return pos >= in.size(); }
  explicit operator bool() const { return !failed; }
  bool operator!() const { return failed; }

  static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

  void skip_space() { while (pos < in.size() && space(in[pos])) ++pos; }

  istream& operator>>(char& c) {
      if (failed) return *this;
      if (flags & F_SKIPWS) skip_space();
      if (pos >= in.size()) { failed = true; return *this; }
      c = in[pos++];
      return *this;
  }

  istream& operator>>(unsigned char& c) { char t = 0; *this >> t; if (!failed) c = (unsigned char)t; return *this; }

  istream& operator>>(std::string& s) {
      if (failed) return *this;
      skip_space();                      // strings always skip leading space
      if (pos >= in.size()) { failed = true; return *this; }
      size_t start = pos;
      while (pos < in.size() && !space(in[pos])) ++pos;
      s.assign(in, start, pos - start);
      return *this;
  }

  bool read_integer(long long& out) {
      if (failed) return false;
      if (flags & F_SKIPWS) skip_space();
      size_t p = pos;
      bool neg = false;
      if (p < in.size() && (in[p] == '-' || in[p] == '+')) neg = in[p++] == '-';
      if (p >= in.size() || in[p] < '0' || in[p] > '9') { failed = true; out = 0; return false; }
      long long v = 0;
      while (p < in.size() && in[p] >= '0' && in[p] <= '9') v = v * 10 + (in[p++] - '0');
      pos = p;
      out = neg ? -v : v;
      return true;
  }

  istream& operator>>(int& v) { long long t; if (read_integer(t)) v = int(t); else v = 0; return *this; }
  istream& operator>>(long& v) { long long t; if (read_integer(t)) v = long(t); else v = 0; return *this; }
  istream& operator>>(long long& v) { long long t; if (read_integer(t)) v = t; else v = 0; return *this; }
  istream& operator>>(unsigned& v) { long long t; if (read_integer(t)) v = unsigned(t); else v = 0; return *this; }
  istream& operator>>(unsigned long& v) { long long t; if (read_integer(t)) v = (unsigned long)t; else v = 0; return *this; }
  istream& operator>>(manip m) { m(*this); return *this; }
};

inline istream& getline(istream& is, std::string& s, char delim = '\n') {
  if (is.failed || is.pos >= is.in.size()) { is.failed = true; return is; }
  size_t end = is.in.find(delim, is.pos);
  if (end == std::string::npos) end = is.in.size();
  s.assign(is.in, is.pos, end - is.pos);
  is.pos = end < is.in.size() ? end + 1 : end;
  return is;
}

class stringstream : public ostream, public istream {
public:
  stringstream() {}
  explicit stringstream(const std::string& s) { in = s; buf = s; }
  std::string str() const { return buf; }
};

class ostringstream : public ostream {
public:
  std::string str() const { return buf; }
};

class istringstream : public istream {
public:
  explicit istringstream(const std::string& s) { in = s; }
};

// The engine's console.  Search progress ("info depth ...") and "bestmove"
// arrive here; the game sets a sink to show or record them.
extern ostream cout;
extern ostream cerr;

} // namespace estd

#endif // #ifndef ESTD_H_INCLUDED

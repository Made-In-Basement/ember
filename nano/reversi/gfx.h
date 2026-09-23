/*
  Reversi for Ember.  The drawing, platform and C++ runtime files here are
  the ones Chess uses (nano/chess/), copied so each game builds on its own.
*/

// gfx.h - drawing into a 32-bit pixel buffer: rectangles with rounded,
// antialiased corners, circles, text from the generated fonts, and pieces.

#ifndef GFX_H_INCLUDED
#define GFX_H_INCLUDED

#include <cstdint>
#include <string>

struct Font;

namespace Gfx {

struct Rgb {
  uint8_t r, g, b;
  constexpr Rgb(uint8_t r_ = 0, uint8_t g_ = 0, uint8_t b_ = 0) : r(r_), g(g_), b(b_) {}
  uint32_t pixel() const { return uint32_t(r) << 16 | uint32_t(g) << 8 | b; }
};

Rgb mix(Rgb a, Rgb b, int alpha);          // alpha 0..255 towards b

struct Canvas {
  int w = 0, h = 0;
  uint32_t* px = nullptr;
  int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;

  void clip(int x0, int y0, int x1, int y1);
  void no_clip() { clip(0, 0, w, h); }

  void blend(int x, int y, Rgb c, int alpha);  // one pixel, alpha 0..255
  void fill(int x, int y, int rw, int rh, Rgb c);
  void fill_alpha(int x, int y, int rw, int rh, Rgb c, int alpha);
  void gradient(int x, int y, int rw, int rh, Rgb top, Rgb bottom);
  void round_rect(int x, int y, int rw, int rh, int radius, Rgb c, int alpha = 255);
  void round_frame(int x, int y, int rw, int rh, int radius, int thickness, Rgb c, int alpha = 255);
  void shadow(int x, int y, int rw, int rh, int radius, int spread, int alpha);
  void circle(double cx, double cy, double r, Rgb c, int alpha = 255);
  void ring(double cx, double cy, double r, double thickness, Rgb c, int alpha = 255);
  void mask(const uint8_t* m, int mw, int mh, int x, int y, Rgb c, int alpha = 255);

  // Text.  x is the left edge, y the baseline.  Returns the advance.
  int text(const Font& f, int x, int y, const std::string& s, Rgb c, int alpha = 255);
  static int text_width(const Font& f, const std::string& s);
  int text_centered(const Font& f, int cx, int y, const std::string& s, Rgb c, int alpha = 255);
  int text_right(const Font& f, int right, int y, const std::string& s, Rgb c, int alpha = 255);
};

// A piece at a square size; white or black.  size must be one of the
// generated sizes (piece_size() picks the best for a square).
int piece_size(int square);
void piece(Canvas& cv, int type, bool white, int size, int x, int y, int alpha = 255, bool shadow = true);

} // namespace Gfx

#endif

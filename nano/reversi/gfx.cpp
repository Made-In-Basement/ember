/*
  Reversi for Ember.  The drawing, platform and C++ runtime files here are
  the ones Chess uses (nano/chess/), copied so each game builds on its own.
*/

// gfx.cpp - software drawing.  Everything is integer or fixed-point where it
// runs per pixel; the x87 is slow, and the processor on Ember may be too.

#include <algorithm>

#include "assets.h"
#include "gfx.h"

namespace Gfx {

Rgb mix(Rgb a, Rgb b, int alpha) {
  return Rgb(uint8_t(a.r + ((b.r - a.r) * alpha) / 255),
               uint8_t(a.g + ((b.g - a.g) * alpha) / 255),
               uint8_t(a.b + ((b.b - a.b) * alpha) / 255));
}

void Canvas::clip(int x0, int y0, int x1, int y1) {
  clip_x0 = std::max(0, x0);
  clip_y0 = std::max(0, y0);
  clip_x1 = std::min(w, x1);
  clip_y1 = std::min(h, y1);
}

static inline uint32_t blend_pixel(uint32_t d, Rgb c, int a) {
  uint32_t r = (d >> 16) & 255, g = (d >> 8) & 255, b = d & 255;
  r += ((int(c.r) - int(r)) * a + 128) >> 8;
  g += ((int(c.g) - int(g)) * a + 128) >> 8;
  b += ((int(c.b) - int(b)) * a + 128) >> 8;
  return r << 16 | g << 8 | b;
}

void Canvas::blend(int x, int y, Rgb c, int alpha) {
  if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1 || alpha <= 0) return;
  uint32_t& d = px[y * w + x];
  if (alpha >= 255) d = c.pixel();
  else d = blend_pixel(d, c, alpha + (alpha >> 7));
}

void Canvas::fill(int x, int y, int rw, int rh, Rgb c) {
  int x0 = std::max(x, clip_x0), y0 = std::max(y, clip_y0);
  int x1 = std::min(x + rw, clip_x1), y1 = std::min(y + rh, clip_y1);
  uint32_t p = c.pixel();
  for (int yy = y0; yy < y1; ++yy) {
      uint32_t* row = px + yy * w;
      for (int xx = x0; xx < x1; ++xx) row[xx] = p;
  }
}

void Canvas::fill_alpha(int x, int y, int rw, int rh, Rgb c, int alpha) {
  if (alpha >= 255) { fill(x, y, rw, rh, c); return; }
  int a = alpha + (alpha >> 7);
  int x0 = std::max(x, clip_x0), y0 = std::max(y, clip_y0);
  int x1 = std::min(x + rw, clip_x1), y1 = std::min(y + rh, clip_y1);
  for (int yy = y0; yy < y1; ++yy) {
      uint32_t* row = px + yy * w;
      for (int xx = x0; xx < x1; ++xx) row[xx] = blend_pixel(row[xx], c, a);
  }
}

void Canvas::gradient(int x, int y, int rw, int rh, Rgb top, Rgb bottom) {
  for (int i = 0; i < rh; ++i)
      fill(x, y + i, rw, 1, mix(top, bottom, rh > 1 ? i * 255 / (rh - 1) : 0));
}

// Coverage of a pixel by a quarter disc of radius r centred at (cx, cy), in
// 1/256ths, from the distance of the pixel's centre: a cheap, good enough
// antialiasing for corners and dots.
static int coverage(int px_, int py_, double cx, double cy, double r) {
  double dx = px_ + 0.5 - cx, dy = py_ + 0.5 - cy;
  double d2 = dx * dx + dy * dy;
  if (d2 <= (r - 0.7) * (r - 0.7)) return 256;
  if (d2 >= (r + 0.7) * (r + 0.7)) return 0;
  // linear across a band 1.4 px wide around the edge
  double d = 0;
  // sqrt by Newton, from a good start: d2 is close to r*r here
  d = r;
  for (int i = 0; i < 3; ++i) d = 0.5 * (d + d2 / d);
  double v = (r + 0.7 - d) / 1.4;
  return v <= 0 ? 0 : v >= 1 ? 256 : int(v * 256);
}

void Canvas::round_rect(int x, int y, int rw, int rh, int radius, Rgb c, int alpha) {
  if (rw <= 0 || rh <= 0) return;
  radius = std::min(radius, std::min(rw, rh) / 2);
  if (radius <= 0) { fill_alpha(x, y, rw, rh, c, alpha); return; }
  // body
  fill_alpha(x, y + radius, rw, rh - 2 * radius, c, alpha);
  fill_alpha(x + radius, y, rw - 2 * radius, radius, c, alpha);
  fill_alpha(x + radius, y + rh - radius, rw - 2 * radius, radius, c, alpha);
  // corners
  for (int j = 0; j < radius; ++j)
      for (int i = 0; i < radius; ++i) {
          int cov = coverage(i, j, radius, radius, radius);
          if (!cov) continue;
          int a = alpha * cov >> 8;
          blend(x + i, y + j, c, a);
          blend(x + rw - 1 - i, y + j, c, a);
          blend(x + i, y + rh - 1 - j, c, a);
          blend(x + rw - 1 - i, y + rh - 1 - j, c, a);
      }
}

void Canvas::round_frame(int x, int y, int rw, int rh, int radius, int t, Rgb c, int alpha) {
  radius = std::min(radius, std::min(rw, rh) / 2);
  fill_alpha(x + radius, y, rw - 2 * radius, t, c, alpha);
  fill_alpha(x + radius, y + rh - t, rw - 2 * radius, t, c, alpha);
  fill_alpha(x, y + radius, t, rh - 2 * radius, c, alpha);
  fill_alpha(x + rw - t, y + radius, t, rh - 2 * radius, c, alpha);
  for (int j = 0; j < radius; ++j)
      for (int i = 0; i < radius; ++i) {
          int outer = coverage(i, j, radius, radius, radius);
          int inner = radius > t ? coverage(i, j, radius, radius, radius - t) : 0;
          int cov = outer - inner;
          if (cov <= 0) continue;
          int a = alpha * cov >> 8;
          blend(x + i, y + j, c, a);
          blend(x + rw - 1 - i, y + j, c, a);
          blend(x + i, y + rh - 1 - j, c, a);
          blend(x + rw - 1 - i, y + rh - 1 - j, c, a);
      }
}

// A soft shadow: rounded rectangles growing outward, each fainter.
void Canvas::shadow(int x, int y, int rw, int rh, int radius, int spread, int alpha) {
  for (int i = spread; i >= 1; --i) {
      int a = alpha * (spread - i + 1) / (spread * spread / 2 + 1);
      round_frame(x - i, y - i + spread / 3, rw + 2 * i, rh + 2 * i, radius + i, 1, Rgb(0, 0, 0), a);
  }
}

void Canvas::circle(double cx, double cy, double r, Rgb c, int alpha) {
  int x0 = int(cx - r - 1), x1 = int(cx + r + 2), y0 = int(cy - r - 1), y1 = int(cy + r + 2);
  for (int yy = y0; yy < y1; ++yy)
      for (int xx = x0; xx < x1; ++xx) {
          int cov = coverage(xx, yy, cx, cy, r);
          if (cov) blend(xx, yy, c, alpha * cov >> 8);
      }
}

void Canvas::ring(double cx, double cy, double r, double t, Rgb c, int alpha) {
  int x0 = int(cx - r - 1), x1 = int(cx + r + 2), y0 = int(cy - r - 1), y1 = int(cy + r + 2);
  for (int yy = y0; yy < y1; ++yy)
      for (int xx = x0; xx < x1; ++xx) {
          int cov = coverage(xx, yy, cx, cy, r) - coverage(xx, yy, cx, cy, r - t);
          if (cov > 0) blend(xx, yy, c, alpha * cov >> 8);
      }
}

void Canvas::mask(const uint8_t* m, int mw, int mh, int x, int y, Rgb c, int alpha) {
  int x0 = std::max(x, clip_x0), y0 = std::max(y, clip_y0);
  int x1 = std::min(x + mw, clip_x1), y1 = std::min(y + mh, clip_y1);
  for (int yy = y0; yy < y1; ++yy) {
      const uint8_t* src = m + (yy - y) * mw;
      uint32_t* row = px + yy * w;
      for (int xx = x0; xx < x1; ++xx) {
          int a = src[xx - x];
          if (!a) continue;
          if (alpha < 255) a = a * alpha / 255;
          if (a >= 255) row[xx] = c.pixel();
          else row[xx] = blend_pixel(row[xx], c, a + (a >> 7));
      }
  }
}

// Text: bytes 32..126 are ASCII; 128.. are the extra characters in the
// order assets.py lists them (middle dot, one half, ellipsis, minus, left
// and right triangles, em dash, multiplication sign).
static const Glyph* glyph_for(const Font& f, unsigned char ch) {
  if (ch >= 32 && ch < 127) return &f.glyph[ch - 32];
  if (ch >= 128 && ch < 136) return &f.glyph[95 + (ch - 128)];
  return &f.glyph[0];
}

int Canvas::text(const Font& f, int x, int y, const std::string& s, Rgb c, int alpha) {
  int pen = x;
  for (unsigned char ch : s) {
      const Glyph* g = glyph_for(f, ch);
      if (g->w)
          mask(f.pixels + g->offset, g->w, g->h, pen + g->x, y + g->y, c, alpha);
      pen += g->advance;
  }
  return pen - x;
}

int Canvas::text_width(const Font& f, const std::string& s) {
  int n = 0;
  for (unsigned char ch : s) n += glyph_for(f, ch)->advance;
  return n;
}

int Canvas::text_centered(const Font& f, int cx, int y, const std::string& s, Rgb c, int alpha) {
  int tw = text_width(f, s);
  return text(f, cx - tw / 2, y, s, c, alpha);
}

int Canvas::text_right(const Font& f, int right, int y, const std::string& s, Rgb c, int alpha) {
  int tw = text_width(f, s);
  return text(f, right - tw, y, s, c, alpha);
}

int piece_size(int square) {
  int best = PieceSets[0].size;
  for (int i = 0; i < PieceSetCount; ++i)
      if (PieceSets[i].size <= square) best = PieceSets[i].size;
  return best;
}

void piece(Canvas& cv, int type, bool white, int size, int x, int y, int alpha, bool shadow) {
  const PieceSet* set = &PieceSets[0];
  for (int i = 0; i < PieceSetCount; ++i)
      if (PieceSets[i].size == size) set = &PieceSets[i];
  const PieceMasks& m = set->piece[type];
  int s = set->size;
  if (shadow)
      cv.mask(m.shadow, s, s, x, y, Rgb(0, 0, 0), alpha * 90 / 255);
  if (white) {
      cv.mask(m.hull, s, s, x, y, Rgb(250, 247, 238), alpha);
      cv.mask(m.line, s, s, x, y, Rgb(30, 30, 32), alpha);
  } else {
      cv.mask(m.hull, s, s, x, y, Rgb(116, 116, 120), alpha);
      cv.mask(m.solid, s, s, x, y, Rgb(28, 28, 32), alpha);
  }
}

} // namespace Gfx

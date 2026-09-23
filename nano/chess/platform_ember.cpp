/*
  Chess for Ember, built on Stockfish 11
  Copyright (C) 2026 ember-contrib authors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.  See nano/chess/COPYING.
*/

// platform_ember.cpp - the game on Ember.
//
// Time comes from the processor's cycle counter, measured once against the
// interval timer (as NDOOM and VOXEL do).  The picture goes to a linear
// VESA framebuffer.  The keyboard and mouse are the desktop's own drivers
// (nano/gui/input.c), which already cope with the ways laptops pretend to
// have a PS/2 mouse.

#include <cstring>

#include "platform.h"

extern "C" {
#include "nano.h"
#include "input.h"
#include "touch.h"

void sys_log(const char* s);
void* malloc(size_t);
void free(void*);
}

[[noreturn]] void ember_fatal(const char* what);

namespace {

uint64_t tsc_hz, tsc_start;

uint64_t rdtsc() {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return (uint64_t(hi) << 32) | lo;
}

uint16_t pit_now() {
  outb(0x43, 0x00);                            // latch channel 0
  uint16_t v = inb(0x40);
  v |= uint16_t(inb(0x40)) << 8;
  return v;
}

void clock_start() {
  uint16_t a = pit_now(), b, gone;
  uint64_t t0 = rdtsc(), t1;
  do {                                         // the counter runs downward
      b = pit_now();
      gone = uint16_t(a - b);
  } while (gone < 40000);                      // about 33 ms of the 1.19 MHz clock
  t1 = rdtsc();
  tsc_hz = (t1 - t0) * 1193182u / gone;
  if (tsc_hz < 100000000u || tsc_hz > 20000000000u)
      tsc_hz = 2000000000u;                    // the timer would not answer
  tsc_start = t1;
}

// ---- screen ----
uint8_t* fb;
int fb_width, fb_height, fb_pitch, fb_bpp;
bool screen_open, input_open_ok;
unsigned rescued_bytes;                        // see rescue_lost_interrupt()

int pick_mode(vbe_mode& best) {
  static vbe_info info;
  vbe_mode m;
  int found = -1;
  long best_score = -1;
  if (sys_vbe_info(&info) != 0)
      return -1;
  for (int i = 0; i < info.mode_count; i++) {
      int mode = info.modes[i];
      if (sys_vbe_mode(mode, &m) != 0 || !m.framebuffer) continue;
      if (m.bpp != 32 && m.bpp != 24 && m.bpp != 16) continue;
      if (m.width < 640 || m.height < 480) continue;
      // The text is drawn at fixed pixel sizes chosen for 1024x768, so that
      // mode comes first; then the largest up to 1280x1024; then, on a panel
      // that offers nothing smaller, the smallest bigger one.
      long score;
      if (m.width == 1024 && m.height == 768) score = 3000000;
      else if (m.width <= 1280 && m.height <= 1024) score = long(m.width) * m.height;
      else score = 1000000000L / (long(m.width) * m.height / 1000 + 1);
      score = score * 4 + (m.bpp == 32 ? 3 : m.bpp == 24 ? 2 : 1);
      if (score > best_score) { best_score = score; found = mode; best = m; }
  }
  return found;
}

} // namespace

extern "C" unsigned now_ms(void) {
  if (!tsc_hz) return 0;
  return unsigned((rdtsc() - tsc_start) / (tsc_hz / 1000));
}

int64_t ember_now_ms() {
  return now_ms();
}

void ember_search_poll() {
  if (Platform::search_poll)
      Platform::search_poll();
}

namespace Platform {

void (*search_poll)() = nullptr;
int input_options = 0;

void init() {
  clock_start();
}

void print(const std::string& line) {
  sys_puts(line.c_str());
  sys_puts("\r\n");
  sys_log(line.c_str());
}

void log(const std::string& line) {
  sys_log(line.c_str());
}

bool write_file(const char* name, const std::string& data) {
  int h = sys_create(name);
  if (h < 0) return false;
  size_t done = 0;
  bool ok = true;
  while (done < data.size()) {
      int chunk = int(data.size() - done > 32000 ? 32000 : data.size() - done);
      if (sys_write(h, data.data() + done, chunk) != chunk) { ok = false; break; }
      done += chunk;
  }
  sys_close(h);
  return ok;
}

bool read_file(const char* name, std::string& data) {
  int h = sys_open(name);
  if (h < 0) return false;
  char buf[4096];
  int n;
  data.clear();
  while ((n = sys_read(h, buf, sizeof buf)) > 0) data.append(buf, n);
  sys_close(h);
  return true;
}

std::string command_line(int, char**) {
  const char* c = (const char*)nx_info->cmdline;
  std::string s;
  for (int i = 0; i < 127 && c[i] && c[i] != '\r' && c[i] != '\n'; ++i) s += c[i];
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

uint64_t now_us() {
  if (!tsc_hz) return 0;
  return (rdtsc() - tsc_start) / (tsc_hz / 1000000);
}

bool open_screen(Screen& s) {
  vbe_mode m;
  int mode = pick_mode(m);
  if (mode < 0 || sys_set_vbe_mode(mode, 1) != 0)
      return false;
  fb = (uint8_t*)m.framebuffer;
  fb_width = m.width;
  fb_height = m.height;
  fb_pitch = m.pitch;
  fb_bpp = m.bpp;
  s.width = m.width;
  s.height = m.height;
  s.pixels = (uint32_t*)malloc(size_t(m.width) * m.height * 4);
  if (!s.pixels) { sys_set_video_mode(3); return false; }
  screen_open = true;

  touch_open();
  if (input_options & INPUT_NO_MOUSE)
      input_open_ok = false;
  else {
      input_open_ok = input_open(m.width - 1, m.height - 1, (input_options & INPUT_PASSIVE) ? 1 : 0) == 0;
      if (!input_open_ok)
          sys_log("CHESS: no mouse; the keyboard still works");
  }
  input_start_keyboard();
  if (!(input_options & INPUT_NO_FAST_TIMER))
      input_fast_timer(1);
  mouse_max_x = m.width - 1;
  mouse_max_y = m.height - 1;
  mouse_x = m.width / 2;
  mouse_y = m.height / 2;
  return true;
}

void present(const Screen& s, int x0, int y0, int x1, int y1) {
  if (!screen_open) return;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > s.width) x1 = s.width;
  if (y1 > s.height) y1 = s.height;
  for (int y = y0; y < y1; ++y) {
      const uint32_t* src = s.pixels + y * s.width + x0;
      uint8_t* dst = fb + y * fb_pitch;
      int n = x1 - x0;
      if (fb_bpp == 32)
          memcpy(dst + x0 * 4, src, size_t(n) * 4);
      else if (fb_bpp == 24) {
          uint8_t* d = dst + x0 * 3;
          for (int i = 0; i < n; ++i, d += 3) {
              uint32_t p = src[i];
              d[0] = uint8_t(p); d[1] = uint8_t(p >> 8); d[2] = uint8_t(p >> 16);
          }
      } else {
          uint16_t* d = (uint16_t*)(dst + x0 * 2);
          for (int i = 0; i < n; ++i) {
              uint32_t p = src[i];
              d[i] = uint16_t(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
          }
      }
  }
}

void close_screen() {
  if (!screen_open) return;
  sys_logf("CHESS: %u keyboard/mouse bytes read after a lost interrupt", rescued_bytes);
  input_fast_timer(0);
  touch_close();
  input_close();
  sys_set_irq_handlers(0, 0);
  sys_set_video_mode(3);
  screen_open = false;
}

// A byte the keyboard controller is still holding means its interrupt was
// lost, and until someone reads it the controller sends nothing more: no
// keys, no mouse.  Hyper-V loses such interrupts when a byte arrives while
// Ember is switching the interrupt controller between real and protected
// mode (every system call does).  So look, and hand the byte to the same
// handler its interrupt would have called.
extern "C" {
  extern void (*nx_irq1_fn)(void);
  extern void (*nx_irq12_fn)(void);
}

void rescue_lost_interrupt() {
  cli();
  uint8_t status = inb(0x64);
  if (status & 0x01) {
      void (*handler)(void) = (status & 0x20) ? nx_irq12_fn : nx_irq1_fn;
      if (handler) { handler(); ++rescued_bytes; }
  }
  sti();
}

bool next_event(Event& e) {
  struct event ev;
  if (screen_open) {
      touch_poll();
      rescue_lost_interrupt();
  }
  while (::next_event(&ev)) {
      e.x = mouse_x;
      e.y = mouse_y;
      e.key = e.ch = 0;
      switch (ev.type) {
      case ::EV_KEY:        e.type = EV_KEY; e.key = ev.a; e.ch = ev.b; return true;
      case ::EV_MOUSE_MOVE: e.type = EV_MOUSE_MOVE; e.x = ev.a; e.y = ev.b; return true;
      case ::EV_MOUSE_DOWN: e.type = EV_MOUSE_DOWN; e.x = ev.a; e.y = ev.b; return true;
      case ::EV_MOUSE_UP:   e.type = EV_MOUSE_UP; e.x = ev.a; e.y = ev.b; return true;
      case ::EV_RIGHT_DOWN: e.type = EV_RIGHT_DOWN; e.x = ev.a; e.y = ev.b; return true;
      default: break;
      }
  }
  return false;
}

bool mouse_present() { return input_open_ok && ::mouse_present; }

int memory_mb() {
  return int((nx_info->mem_end - nx_info->mem_start) / (1024 * 1024));
}

void idle() {
  __asm__ volatile("hlt");
}

std::string date() {
  rmcall r;
  memset(&r, 0, sizeof r);
  r.ax = 0x0400;                               // read the real-time clock's date
  r.intno = 0x1A;
  sys_bios(&r);
  if (r.flags & 1) return "????.??.??";
  auto bcd = [](int v) { return (v >> 4) * 10 + (v & 15); };
  int century = bcd(r.cx >> 8), year = bcd(r.cx & 0xFF), month = bcd(r.dx >> 8), day = bcd(r.dx & 0xFF);
  char buf[16];
  buf[0] = char('0' + century / 10); buf[1] = char('0' + century % 10);
  buf[2] = char('0' + year / 10);    buf[3] = char('0' + year % 10);
  buf[4] = '.';
  buf[5] = char('0' + month / 10);   buf[6] = char('0' + month % 10);
  buf[7] = '.';
  buf[8] = char('0' + day / 10);     buf[9] = char('0' + day % 10);
  buf[10] = 0;
  return buf;
}

} // namespace Platform

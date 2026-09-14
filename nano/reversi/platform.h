/*
  Reversi for Ember.  The drawing, platform and C++ runtime files here are
  the ones Chess uses (nano/chess/), copied so each game builds on its own.
*/

// platform.h - what the game needs from the machine.
//
// platform_ember.cpp is the real thing: Ember's clock, screen, keyboard,
// mouse and files.  In ember-contrib, platform_host.cpp stands in for it on
// Linux, so a game's logic can be tested at full speed on the host.

#ifndef PLATFORM_H_INCLUDED
#define PLATFORM_H_INCLUDED

#include <cstdint>
#include <string>

namespace Platform {

void init();

// A line of text for whoever is watching: the text screen and EMBER.LOG on
// Ember, standard output on the host.
void print(const std::string& line);
void log(const std::string& line);           // EMBER.LOG only

bool write_file(const char* name, const std::string& data);
bool read_file(const char* name, std::string& data);

// The raw command line: Ember hands it over as one string, so a FEN with
// spaces in it arrives intact.
std::string command_line(int argc, char** argv);

// Called by the search every 1024 nodes (MainThread::check_time).
extern void (*search_poll)();

// Microseconds since startup, for animation.
uint64_t now_us();

// ---- graphics, keyboard and mouse (Ember only; the host has none) ----

struct Screen {
  int width, height;
  uint32_t* pixels;          // 0x00RRGGBB, width * height
};

// How open_screen() sets up the mouse and timer (for CHESS INPUT's tests).
enum { INPUT_PASSIVE = 1, INPUT_NO_MOUSE = 2, INPUT_NO_FAST_TIMER = 4 };
extern int input_options;

bool open_screen(Screen& s);
void present(const Screen& s, int x0, int y0, int x1, int y1);
void close_screen();

enum EventType { EV_NONE, EV_KEY, EV_MOUSE_MOVE, EV_MOUSE_DOWN, EV_MOUSE_UP, EV_RIGHT_DOWN };

struct Event {
  EventType type;
  int key;                   // EV_KEY: scancode (0x100 set for extended keys)
  int ch;                    // EV_KEY: the character typed, if any
  int x, y;                  // mouse position
};

bool next_event(Event& e);
bool mouse_present();
void idle();                 // wait for the next interrupt

// Megabytes of memory free for the program's heap when it started.
int memory_mb();

// Today's date for the PGN, as "YYYY.MM.DD", or "????.??.??".
std::string date();

} // namespace Platform

#endif

/*
  Chess for Ember, built on Stockfish 11
  Copyright (C) 2026 ember-contrib authors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.  See nano/chess/COPYING.
*/

// app.cpp - the board on the screen, and the player at the keyboard.
//
//   CHESS                    the new game screen
//   CHESS WHITE | BLACK      play that side against Stockfish (RANDOM too)
//   CHESS 1 .. 8             Stockfish's level (LEVEL n also works)
//   CHESS 2P                 two players at one keyboard
//   CHESS WATCH              Stockfish plays itself
//   CHESS FEN <fen>          start from a position (the rest of the line)
//   CHESS ... EXIT           leave when the game ends (for scripted tests)
//
// Any argument skips the new game screen.  The game is saved to CHESS.PGN
// when it ends, and when the player leaves a game in progress.

#include <algorithm>
#include <cmath>

#include "assets.h"
#include "estd.h"
#include "movegen.h"
#include "thread.h"
#include "uci.h"

#include "app.h"
#include "game.h"
#include "gfx.h"
#include "platform.h"

using namespace Gfx;

namespace {

// ---------------------------------------------------------------- look

const Rgb BgTop(33, 37, 48), BgBottom(17, 19, 26);
const Rgb Panel(38, 43, 56), PanelHi(52, 58, 75), PanelLine(62, 69, 88);
const Rgb Text(236, 238, 243), Dim(152, 159, 176), Faint(104, 111, 128);
const Rgb Accent(236, 166, 76), AccentDark(40, 30, 18);
const Rgb Good(126, 204, 120), Bad(232, 96, 84);
const Rgb LightSq(240, 217, 181), DarkSq(181, 136, 99), Frame(58, 43, 33);
const Rgb LastMove(214, 196, 70), Selected(84, 162, 232), HintSq(92, 196, 110);

// Levels: Stockfish's Skill Level (0 weakest .. 20 full strength) and how
// long it may think.  Its skill setting picks, now and then, a weaker move
// among the best few, more often the lower the level.
struct Level { int skill; int ms; const char* name; };
const Level Levels[9] = {
  { 0, 0, "" },
  { 0, 250, "Beginner" }, { 3, 400, "Novice" }, { 6, 600, "Casual" }, { 9, 800, "Club" },
  { 12, 1000, "Strong" }, { 15, 1500, "Expert" }, { 18, 2500, "Master" }, { 20, 4000, "Maximum" },
};

const char MidDot[] = "\x80";
const char LeftTri[] = "\x84";
const char RightTri[] = "\x85";

enum Mode { VS_ENGINE, TWO_PLAYERS, WATCH };
enum Dialog { NONE, NEW_GAME, PROMOTION, GAME_OVER, HELP, QUIT };

// Scancodes, as nano/gui/input.c reports them (extended keys | 0x100).
enum Key {
  K_ESC = 0x01, K_BACKSPACE = 0x0E, K_TAB = 0x0F, K_ENTER = 0x1C, K_SPACE = 0x39,
  K_F1 = 0x3B, K_F2 = 0x3C, K_F3 = 0x3D, K_F4 = 0x3E, K_F5 = 0x3F,
  K_UP = 0x148, K_DOWN = 0x150, K_LEFT = 0x14B, K_RIGHT = 0x14D,
};

struct Button {
  int x, y, w, h;
  int id;
  bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

enum ButtonId {
  B_NONE, B_NEW, B_UNDO, B_FLIP, B_HINT, B_LEVEL_DOWN, B_LEVEL_UP, B_HELP,
  B_DLG_START, B_DLG_CANCEL, B_DLG_CLOSE, B_DLG_QUIT, B_DLG_STAY,
  B_MODE0, B_MODE1, B_MODE2, B_SIDE0, B_SIDE1, B_SIDE2, B_DLG_LVL_DOWN, B_DLG_LVL_UP,
  B_PROMO_Q, B_PROMO_R, B_PROMO_B, B_PROMO_N, B_DLG_NEW_GAME,
};

// ---------------------------------------------------------------- engine

struct SearchReport {
  std::string best;
  int depth = 0;
  int score = 0;          // centipawns, White's point of view
  bool mate = false;
  int mateIn = 0;         // moves, + White mates
  uint64_t nodes = 0;
  bool valid = false;
};

SearchReport Report;
Color ReportSide = WHITE;

// Stockfish's "info" and "bestmove" lines, as they are printed.
void engine_line(const char* text, size_t len) {
  std::string line(text, len);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  estd::istringstream is(line);
  std::string tok;
  is >> tok;
  if (tok == "bestmove") {
      is >> Report.best;
      return;
  }
  if (tok != "info") return;
  int depth = 0, multipv = 1;
  int value = 0;
  bool mate = false, haveScore = false;
  uint64_t nodes = 0;
  while (is >> tok) {
      if (tok == "depth") is >> depth;
      else if (tok == "multipv") is >> multipv;
      else if (tok == "nodes") { long long n; is >> n; nodes = uint64_t(n); }
      else if (tok == "score") {
          std::string kind;
          is >> kind >> value;
          mate = kind == "mate";
          haveScore = true;
      }
      else if (tok == "pv") break;
  }
  if (nodes) Report.nodes = nodes;
  if (!haveScore || multipv != 1) return;
  Report.depth = depth;
  int sign = ReportSide == WHITE ? 1 : -1;
  Report.mate = mate;
  Report.mateIn = mate ? sign * value : 0;
  Report.score = mate ? 0 : sign * value;
  Report.valid = true;
}

// ---------------------------------------------------------------- the app

class ChessApp {
public:
  explicit ChessApp(const std::string& args);
  int run();

private:
  // setup
  void parse_args(const std::string& args);
  void start_game();
  void layout();

  // flow
  bool engine_to_move() const;
  void engine_move(bool hint);
  void human_move(Move m);
  void after_move();
  void animate(Move m, Piece piece);
  void undo();
  void save_pgn();

  // input
  void handle(const Platform::Event& e);
  void key(const Platform::Event& e);
  void mouse_down(int x, int y);
  void mouse_up(int x, int y);
  void dialog_key(const Platform::Event& e);
  void dialog_click(int x, int y);
  void press(int id);
  void try_move(Square from, Square to);
  void typed(char c);
  static void search_poll();

  // squares and screen positions
  Square square_at(int x, int y) const;
  void square_xy(Square s, int& x, int& y) const;
  bool human_side(Color c) const;

  // drawing
  void draw();
  void draw_background();
  void draw_board();
  void draw_eval_bar();
  void draw_panel();
  void draw_player_card(int x, int y, int w, int h, Color side);
  void draw_moves(int x, int y, int w, int h);
  void draw_buttons();
  void draw_dialog();
  void draw_cursor();
  void button(int x, int y, int w, int h, int id, const std::string& label, const std::string& hint,
              bool primary = false, bool enabled = true);
  void present_full();
  void present_cursor(int oldX, int oldY);

  // state
  Platform::Screen screen;
  Canvas scene;           // everything but the mouse pointer
  Canvas front;           // what is on the screen
  Game::Chess game;
  Mode mode = VS_ENGINE;
  int level = 3;
  int sideChoice = 0;     // 0 White, 1 Black, 2 random
  Color humanColor = WHITE;
  bool flipped = false;
  bool exitWhenOver = false;
  bool showDialog = true;
  Dialog dialog = NONE;
  int dialogRow = 0;
  std::string startFen = Game::StartFEN;
  std::string fenError;

  Square selected = SQ_NONE;
  Square cursor = SQ_E2;
  bool cursorVisible = false;
  bool dragging = false;
  int mouseX = 0, mouseY = 0;
  int drawnMouseX = -1, drawnMouseY = -1;
  std::string typedMove;
  Square promoFrom = SQ_NONE, promoTo = SQ_NONE;
  Move hint = MOVE_NONE;
  SearchReport eval;
  std::string notice;
  uint64_t noticeUntil = 0;
  bool pgnSaved = false;
  bool running = true;
  bool dirty = true;
  bool thinking = false;
  uint64_t thinkStart = 0, lastPoll = 0;
  uint64_t overAt = 0;
  // animation
  bool animating = false;
  Move animMove = MOVE_NONE;
  Piece animPiece = NO_PIECE;
  double animT = 0;

  // layout
  int sq = 80, bx = 0, by = 0, evalX = 0, evalW = 14, panelX = 0, panelW = 260;
  std::vector<Button> buttons;

  static ChessApp* instance;
};

ChessApp* ChessApp::instance = nullptr;


ChessApp::ChessApp(const std::string& args) {
  instance = this;
  parse_args(args);
}


void ChessApp::parse_args(const std::string& args) {
  estd::istringstream is(args);
  std::string w;
  while (is >> w) {
      std::string u = w;
      for (auto& c : u) c = char(toupper((unsigned char)c));
      showDialog = false;
      if (u == "WHITE") { mode = VS_ENGINE; sideChoice = 0; }
      else if (u == "BLACK") { mode = VS_ENGINE; sideChoice = 1; }
      else if (u == "RANDOM") { mode = VS_ENGINE; sideChoice = 2; }
      else if (u == "2P" || u == "HUMAN") mode = TWO_PLAYERS;
      else if (u == "WATCH" || u == "DEMO") mode = WATCH;
      else if (u == "EXIT") exitWhenOver = true;
      else if (u == "LEVEL") { int n = 0; is >> n; if (n >= 1 && n <= 8) level = n; }
      else if (u.size() == 1 && u[0] >= '1' && u[0] <= '8') level = u[0] - '0';
      else if (u == "FEN") {
          std::string fen, part;
          while (is >> part) {
              std::string pu = part;
              for (auto& c : pu) c = char(toupper((unsigned char)c));
              if (pu == "EXIT") { exitWhenOver = true; break; }
              fen += (fen.empty() ? "" : " ") + part;
          }
          startFen = fen;
      }
  }
}


void ChessApp::start_game() {
  fenError = game.reset(startFen);
  if (!fenError.empty()) {
      notice = "Bad FEN: " + fenError;
      noticeUntil = Platform::now_us() + 8000000;
      startFen = Game::StartFEN;
      game.reset(startFen);
  }
  if (mode == VS_ENGINE)
      humanColor = sideChoice == 2 ? (Platform::now_us() & 1 ? WHITE : BLACK) : (sideChoice == 1 ? BLACK : WHITE);
  else
      humanColor = WHITE;
  flipped = mode == VS_ENGINE && humanColor == BLACK;
  selected = SQ_NONE;
  hint = MOVE_NONE;
  typedMove.clear();
  eval = SearchReport();
  pgnSaved = false;
  overAt = 0;
  UCI::execute("ucinewgame");
  dirty = true;
}


void ChessApp::layout() {
  int W = screen.width, H = screen.height;
  int margin = std::max(16, H / 28);
  panelW = std::max(250, std::min(340, W * 27 / 100));
  int gap = 26, pad = 8;
  evalW = std::max(10, H / 60);
  int byHeight = (H - 2 * margin - 2 * pad) / 8;
  int byWidth = (W - 2 * margin - evalW - 12 - 2 * pad - gap - panelW) / 8;
  sq = std::max(24, std::min(byHeight, byWidth));
  int content = evalW + 12 + pad + 8 * sq + pad + gap + panelW;
  int left = (W - content) / 2;
  evalX = left;
  bx = left + evalW + 12 + pad;
  by = (H - 8 * sq) / 2;
  panelX = bx + 8 * sq + pad + gap;
  panelW = std::min(panelW, W - panelX - margin / 2);
}


bool ChessApp::human_side(Color c) const {
  if (mode == TWO_PLAYERS) return true;
  if (mode == WATCH) return false;
  return c == humanColor;
}


bool ChessApp::engine_to_move() const {
  return dialog == NONE && !animating && game.ending() == Game::ONGOING
      && !human_side(game.side_to_move());
}


Square ChessApp::square_at(int x, int y) const {
  if (x < bx || y < by || x >= bx + 8 * sq || y >= by + 8 * sq) return SQ_NONE;
  int f = (x - bx) / sq, r = 7 - (y - by) / sq;
  if (flipped) { f = 7 - f; r = 7 - r; }
  return make_square(File(f), Rank(r));
}


void ChessApp::square_xy(Square s, int& x, int& y) const {
  int f = file_of(s), r = rank_of(s);
  if (flipped) { f = 7 - f; r = 7 - r; }
  x = bx + f * sq;
  y = by + (7 - r) * sq;
}

// ---------------------------------------------------------------- flow

// The search runs to the end of its time.  Every 1024 nodes Stockfish
// calls back here, which keeps the pointer moving and the "thinking" line
// alive, and lets Esc or Space tell it to move now.
void ChessApp::search_poll() {
  ChessApp* a = instance;
  if (!a || !a->thinking) return;
  Platform::Event e;
  while (Platform::next_event(e)) {
      if (e.type == Platform::EV_MOUSE_MOVE || e.type == Platform::EV_MOUSE_DOWN) {
          a->mouseX = e.x; a->mouseY = e.y;
      } else if (e.type == Platform::EV_KEY) {
          int k = e.key & 0x1FF;
          if (k == K_ESC || k == K_SPACE) Threads.stop = true;
      }
  }
  uint64_t now = Platform::now_us();
  if (now - a->lastPoll > 120000) {
      a->lastPoll = now;
      a->draw();
      a->present_full();
  } else if (a->mouseX != a->drawnMouseX || a->mouseY != a->drawnMouseY) {
      a->present_cursor(a->drawnMouseX, a->drawnMouseY);
  }
}


void ChessApp::engine_move(bool asHint) {
  const Level& lv = Levels[level];
  Report = SearchReport();
  ReportSide = game.side_to_move();
  estd::cout.sink = engine_line;
  estd::cerr.discard = true;

  UCI::execute(std::string("setoption name Skill Level value ") + std::to_string(asHint ? 20 : lv.skill));
  UCI::execute(game.uci_position());
  thinking = true;
  thinkStart = Platform::now_us();
  lastPoll = 0;
  dirty = true;
  draw();
  present_full();
  Platform::search_poll = search_poll;
  UCI::execute("go movetime " + std::to_string(asHint ? 1000 : lv.ms));
  Platform::search_poll = nullptr;
  thinking = false;

  if (Report.valid) eval = Report;
  Move m = MOVE_NONE;
  for (Move lm : game.legal_moves())
      if (UCI::move(lm, false) == Report.best) m = lm;
  if (m == MOVE_NONE) return;

  if (asHint) {
      hint = m;
      notice = "Hint: " + game.san(m);
      noticeUntil = Platform::now_us() + 4000000;
      dirty = true;
      return;
  }
  Piece p = game.position().moved_piece(m);
  animate(m, p);
  game.play(m);
  after_move();
}


void ChessApp::human_move(Move m) {
  Piece p = game.position().moved_piece(m);
  selected = SQ_NONE;
  hint = MOVE_NONE;
  typedMove.clear();
  if (!dragging)
      animate(m, p);
  dragging = false;
  game.play(m);
  after_move();
}


void ChessApp::after_move() {
  dirty = true;
  if (game.ending() != Game::ONGOING) {
      save_pgn();
      overAt = Platform::now_us();
      if (!exitWhenOver) {
          dialog = GAME_OVER;
      }
  }
}


void ChessApp::animate(Move m, Piece piece) {
  animating = true;
  animMove = m;
  animPiece = piece;
  uint64_t t0 = Platform::now_us();
  const double length = 180000.0;
  for (;;) {
      double t = (Platform::now_us() - t0) / length;
      if (t >= 1) break;
      animT = 1 - (1 - t) * (1 - t) * (1 - t);    // ease out
      draw();
      present_full();
  }
  animating = false;
  dirty = true;
}


void ChessApp::undo() {
  if (thinking) return;
  if (mode == VS_ENGINE) {
      // Back to the player's own turn: usually the reply and the move before it.
      if (game.moves().empty()) return;
      game.undo();
      while (!game.moves().empty() && game.side_to_move() != humanColor)
          game.undo();
      if (game.side_to_move() != humanColor && game.moves().empty() && humanColor == BLACK) {
          // The engine opened; leave its first move in place.
      }
  } else {
      game.undo();
  }
  selected = SQ_NONE;
  hint = MOVE_NONE;
  pgnSaved = false;
  overAt = 0;
  dialog = NONE;
  dirty = true;
}


void ChessApp::save_pgn() {
  if (game.moves().empty() || pgnSaved) return;
  std::string lvl = std::string("Stockfish 11 (level ") + std::to_string(level) + ")";
  std::string white = mode == WATCH ? lvl : mode == TWO_PLAYERS ? "White" : humanColor == WHITE ? "Player" : lvl;
  std::string black = mode == WATCH ? lvl : mode == TWO_PLAYERS ? "Black" : humanColor == BLACK ? "Player" : lvl;
  std::string pgn = game.pgn({ { "Event", "Casual game" }, { "Site", "Ember" },
                               { "Date", Platform::date() }, { "Round", "-" },
                               { "White", white }, { "Black", black } });
  if (Platform::write_file("CHESS.PGN", pgn)) {
      pgnSaved = true;
      notice = "Game saved to CHESS.PGN";
      noticeUntil = Platform::now_us() + 3000000;
  }
}

// ---------------------------------------------------------------- input

void ChessApp::handle(const Platform::Event& e) {
  switch (e.type) {
  case Platform::EV_MOUSE_MOVE:
      mouseX = e.x; mouseY = e.y;
      cursorVisible = false;
      if (dragging) dirty = true;
      break;
  case Platform::EV_MOUSE_DOWN:
      mouseX = e.x; mouseY = e.y;
      if (dialog != NONE) dialog_click(e.x, e.y);
      else mouse_down(e.x, e.y);
      break;
  case Platform::EV_MOUSE_UP:
      mouseX = e.x; mouseY = e.y;
      if (dialog == NONE) mouse_up(e.x, e.y);
      break;
  case Platform::EV_RIGHT_DOWN:
      selected = SQ_NONE; dragging = false; typedMove.clear(); dirty = true;
      break;
  case Platform::EV_KEY:
      if (dialog != NONE) dialog_key(e);
      else key(e);
      break;
  default:
      break;
  }
}


void ChessApp::press(int id) {
  switch (id) {
  case B_NEW:    dialog = NEW_GAME; dialogRow = 0; break;
  case B_UNDO:   undo(); break;
  case B_FLIP:   flipped = !flipped; break;
  case B_HINT:
      if (game.ending() == Game::ONGOING && human_side(game.side_to_move()))
          engine_move(true);
      break;
  case B_LEVEL_DOWN: case B_DLG_LVL_DOWN: level = std::max(1, level - 1); break;
  case B_LEVEL_UP:   case B_DLG_LVL_UP:   level = std::min(8, level + 1); break;
  case B_HELP:   dialog = HELP; break;
  case B_MODE0: mode = VS_ENGINE; dialogRow = 0; break;
  case B_MODE1: mode = TWO_PLAYERS; dialogRow = 0; break;
  case B_MODE2: mode = WATCH; dialogRow = 0; break;
  case B_SIDE0: sideChoice = 0; dialogRow = 1; break;
  case B_SIDE1: sideChoice = 1; dialogRow = 1; break;
  case B_SIDE2: sideChoice = 2; dialogRow = 1; break;
  case B_DLG_START: dialog = NONE; start_game(); break;
  case B_DLG_NEW_GAME: dialog = NEW_GAME; dialogRow = 0; break;
  case B_DLG_CANCEL: case B_DLG_CLOSE: dialog = NONE; break;
  case B_DLG_QUIT: save_pgn(); running = false; break;
  case B_DLG_STAY: dialog = NONE; break;
  case B_PROMO_Q: case B_PROMO_R: case B_PROMO_B: case B_PROMO_N: {
      static const PieceType kinds[] = { QUEEN, ROOK, BISHOP, KNIGHT };
      Move m = game.find_move(promoFrom, promoTo, kinds[id - B_PROMO_Q]);
      dialog = NONE;
      if (m != MOVE_NONE) human_move(m);
      break;
  }
  default: break;
  }
  dirty = true;
}


void ChessApp::try_move(Square from, Square to) {
  if (from == SQ_NONE || to == SQ_NONE) return;
  if (game.is_promotion(from, to)) {
      promoFrom = from;
      promoTo = to;
      dialog = PROMOTION;
      dialogRow = 0;
      dragging = false;
      dirty = true;
      return;
  }
  Move m = game.find_move(from, to);
  if (m != MOVE_NONE)
      human_move(m);
}


void ChessApp::mouse_down(int x, int y) {
  for (const Button& b : buttons)
      if (b.contains(x, y)) { press(b.id); return; }

  Square s = square_at(x, y);
  if (s == SQ_NONE || game.ending() != Game::ONGOING || !human_side(game.side_to_move())) {
      selected = SQ_NONE;
      dirty = true;
      return;
  }
  Piece pc = game.position().piece_on(s);
  if (selected != SQ_NONE && s != selected) {
      Move m = game.find_move(selected, s, QUEEN);
      if (m != MOVE_NONE) { try_move(selected, s); return; }
  }
  if (pc != NO_PIECE && color_of(pc) == game.side_to_move()) {
      selected = s;
      dragging = true;
  } else
      selected = SQ_NONE;
  dirty = true;
}


void ChessApp::mouse_up(int x, int y) {
  if (!dragging) return;
  dragging = false;
  Square s = square_at(x, y);
  if (s != SQ_NONE && s != selected && game.find_move(selected, s, QUEEN) != MOVE_NONE) {
      dragging = true;             // the piece is already where it lands: no slide
      try_move(selected, s);
      dragging = false;
  }
  dirty = true;
}


void ChessApp::typed(char c) {
  typedMove += c;
  if (typedMove.size() < 4) { dirty = true; return; }
  Square from = make_square(File(typedMove[0] - 'a'), Rank(typedMove[1] - '1'));
  Square to = make_square(File(typedMove[2] - 'a'), Rank(typedMove[3] - '1'));
  if (game.find_move(from, to, QUEEN) != MOVE_NONE) {
      selected = from;
      try_move(from, to);
  } else {
      notice = "Not a legal move: " + typedMove;
      noticeUntil = Platform::now_us() + 2500000;
  }
  typedMove.clear();
  dirty = true;
}


void ChessApp::key(const Platform::Event& e) {
  int k = e.key & 0x1FF;
  char ch = char(e.ch);
  switch (k) {
  case K_F1: press(B_HELP); return;
  case K_F2: press(B_NEW); return;
  case K_F3: press(B_UNDO); return;
  case K_F4: press(B_FLIP); return;
  case K_F5: press(B_HINT); return;
  case K_ESC:
      if (selected != SQ_NONE || !typedMove.empty()) { selected = SQ_NONE; typedMove.clear(); }
      else dialog = QUIT;
      dirty = true;
      return;
  case K_BACKSPACE:
      if (!typedMove.empty()) typedMove.pop_back();
      dirty = true;
      return;
  case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
      int df = k == K_LEFT ? -1 : k == K_RIGHT ? 1 : 0;
      int dr = k == K_UP ? 1 : k == K_DOWN ? -1 : 0;
      if (flipped) { df = -df; dr = -dr; }
      if (cursorVisible) {
          int f = std::max(0, std::min(7, int(file_of(cursor)) + df));
          int r = std::max(0, std::min(7, int(rank_of(cursor)) + dr));
          cursor = make_square(File(f), Rank(r));
      }
      cursorVisible = true;
      dirty = true;
      return;
  }
  case K_ENTER: case K_SPACE:
      if (!cursorVisible) { cursorVisible = true; dirty = true; return; }
      if (game.ending() != Game::ONGOING || !human_side(game.side_to_move())) return;
      if (selected != SQ_NONE && cursor != selected
          && game.find_move(selected, cursor, QUEEN) != MOVE_NONE) {
          try_move(selected, cursor);
      } else {
          Piece pc = game.position().piece_on(cursor);
          selected = (pc != NO_PIECE && color_of(pc) == game.side_to_move() && cursor != selected)
                   ? cursor : SQ_NONE;
      }
      dirty = true;
      return;
  default:
      break;
  }
  if (ch == '+' || ch == '=') { press(B_LEVEL_UP); return; }
  if (ch == '-' || ch == '_') { press(B_LEVEL_DOWN); return; }
  // Typing a move: e2e4.  Letters a-h and digits 1-8 in turn.
  if (game.ending() != Game::ONGOING || !human_side(game.side_to_move())) return;
  char lower = char(tolower((unsigned char)ch));
  bool wantFile = typedMove.size() % 2 == 0;
  if (wantFile && lower >= 'a' && lower <= 'h') typed(lower);
  else if (!wantFile && ch >= '1' && ch <= '8') typed(ch);
}


void ChessApp::dialog_key(const Platform::Event& e) {
  int k = e.key & 0x1FF;
  char ch = char(tolower((unsigned char)e.ch));
  switch (dialog) {
  case NEW_GAME:
      if (k == K_ESC) { press(B_DLG_CANCEL); return; }
      if (k == K_ENTER) { press(B_DLG_START); return; }
      if (k == K_UP) dialogRow = std::max(0, dialogRow - 1);
      if (k == K_DOWN || k == K_TAB) dialogRow = std::min(2, dialogRow + 1);
      if (k == K_LEFT || k == K_RIGHT) {
          int d = k == K_LEFT ? -1 : 1;
          if (dialogRow == 0) mode = Mode((int(mode) + 3 + d) % 3);
          else if (dialogRow == 1) sideChoice = (sideChoice + 3 + d) % 3;
          else level = std::max(1, std::min(8, level + d));
      }
      dirty = true;
      return;
  case PROMOTION:
      if (k == K_ESC) { dialog = NONE; selected = SQ_NONE; dirty = true; return; }
      if (ch == 'q' || ch == '1') press(B_PROMO_Q);
      else if (ch == 'r' || ch == '2') press(B_PROMO_R);
      else if (ch == 'b' || ch == '3') press(B_PROMO_B);
      else if (ch == 'n' || ch == '4') press(B_PROMO_N);
      else if (k == K_LEFT) { dialogRow = (dialogRow + 3) % 4; dirty = true; }
      else if (k == K_RIGHT) { dialogRow = (dialogRow + 1) % 4; dirty = true; }
      else if (k == K_ENTER || k == K_SPACE) press(B_PROMO_Q + dialogRow);
      return;
  case GAME_OVER:
      if (k == K_ENTER || k == K_F2) { dialog = NEW_GAME; dialogRow = 0; dirty = true; }
      else if (k == K_ESC) press(B_DLG_CLOSE);
      else if (k == K_F3) press(B_UNDO);
      return;
  case HELP:
      if (k == K_ESC || k == K_ENTER || k == K_F1) press(B_DLG_CLOSE);
      return;
  case QUIT:
      if (k == K_ENTER || ch == 'y') press(B_DLG_QUIT);
      else if (k == K_ESC || ch == 'n') press(B_DLG_STAY);
      return;
  default:
      return;
  }
}


void ChessApp::dialog_click(int x, int y) {
  for (const Button& b : buttons)
      if (b.id >= B_DLG_START && b.contains(x, y)) { press(b.id); return; }
  if (dialog == PROMOTION) { dialog = NONE; selected = SQ_NONE; dirty = true; }
}

// ---------------------------------------------------------------- drawing

void ChessApp::button(int x, int y, int w, int h, int id, const std::string& label,
                      const std::string& hintText, bool primary, bool enabled) {
  bool hover = enabled && mouseX >= x && mouseY >= y && mouseX < x + w && mouseY < y + h
            && Platform::mouse_present();
  Rgb bg = primary ? (hover ? Rgb(248, 184, 98) : Accent) : (hover ? PanelLine : PanelHi);
  Rgb fg = primary ? AccentDark : (enabled ? Text : Faint);
  scene.round_rect(x, y, w, h, 9, bg);
  if (hintText.empty())
      scene.text_centered(font_bold, x + w / 2, y + h / 2 + 5, label, fg);
  else {
      scene.text_centered(font_bold, x + w / 2, y + h / 2, label, fg);
      scene.text_centered(font_small, x + w / 2, y + h / 2 + 15, hintText, primary ? AccentDark : Dim,
                          primary ? 200 : 255);
  }
  if (enabled) buttons.push_back({ x, y, w, h, id });
}


void ChessApp::draw_background() {
  scene.gradient(0, 0, scene.w, scene.h, BgTop, BgBottom);
}


void ChessApp::draw_board() {
  const Position& pos = game.position();
  int pad = std::max(6, sq / 10);
  scene.shadow(bx - pad, by - pad, 8 * sq + 2 * pad, 8 * sq + 2 * pad, 12, 14, 110);
  scene.round_rect(bx - pad, by - pad, 8 * sq + 2 * pad, 8 * sq + 2 * pad, 12, Frame);

  Square checkSq = game.in_check() ? pos.square<KING>(pos.side_to_move()) : SQ_NONE;
  Move last = game.moves().empty() ? MOVE_NONE : game.moves().back().move;
  if (animating) last = MOVE_NONE;
  auto landing = [](Move m) {
      Square f = from_sq(m), t = to_sq(m);
      return type_of(m) == CASTLING ? make_square(t > f ? FILE_G : FILE_C, rank_of(f)) : t;
  };

  for (int r = 0; r < 8; ++r)
      for (int f = 0; f < 8; ++f) {
          Square s = make_square(File(f), Rank(r));
          int x, y;
          square_xy(s, x, y);
          bool light = (f + r) % 2 == 1;
          scene.fill(x, y, sq, sq, light ? LightSq : DarkSq);
          if (last != MOVE_NONE && (s == from_sq(last) || s == landing(last)))
              scene.fill_alpha(x, y, sq, sq, LastMove, 105);
          if (s == selected)
              scene.fill_alpha(x, y, sq, sq, Selected, 120);
          if (hint != MOVE_NONE && (s == from_sq(hint) || s == landing(hint)))
              scene.round_frame(x + 2, y + 2, sq - 4, sq - 4, 6, std::max(3, sq / 18), HintSq, 230);
          if (s == checkSq)
              for (int i = 6; i >= 1; --i)
                  scene.circle(x + sq / 2.0, y + sq / 2.0, sq * (0.18 + 0.055 * i), Bad, 34);
      }

  // coordinates, in the corner squares' opposite colour
  for (int i = 0; i < 8; ++i) {
      Square rankSq = make_square(flipped ? FILE_H : FILE_A, Rank(flipped ? i : 7 - i));
      int x, y;
      square_xy(rankSq, x, y);
      bool light = (file_of(rankSq) + rank_of(rankSq)) % 2 == 1;
      scene.text(font_coord, x + 3, y + font_coord.ascent + 1, std::string(1, char('1' + rank_of(rankSq))),
                 light ? DarkSq : LightSq);
      Square fileSq = make_square(File(flipped ? 7 - i : i), flipped ? RANK_8 : RANK_1);
      square_xy(fileSq, x, y);
      light = (file_of(fileSq) + rank_of(fileSq)) % 2 == 1;
      scene.text_right(font_coord, x + sq - 3, y + sq - 3, std::string(1, char('a' + file_of(fileSq))),
                       light ? DarkSq : LightSq);
  }

  // where the selected piece may go
  if (selected != SQ_NONE)
      for (Move m : game.legal_moves()) {
          if (from_sq(m) != selected) continue;
          Square t = landing(m);
          int x, y;
          square_xy(t, x, y);
          if (pos.piece_on(to_sq(m)) != NO_PIECE && type_of(m) != CASTLING)
              scene.ring(x + sq / 2.0, y + sq / 2.0, sq * 0.47, sq * 0.085, Rgb(20, 30, 20), 80);
          else
              scene.circle(x + sq / 2.0, y + sq / 2.0, sq * 0.15, Rgb(20, 30, 20), 75);
      }

  if (cursorVisible && dialog == NONE) {
      int x, y;
      square_xy(cursor, x, y);
      scene.round_frame(x + 1, y + 1, sq - 2, sq - 2, 5, std::max(3, sq / 22), Rgb(255, 255, 255), 210);
  }

  // pieces
  int ps = piece_size(sq);
  int off = (sq - ps) / 2;
  Square animFrom = SQ_NONE, animTo = SQ_NONE, animRook = SQ_NONE, animRookTo = SQ_NONE;
  if (animating) {
      animFrom = from_sq(animMove);
      animTo = landing(animMove);
      if (type_of(animMove) == CASTLING) {
          animRook = to_sq(animMove);
          animRookTo = make_square(animTo > animFrom ? FILE_F : FILE_D, rank_of(animFrom));
      }
  }
  for (Square s = SQ_A1; s <= SQ_H8; ++s) {
      Piece pc = pos.piece_on(s);
      if (pc == NO_PIECE || s == animFrom || s == animRook) continue;
      int x, y;
      square_xy(s, x, y);
      int alpha = 255;
      if (dragging && s == selected) alpha = 80;
      if (animating && s == animTo) alpha = int(255 * (1 - animT));
      piece(scene, type_of(pc) - 1, color_of(pc) == WHITE, ps, x + off, y + off, alpha);
  }
  if (animating) {
      auto slide = [&](Square from, Square to, Piece pc) {
          int x0, y0, x1, y1;
          square_xy(from, x0, y0);
          square_xy(to, x1, y1);
          int x = int(x0 + (x1 - x0) * animT), y = int(y0 + (y1 - y0) * animT);
          piece(scene, type_of(pc) - 1, color_of(pc) == WHITE, ps, x + off, y + off);
      };
      if (animRook != SQ_NONE) slide(animRook, animRookTo, pos.piece_on(animRook));
      slide(animFrom, animTo, animPiece);
  }
  if (dragging && selected != SQ_NONE) {
      Piece pc = pos.piece_on(selected);
      if (pc != NO_PIECE)
          piece(scene, type_of(pc) - 1, color_of(pc) == WHITE, ps, mouseX - ps / 2, mouseY - ps / 2);
  }
}


void ChessApp::draw_eval_bar() {
  int h = 8 * sq, x = evalX, y = by;
  scene.round_rect(x, y, evalW, h, evalW / 2, Rgb(26, 28, 34));
  // White's share of the bar: 50% at equality, following a logistic curve
  // of the score so a pawn is a visible step and a rook nearly the end.
  double share = 0.5;
  if (eval.valid) {
      if (eval.mate) share = eval.mateIn > 0 ? 1.0 : 0.0;
      else share = 1.0 / (1.0 + std::exp(-eval.score / 350.0));
  }
  int whiteH = int(h * share);
  int inner = 2;
  int fillH = std::max(0, whiteH - inner);
  if (!flipped)
      scene.round_rect(x + inner, y + h - inner - fillH, evalW - 2 * inner, fillH, (evalW - 2 * inner) / 2, Rgb(236, 234, 228));
  else
      scene.round_rect(x + inner, y + inner, evalW - 2 * inner, fillH, (evalW - 2 * inner) / 2, Rgb(236, 234, 228));
  scene.fill_alpha(x, y + h / 2, evalW, 1, Accent, 160);
}


void ChessApp::draw_player_card(int x, int y, int w, int h, Color side) {
  const Position& pos = game.position();
  bool toMove = game.ending() == Game::ONGOING && pos.side_to_move() == side;
  scene.round_rect(x, y, w, h, 12, Panel);
  if (toMove)
      scene.round_frame(x, y, w, h, 12, 2, Accent, 220);

  int cx = x + 30, cy = y + 30;
  scene.circle(cx, cy, 19, side == WHITE ? Rgb(236, 232, 222) : Rgb(22, 24, 30));
  scene.ring(cx, cy, 19, 1.5, PanelLine);
  piece(scene, KING - 1, side == WHITE, 24, cx - 12, cy - 13, 255, false);

  std::string name, sub;
  if (mode == TWO_PLAYERS) { name = side == WHITE ? "White" : "Black"; sub = "Player"; }
  else if (mode == WATCH || side != humanColor) {
      name = "Stockfish 11";
      sub = std::string("Level ") + std::to_string(level) + " " + MidDot + " " + Levels[level].name;
  } else { name = "You"; sub = side == WHITE ? "White" : "Black"; }
  scene.text(font_bold, x + 58, y + 26, name, Text);
  scene.text(font_small, x + 58, y + 43, sub, Dim);

  // pieces this side has taken, and the material it is ahead by
  std::vector<Piece> taken = game.lost(~side);
  int px_ = x + 14, py = y + h - 30;
  for (size_t i = 0; i < taken.size(); ++i) {
      piece(scene, type_of(taken[i]) - 1, color_of(taken[i]) == WHITE, 24, px_, py, 255, false);
      px_ += (i + 1 < taken.size() && type_of(taken[i + 1]) == type_of(taken[i])) ? 11 : 17;
      if (px_ > x + w - 60) break;
  }
  int bal = game.material_balance() * (side == WHITE ? 1 : -1);
  if (bal > 0)
      scene.text(font_bold, px_ + 10, py + 17, "+" + std::to_string(bal), Dim);
  if (toMove && thinking && side != humanColor) {
      int dots = int((Platform::now_us() - thinkStart) / 300000) % 4;
      scene.text_right(font_small, x + w - 12, y + 26, std::string("thinking") + std::string(dots, '.'), Accent);
  }
}


void ChessApp::draw_moves(int x, int y, int w, int h) {
  scene.round_rect(x, y, w, h, 12, Panel);
  scene.text(font_bold, x + 14, y + 22, "Moves", Dim);
  const auto& mv = game.moves();
  int rowH = 23;
  int top = y + 34;
  int rows = std::max(1, (h - 44) / rowH);

  std::vector<std::string> fields;
  estd::istringstream is(game.start_fen());
  std::string tok;
  while (is >> tok) fields.push_back(tok);
  bool blackFirst = fields.size() > 1 && fields[1] == "b";
  int startNo = fields.size() > 5 ? atoi(fields[5].c_str()) : 1;

  int total = int(mv.size()) + (blackFirst ? 1 : 0);
  int lines = (total + 1) / 2;
  int first = std::max(0, lines - rows);
  int colW = (w - 54) / 2;
  for (int line = first; line < lines; ++line) {
      int yy = top + (line - first) * rowH;
      scene.text_right(font_move, x + 44, yy + 16, std::to_string(startNo + line) + ".", Faint);
      for (int c = 0; c < 2; ++c) {
          int idx = line * 2 + c - (blackFirst ? 1 : 0);
          if (idx < 0 || idx >= int(mv.size())) continue;
          int tx = x + 54 + c * colW;
          bool latest = idx == int(mv.size()) - 1;
          if (latest)
              scene.round_rect(tx - 6, yy, colW - 6, rowH - 2, 6, PanelHi);
          scene.text(latest ? font_moveb : font_move, tx, yy + 16, mv[idx].san, latest ? Text : Rgb(208, 212, 222));
      }
  }
  if (mv.empty())
      scene.text(font_body, x + 14, top + 16, "No moves yet", Faint);
}


void ChessApp::draw_panel() {
  int x = panelX, w = panelW;
  int y = by - std::max(6, sq / 10);
  int bottom = by + 8 * sq + std::max(6, sq / 10);

  scene.text(font_title, x, y + 24, "Chess", Text);
  scene.text(font_small, x + Canvas::text_width(font_title, "Chess") + 10, y + 23,
             std::string("Stockfish 11 ") + MidDot + " Ember", Dim);

  Color topSide = flipped ? WHITE : BLACK;
  int cardH = 86;
  int cardY = y + 40;
  draw_player_card(x, cardY, w, cardH, topSide);

  // status
  int sy = cardY + cardH + 10;
  std::string status, detail;
  Rgb statusColor = Text;
  if (game.ending() != Game::ONGOING) {
      status = game.ending_text();
      Color win = game.winner();
      detail = win == COLOR_NB ? "Draw" : (win == WHITE ? "White wins" : "Black wins");
      statusColor = Accent;
  } else if (thinking) {
      status = "Stockfish is thinking";
      detail = "depth " + std::to_string(Report.depth) + "  " + MidDot + "  "
             + std::to_string(Report.nodes / 1000) + "k positions  " + MidDot + "  Esc: move now";
  } else {
      Color stm = game.side_to_move();
      if (mode == VS_ENGINE) status = stm == humanColor ? "Your move" : "Stockfish to move";
      else status = stm == WHITE ? "White to move" : "Black to move";
      if (game.in_check()) { status += " " + std::string(MidDot) + " Check"; statusColor = Bad; }
      if (!typedMove.empty()) detail = "Typing: " + typedMove;
      else if (eval.valid)
          detail = eval.mate ? std::string("Mate in ") + std::to_string(std::abs(eval.mateIn)) + " for "
                               + (eval.mateIn > 0 ? "White" : "Black")
                             : std::string("Evaluation ") + (eval.score >= 0 ? "+" : "\x83")
                               + std::to_string(std::abs(eval.score) / 100) + "."
                               + std::string(std::abs(eval.score) % 100 < 10 ? "0" : "")
                               + std::to_string(std::abs(eval.score) % 100)
                               + " (depth " + std::to_string(eval.depth) + ")";
      else detail = "Drag a piece, or type a move like e2e4";
  }
  if (Platform::now_us() < noticeUntil) detail = notice;
  scene.text(font_large, x + 2, sy + 20, status, statusColor);
  scene.text(font_small, x + 2, sy + 40, detail, Dim);

  // buttons at the bottom, the second player card above them
  int bh = 46, bgap = 8;
  int buttonsY = bottom - 2 * bh - bgap;
  int card2Y = buttonsY - 12 - cardH;
  int movesY = sy + 54;
  draw_moves(x, movesY, w, card2Y - 10 - movesY);
  draw_player_card(x, card2Y, w, cardH, ~topSide);

  int bw = (w - 2 * bgap) / 3;
  button(x, buttonsY, bw, bh, B_NEW, "New game", "F2");
  button(x + bw + bgap, buttonsY, bw, bh, B_UNDO, "Undo", "F3", false, !game.moves().empty());
  button(x + 2 * (bw + bgap), buttonsY, bw, bh, B_FLIP, "Flip", "F4");
  int y2 = buttonsY + bh + bgap;
  bool canHint = mode != WATCH && game.ending() == Game::ONGOING;
  button(x, y2, bw, bh, B_HINT, "Hint", "F5", false, canHint);
  // level: - value +
  int lx = x + bw + bgap;
  scene.round_rect(lx, y2, bw, bh, 9, PanelHi);
  scene.text_centered(font_bold, lx + bw / 2, y2 + bh / 2, "Level " + std::to_string(level), Text);
  scene.text_centered(font_small, lx + bw / 2, y2 + bh / 2 + 15, "\x83 / +", Dim);
  // the left half lowers the level, the right half raises it
  buttons.push_back({ lx, y2, bw / 2, bh, B_LEVEL_DOWN });
  buttons.push_back({ lx + bw / 2, y2, bw - bw / 2, bh, B_LEVEL_UP });
  button(x + 2 * (bw + bgap), y2, bw, bh, B_HELP, "Help", "F1");
}


void ChessApp::draw_dialog() {
  if (dialog == NONE) return;
  scene.fill_alpha(0, 0, scene.w, scene.h, Rgb(6, 8, 12), 150);
  int w = 460, h = 300;
  if (dialog == NEW_GAME) h = 340;
  if (dialog == PROMOTION) { w = 420; h = 210; }
  if (dialog == HELP) { w = 560; h = 440; }
  if (dialog == QUIT) { w = 380; h = 170; }
  if (dialog == GAME_OVER) { w = 420; h = 250; }
  int x = (scene.w - w) / 2, y = (scene.h - h) / 2;
  scene.shadow(x, y, w, h, 16, 18, 150);
  scene.round_rect(x, y, w, h, 16, Panel);
  scene.round_frame(x, y, w, h, 16, 1, PanelLine);

  auto segment = [&](int row, int sx, int sy, int sw, const std::vector<std::string>& labels,
                     int chosen, int firstId) {
      int n = int(labels.size()), gap = 6;
      int segW = (sw - gap * (n - 1)) / n;
      for (int i = 0; i < n; ++i) {
          int bxx = sx + i * (segW + gap);
          bool on = i == chosen;
          scene.round_rect(bxx, sy, segW, 36, 8, on ? Accent : PanelHi);
          scene.text_centered(font_bold, bxx + segW / 2, sy + 23, labels[i], on ? AccentDark : Text);
          buttons.push_back({ bxx, sy, segW, 36, firstId + i });
      }
      if (dialogRow == row)
          scene.round_frame(sx - 4, sy - 4, sw + 8, 44, 11, 2, Rgb(255, 255, 255), 150);
  };

  switch (dialog) {
  case NEW_GAME: {
      scene.text(font_title, x + 28, y + 46, "New game", Text);
      int lx = x + 28, lw = w - 56;
      scene.text(font_small, lx, y + 82, "Opponent", Dim);
      segment(0, lx, y + 90, lw, { "Stockfish", "Two players", "Watch" }, int(mode), B_MODE0);
      scene.text(font_small, lx, y + 150, "You play", Dim, mode == VS_ENGINE ? 255 : 110);
      segment(1, lx, y + 158, lw, { "White", "Black", "Random" }, sideChoice, B_SIDE0);
      scene.text(font_small, lx, y + 218, "Stockfish's level", Dim);
      scene.round_rect(lx, y + 226, lw, 36, 8, PanelHi);
      buttons.push_back({ lx, y + 226, 60, 36, B_DLG_LVL_DOWN });
      buttons.push_back({ lx + lw - 60, y + 226, 60, 36, B_DLG_LVL_UP });
      scene.text(font_large, lx + 20, y + 250, LeftTri, Dim);
      scene.text_right(font_large, lx + lw - 20, y + 250, RightTri, Dim);
      scene.text_centered(font_bold, lx + lw / 2, y + 249,
                          std::to_string(level) + "  " + MidDot + "  " + Levels[level].name, Text);
      if (dialogRow == 2)
          scene.round_frame(lx - 4, y + 222, lw + 8, 44, 11, 2, Rgb(255, 255, 255), 150);
      scene.text_right(font_small, x + w - 28, y + 44, "Arrows and Enter work too", Faint);
      button(x + w - 28 - 130, y + h - 58, 130, 40, B_DLG_START, "Start", "", true);
      button(x + w - 28 - 130 - 10 - 110, y + h - 58, 110, 40, B_DLG_CANCEL, "Cancel", "");
      break;
  }
  case PROMOTION: {
      scene.text(font_large, x + 28, y + 40, "Promote to", Text);
      static const PieceType kinds[] = { QUEEN, ROOK, BISHOP, KNIGHT };
      static const char* names[] = { "Queen (Q)", "Rook (R)", "Bishop (B)", "Knight (N)" };
      bool white = game.side_to_move() == WHITE;
      int cell = (w - 56 - 30) / 4;
      for (int i = 0; i < 4; ++i) {
          int cx = x + 28 + i * (cell + 10);
          int cy = y + 60;
          bool hot = dialogRow == i || (mouseX >= cx && mouseY >= cy && mouseX < cx + cell && mouseY < cy + cell + 26);
          scene.round_rect(cx, cy, cell, cell + 26, 10, hot ? PanelLine : PanelHi);
          int ps = piece_size(cell - 8);
          piece(scene, kinds[i] - 1, white, ps, cx + (cell - ps) / 2, cy + 2);
          scene.text_centered(font_small, cx + cell / 2, cy + cell + 16, names[i], Dim);
          buttons.push_back({ cx, cy, cell, cell + 26, B_PROMO_Q + i });
      }
      break;
  }
  case GAME_OVER: {
      Color win = game.winner();
      std::string title = game.ending() == Game::CHECKMATE ? "Checkmate" : win == COLOR_NB ? "Draw" : "Game over";
      std::string who;
      if (win == COLOR_NB) who = game.ending_text();
      else if (mode == VS_ENGINE) who = win == humanColor ? "You win!" : "Stockfish wins";
      else who = win == WHITE ? "White wins" : "Black wins";
      scene.text_centered(font_huge, x + w / 2, y + 66, title, Text);
      scene.text_centered(font_large, x + w / 2, y + 104, who, Accent);
      if (win != COLOR_NB && game.ending() != Game::CHECKMATE)
          scene.text_centered(font_body, x + w / 2, y + 130, game.ending_text(), Dim);
      scene.text_centered(font_body, x + w / 2, y + 156,
                          game.result() + "  " + MidDot + "  " + std::to_string((game.moves().size() + 1) / 2) + " moves"
                          + (pgnSaved ? "  " + std::string(MidDot) + "  saved to CHESS.PGN" : ""), Dim);
      button(x + w / 2 + 6, y + h - 62, 150, 42, B_DLG_NEW_GAME, "New game", "", true);
      button(x + w / 2 - 6 - 150, y + h - 62, 150, 42, B_DLG_CLOSE, "Look at board", "");
      break;
  }
  case HELP: {
      scene.text(font_title, x + 28, y + 46, "How to play", Text);
      static const char* rows[][2] = {
        { "Drag a piece", "or click it, then click where it goes" },
        { "Type a move", "e2e4 moves e2 to e4; Backspace corrects" },
        { "Arrows, Enter", "move the square cursor, pick up, put down" },
        { "Esc", "drop the piece in hand; else leave the game" },
        { "F2", "new game: opponent, colour, level" },
        { "F3", "undo (your last move and Stockfish's reply)" },
        { "F4", "turn the board around" },
        { "F5", "hint: Stockfish's move for you" },
        { "+ and \x83", "Stockfish's level, 1 to 8" },
        { "Esc or Space", "while Stockfish thinks: move now" },
      };
      int ry = y + 86;
      for (auto& r : rows) {
          scene.text(font_bold, x + 28, ry, r[0], Accent);
          scene.text(font_body, x + 170, ry, r[1], Text);
          ry += 28;
      }
      scene.text(font_small, x + 28, y + h - 58, "Draws by repetition, the fifty-move rule and insufficient", Dim);
      scene.text(font_small, x + 28, y + h - 42, "material are applied automatically. Games save to CHESS.PGN.", Dim);
      button(x + w - 28 - 110, y + h - 66, 110, 40, B_DLG_CLOSE, "Close", "", true);
      break;
  }
  case QUIT: {
      scene.text(font_large, x + 28, y + 44, "Leave the game?", Text);
      scene.text(font_body, x + 28, y + 72, game.moves().empty() ? "Back to the Ember prompt."
                                                                 : "The game will be saved to CHESS.PGN.", Dim);
      button(x + w - 28 - 120, y + h - 62, 120, 40, B_DLG_QUIT, "Leave", "", true);
      button(x + w - 28 - 120 - 10 - 100, y + h - 62, 100, 40, B_DLG_STAY, "Stay", "");
      break;
  }
  default:
      break;
  }
}


// The pointer: an arrow, drawn last, straight onto the screen buffer.
static const char* Arrow[] = {
  "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
  "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
  "X.X X..X    ", "XX  X..X    ", "X    X..X   ", "     X..X   ", "      XX    ",
};

void ChessApp::draw_cursor() {
  if (!Platform::mouse_present()) return;
  for (int j = 0; j < 17; ++j)
      for (int i = 0; i < 12; ++i) {
          char c = Arrow[j][i];
          if (c == ' ') continue;
          front.blend(mouseX + i, mouseY + j, c == 'X' ? Rgb(0, 0, 0) : Rgb(255, 255, 255), 255);
      }
  drawnMouseX = mouseX;
  drawnMouseY = mouseY;
}


void ChessApp::draw() {
  buttons.clear();
  scene.no_clip();
  draw_background();
  draw_eval_bar();
  draw_board();
  draw_panel();
  if (dialog != NONE) {
      buttons.clear();                          // only the dialog's own buttons respond
      draw_dialog();
  }
  dirty = false;
}


void ChessApp::present_full() {
  memcpy(front.px, scene.px, size_t(scene.w) * scene.h * 4);
  front.no_clip();
  draw_cursor();
  Platform::present(screen, 0, 0, screen.width, screen.height);
}


void ChessApp::present_cursor(int oldX, int oldY) {
  // Put back what the pointer covered, draw it in its new place.
  auto restore = [&](int x, int y) {
      for (int j = 0; j < 17; ++j) {
          int yy = y + j;
          if (yy < 0 || yy >= scene.h) continue;
          for (int i = 0; i < 12; ++i) {
              int xx = x + i;
              if (xx < 0 || xx >= scene.w) continue;
              front.px[yy * scene.w + xx] = scene.px[yy * scene.w + xx];
          }
      }
  };
  if (oldX >= 0) restore(oldX, oldY);
  draw_cursor();
  if (oldX >= 0) Platform::present(screen, oldX, oldY, oldX + 12, oldY + 17);
  Platform::present(screen, mouseX, mouseY, mouseX + 12, mouseY + 17);
}


int ChessApp::run() {
  if (!Platform::open_screen(screen)) {
      Platform::print("CHESS needs a VESA graphics mode of at least 640x480 in 16, 24 or 32-bit colour.");
      return 1;
  }
  scene.w = front.w = screen.width;
  scene.h = front.h = screen.height;
  front.px = screen.pixels;
  scene.px = new uint32_t[size_t(screen.width) * screen.height];
  scene.no_clip();
  front.no_clip();
  mouseX = screen.width / 2;
  mouseY = screen.height / 2;
  layout();

  estd::cout.sink = engine_line;
  estd::cerr.discard = true;
  start_game();
  if (showDialog) dialog = NEW_GAME;

  uint64_t lastEngineMove = 0;
  while (running) {
      Platform::Event e;
      bool got = false;
      while (Platform::next_event(e)) { handle(e); got = true; }

      if (exitWhenOver && overAt && Platform::now_us() - overAt > 1500000) break;

      if (engine_to_move()) {
          // In Watch mode leave a moment to see each move.
          if (mode == WATCH && Platform::now_us() - lastEngineMove < 500000) {
              Platform::idle();
              continue;
          }
          engine_move(false);
          lastEngineMove = Platform::now_us();
          dirty = true;
      }

      if (Platform::now_us() < noticeUntil + 100000) dirty = true;
      if (dirty) {
          draw();
          present_full();
      } else if (mouseX != drawnMouseX || mouseY != drawnMouseY) {
          present_cursor(drawnMouseX, drawnMouseY);
      } else if (!got)
          Platform::idle();
  }

  save_pgn();
  Platform::close_screen();
  Platform::print("Thanks for playing chess.");
  return 0;
}

} // namespace


namespace App {

int run(const std::string& args) {
  ChessApp app(args);
  return app.run();
}

}

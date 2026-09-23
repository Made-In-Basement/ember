// app.cpp - Reversi on Ember: the board, the player, the computer.
//
//   REVERSI                  the new game screen
//   REVERSI BLACK | WHITE    play that colour against the computer (W also works)
//   REVERSI 1 .. 5           the computer's level
//   REVERSI 2P               two players at one keyboard
//   REVERSI WATCH            the computer plays itself (A also works)
//   REVERSI ... EXIT         leave when the game ends, for scripts
//
// The rules and the computer are engine.c, a transcription of the browser
// game's engine, checked against game.js move for move (docs/REVERSI.md).
// The drawing, keyboard, mouse and clock are Chess's (gfx.cpp,
// platform_ember.cpp): the same VESA screen, antialiased shapes and fonts,
// and input handling.
// When a game ends, its moves and score are written to REVERSI.TXT.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "assets.h"
#include "gfx.h"
#include "platform.h"

extern "C" {
#include "engine.h"
int rand(void);
void srand(unsigned);
}

using namespace Gfx;

namespace {

// ---------------------------------------------------------------- look

const Rgb BgTop(33, 37, 48), BgBottom(17, 19, 26);
const Rgb Panel(38, 43, 56), PanelHi(52, 58, 75), PanelLine(62, 69, 88);
const Rgb Text(236, 238, 243), Dim(152, 159, 176), Faint(104, 111, 128);
const Rgb Accent(236, 166, 76), AccentDark(40, 30, 18), Bad(232, 96, 84);
const Rgb Felt(38, 122, 82), FeltDark(30, 102, 68), Grid(20, 70, 46), Frame(58, 43, 33);
const Rgb BlackDisc(28, 28, 32), WhiteDisc(242, 240, 232);

struct Level { int depth; const char* name; };
const Level Levels[6] = { { 0, "" }, { 1, "Beginner" }, { 2, "Easy" }, { 3, "Medium" },
                          { 4, "Hard" }, { 5, "Expert" } };

const char MidDot[] = "\x80";

enum Mode { VS_COMPUTER, TWO_PLAYERS, WATCH };
enum Dialog { NONE, NEW_GAME, GAME_OVER, HELP, QUIT };

enum Key {
  K_ESC = 0x01, K_BACKSPACE = 0x0E, K_TAB = 0x0F, K_ENTER = 0x1C, K_SPACE = 0x39,
  K_F1 = 0x3B, K_F2 = 0x3C, K_F3 = 0x3D, K_F4 = 0x3E,
  K_UP = 0x148, K_DOWN = 0x150, K_LEFT = 0x14B, K_RIGHT = 0x14D,
};

enum ButtonId {
  B_NONE, B_NEW, B_UNDO, B_HINTS, B_LEVEL_DOWN, B_LEVEL_UP, B_HELP,
  B_DLG_START, B_DLG_CANCEL, B_DLG_CLOSE, B_DLG_QUIT, B_DLG_STAY, B_DLG_NEW_GAME,
  B_MODE0, B_MODE1, B_MODE2, B_SIDE0, B_SIDE1, B_DLG_LVL_DOWN, B_DLG_LVL_UP,
};

struct Button {
  int x, y, w, h, id;
  bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct LogEntry { int color; int r, c; bool pass; int flips; };

struct State {
  Board board;
  int turn;
  int lastR, lastC;
  size_t logSize;
};

// ---------------------------------------------------------------- the app

class ReversiApp {
public:
  explicit ReversiApp(const std::string& args);
  int run();

private:
  void parse_args(const std::string& args);
  void start_game();
  void layout();

  bool computer_to_move() const;
  bool human_to_move() const;
  int moves_for(int color) const { return legal_moves(board, color, nullptr); }
  void play(int r, int c);
  void computer_move();
  void advance();
  void animate(int r, int c, const Board& before);
  void undo();
  void save_result();
  std::string result_text() const;

  void handle(const Platform::Event& e);
  void key(const Platform::Event& e);
  void mouse_down(int x, int y);
  void dialog_key(const Platform::Event& e);
  void press(int id);

  bool square_at(int x, int y, int& r, int& c) const;
  void draw();
  void draw_board();
  void draw_score_bar();
  void draw_panel();
  void draw_player_card(int x, int y, int w, int h, int color);
  void draw_log(int x, int y, int w, int h);
  void draw_dialog();
  void draw_disc(double cx, double cy, double radius, int color, double squeeze = 1.0, int alpha = 255);
  void button(int x, int y, int w, int h, int id, const std::string& label, const std::string& hint,
              bool primary = false, bool enabled = true);
  void present_full();
  void present_cursor();

  Platform::Screen screen;
  Canvas scene, front;
  Mode mode = VS_COMPUTER;
  int level = 3;
  int humanColor = BLACK;
  bool exitWhenOver = false;
  bool showDialog = true;
  Dialog dialog = NONE;
  int dialogRow = 0;

  Board board;
  int turn = BLACK;
  int lastR = -1, lastC = -1;
  bool over = false;
  std::vector<LogEntry> log;
  std::vector<State> history;
  std::string notice;
  uint64_t noticeUntil = 0, overAt = 0;
  bool hints = true, resultSaved = false;

  int cursorR = 2, cursorC = 3;
  bool cursorVisible = false;
  char typedFile = 0;
  int mouseX = 0, mouseY = 0, drawnMouseX = -1, drawnMouseY = -1;
  bool dirty = true, running = true, thinking = false;
  uint64_t thinkStart = 0;

  // flip animation
  bool animating = false;
  Board animBefore;
  int animR = -1, animC = -1;
  double animT = 0;

  int sq = 80, bx = 0, by = 0, barX = 0, barW = 14, panelX = 0, panelW = 260;
  std::vector<Button> buttons;
};


ReversiApp::ReversiApp(const std::string& args) { parse_args(args); }


void ReversiApp::parse_args(const std::string& args) {
  size_t i = 0;
  while (i < args.size()) {
      while (i < args.size() && args[i] == ' ') ++i;
      size_t j = i;
      while (j < args.size() && args[j] != ' ') ++j;
      std::string u = args.substr(i, j - i);
      i = j;
      if (u.empty()) continue;
      for (auto& ch : u) ch = char(toupper((unsigned char)ch));
      showDialog = false;
      if (u == "BLACK" || u == "B") { mode = VS_COMPUTER; humanColor = BLACK; }
      else if (u == "WHITE" || u == "W") { mode = VS_COMPUTER; humanColor = WHITE; }
      else if (u == "2P" || u == "HUMAN") mode = TWO_PLAYERS;
      else if (u == "WATCH" || u == "A" || u == "DEMO") mode = WATCH;
      else if (u == "EXIT") exitWhenOver = true;
      else if (u.size() == 1 && u[0] >= '1' && u[0] <= '5') level = u[0] - '0';
  }
}


void ReversiApp::start_game() {
  memset(board, EMPTY, sizeof board);
  board[3 * 8 + 3] = WHITE; board[4 * 8 + 4] = WHITE;
  board[3 * 8 + 4] = BLACK; board[4 * 8 + 3] = BLACK;
  turn = BLACK;
  lastR = lastC = -1;
  over = false;
  log.clear();
  history.clear();
  cursorR = 2; cursorC = 3;
  typedFile = 0;
  overAt = 0;
  resultSaved = false;
  dirty = true;
}


void ReversiApp::layout() {
  int W = screen.width, H = screen.height;
  int margin = std::max(16, H / 28);
  panelW = std::max(250, std::min(340, W * 27 / 100));
  int gap = 26, pad = std::max(6, H / 90);
  barW = std::max(10, H / 60);
  int byHeight = (H - 2 * margin - 2 * pad) / 8;
  int byWidth = (W - 2 * margin - barW - 12 - 2 * pad - gap - panelW) / 8;
  sq = std::max(24, std::min(byHeight, byWidth));
  int content = barW + 12 + pad + 8 * sq + pad + gap + panelW;
  int left = (W - content) / 2;
  barX = left;
  bx = left + barW + 12 + pad;
  by = (H - 8 * sq) / 2;
  panelX = bx + 8 * sq + pad + gap;
  panelW = std::min(panelW, W - panelX - margin / 2);
}


bool ReversiApp::computer_to_move() const {
  if (over || dialog != NONE || animating) return false;
  return mode == WATCH || (mode == VS_COMPUTER && turn != humanColor);
}

bool ReversiApp::human_to_move() const {
  if (over) return false;
  return mode == TWO_PLAYERS || (mode == VS_COMPUTER && turn == humanColor);
}

// ---------------------------------------------------------------- the game

void ReversiApp::play(int r, int c) {
  if (over || !flips_for(board, r, c, turn, nullptr)) return;
  State s;
  memcpy(s.board, board, sizeof board);
  s.turn = turn; s.lastR = lastR; s.lastC = lastC; s.logSize = log.size();
  history.push_back(s);

  Board before;
  memcpy(before, board, sizeof board);
  int flips = apply_move(board, r, c, turn);
  log.push_back({ turn, r, c, false, flips });
  lastR = r; lastC = c;
  typedFile = 0;
  animate(r, c, before);
  advance();
}


// After a move: the other side plays if it can, passes if it cannot while
// this side still can, and the game is over when neither can.
void ReversiApp::advance() {
  int next = opp(turn);
  if (moves_for(next)) { turn = next; dirty = true; return; }
  if (moves_for(turn)) {
      log.push_back({ next, -1, -1, true, 0 });
      notice = std::string(next == BLACK ? "Black" : "White") + " has no move and passes";
      noticeUntil = Platform::now_us() + 3000000;
      dirty = true;
      return;
  }
  over = true;
  overAt = Platform::now_us();
  save_result();
  if (!exitWhenOver) dialog = GAME_OVER;
  dirty = true;
}


void ReversiApp::computer_move() {
  thinking = true;
  thinkStart = Platform::now_us();
  dirty = true;
  draw();
  present_full();
  int r, c;
  bool have = choose_ai_move(board, turn, Levels[level].depth, &r, &c);
  // Let the move be seen coming: at least 450 ms of "thinking".
  while (Platform::now_us() - thinkStart < 450000) {
      Platform::Event e;
      while (Platform::next_event(e))
          if (e.type == Platform::EV_MOUSE_MOVE) { mouseX = e.x; mouseY = e.y; }
      if (mouseX != drawnMouseX || mouseY != drawnMouseY) present_cursor();
      Platform::idle();
  }
  thinking = false;
  if (have) play(r, c);
  else advance();
}


void ReversiApp::animate(int r, int c, const Board& before) {
  animating = true;
  memcpy(animBefore, before, sizeof animBefore);
  animR = r; animC = c;
  uint64_t t0 = Platform::now_us();
  const double length = 320000.0;
  for (;;) {
      double t = (Platform::now_us() - t0) / length;
      if (t >= 1) break;
      animT = t;
      draw();
      present_full();
  }
  animating = false;
  dirty = true;
}


void ReversiApp::undo() {
  if (history.empty() || thinking) return;
  // Against the computer, back to the player's own turn.
  do {
      State s = history.back();
      history.pop_back();
      memcpy(board, s.board, sizeof board);
      turn = s.turn; lastR = s.lastR; lastC = s.lastC;
      log.resize(s.logSize);
  } while (mode == VS_COMPUTER && turn != humanColor && !history.empty());
  over = false;
  overAt = 0;
  resultSaved = false;
  dialog = NONE;
  dirty = true;
}


std::string ReversiApp::result_text() const {
  int b, w;
  count_discs(board, &b, &w);
  std::string s = "Reversi on Ember\r\n";
  s += std::string("Black: ") + (mode == TWO_PLAYERS ? "player" : mode == WATCH || humanColor != BLACK ? "computer" : "player") + "\r\n";
  s += std::string("White: ") + (mode == TWO_PLAYERS ? "player" : mode == WATCH || humanColor != WHITE ? "computer" : "player") + "\r\n";
  s += "Level: " + std::to_string(level) + "\r\nMoves:";
  for (const auto& e : log) {
      s += ' ';
      s += e.color == BLACK ? 'B' : 'W';
      if (e.pass) s += ":pass";
      else { s += ':'; s += char('a' + e.c); s += char('1' + e.r); }
  }
  s += "\r\nScore: " + std::to_string(b) + "-" + std::to_string(w) + "\r\n";
  s += std::string("Result: ") + (b > w ? "Black wins" : w > b ? "White wins" : "Draw") + "\r\n";
  return s;
}


void ReversiApp::save_result() {
  if (resultSaved) return;
  resultSaved = Platform::write_file("REVERSI.TXT", result_text());
}

// ---------------------------------------------------------------- input

bool ReversiApp::square_at(int x, int y, int& r, int& c) const {
  if (x < bx || y < by || x >= bx + 8 * sq || y >= by + 8 * sq) return false;
  c = (x - bx) / sq;
  r = (y - by) / sq;
  return true;
}


void ReversiApp::handle(const Platform::Event& e) {
  switch (e.type) {
  case Platform::EV_MOUSE_MOVE:
      mouseX = e.x; mouseY = e.y;
      cursorVisible = false;
      if (hints && human_to_move() && dialog == NONE) dirty = true;   // the ghost disc follows
      break;
  case Platform::EV_MOUSE_DOWN:
      mouseX = e.x; mouseY = e.y;
      if (dialog != NONE) {
          for (const Button& b : buttons)
              if (b.contains(e.x, e.y)) { press(b.id); break; }
      } else
          mouse_down(e.x, e.y);
      break;
  case Platform::EV_KEY:
      if (dialog != NONE) dialog_key(e);
      else key(e);
      break;
  default:
      break;
  }
}


void ReversiApp::press(int id) {
  switch (id) {
  case B_NEW:        dialog = NEW_GAME; dialogRow = 0; break;
  case B_UNDO:       undo(); break;
  case B_HINTS:      hints = !hints; break;
  case B_LEVEL_DOWN: case B_DLG_LVL_DOWN: level = std::max(1, level - 1); break;
  case B_LEVEL_UP:   case B_DLG_LVL_UP:   level = std::min(5, level + 1); break;
  case B_HELP:       dialog = HELP; break;
  case B_MODE0: mode = VS_COMPUTER; dialogRow = 0; break;
  case B_MODE1: mode = TWO_PLAYERS; dialogRow = 0; break;
  case B_MODE2: mode = WATCH; dialogRow = 0; break;
  case B_SIDE0: humanColor = BLACK; dialogRow = 1; break;
  case B_SIDE1: humanColor = WHITE; dialogRow = 1; break;
  case B_DLG_START: dialog = NONE; start_game(); break;
  case B_DLG_NEW_GAME: dialog = NEW_GAME; dialogRow = 0; break;
  case B_DLG_CANCEL: case B_DLG_CLOSE: case B_DLG_STAY: dialog = NONE; break;
  case B_DLG_QUIT: running = false; break;
  default: break;
  }
  dirty = true;
}


void ReversiApp::mouse_down(int x, int y) {
  for (const Button& b : buttons)
      if (b.contains(x, y)) { press(b.id); return; }
  int r, c;
  if (human_to_move() && square_at(x, y, r, c)) {
      if (flips_for(board, r, c, turn, nullptr)) play(r, c);
      else { notice = "Not a legal move"; noticeUntil = Platform::now_us() + 1500000; dirty = true; }
  }
}


void ReversiApp::key(const Platform::Event& e) {
  int k = e.key & 0x1FF;
  char ch = char(e.ch);
  char lower = char(tolower((unsigned char)ch));
  switch (k) {
  case K_F1: press(B_HELP); return;
  case K_F2: press(B_NEW); return;
  case K_F3: press(B_UNDO); return;
  case K_F4: press(B_HINTS); return;
  case K_ESC:
      if (typedFile) typedFile = 0; else dialog = QUIT;
      dirty = true;
      return;
  case K_BACKSPACE: typedFile = 0; dirty = true; return;
  case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT:
      if (cursorVisible) {
          if (k == K_UP && cursorR > 0) cursorR--;
          if (k == K_DOWN && cursorR < 7) cursorR++;
          if (k == K_LEFT && cursorC > 0) cursorC--;
          if (k == K_RIGHT && cursorC < 7) cursorC++;
      }
      cursorVisible = true;
      dirty = true;
      return;
  case K_ENTER: case K_SPACE:
      if (!cursorVisible) { cursorVisible = true; dirty = true; return; }
      if (human_to_move()) {
          if (flips_for(board, cursorR, cursorC, turn, nullptr)) play(cursorR, cursorC);
          else { notice = "Not a legal move"; noticeUntil = Platform::now_us() + 1500000; dirty = true; }
      }
      return;
  default: break;
  }
  if (ch == '+' || ch == '=') { press(B_LEVEL_UP); return; }
  if (ch == '-' || ch == '_') { press(B_LEVEL_DOWN); return; }
  // The keys of the first Reversi for Ember, kept: U undo, N new game,
  // L level, S hints, Q quit.  None is a board letter (a-h).
  if (lower == 'u') { press(B_UNDO); return; }
  if (lower == 'n') { press(B_NEW); return; }
  if (lower == 'l') { level = level >= 5 ? 1 : level + 1; dirty = true; return; }
  if (lower == 's') { press(B_HINTS); return; }
  if (lower == 'q') { dialog = QUIT; dirty = true; return; }
  // A square by name: a letter a-h, then a digit 1-8 plays there.
  if (lower >= 'a' && lower <= 'h') { typedFile = lower; cursorC = lower - 'a'; dirty = true; return; }
  if (ch >= '1' && ch <= '8') {
      cursorR = ch - '1';
      if (typedFile && human_to_move()) {
          int c = typedFile - 'a';
          typedFile = 0;
          if (flips_for(board, cursorR, c, turn, nullptr)) play(cursorR, c);
          else { notice = "Not a legal move"; noticeUntil = Platform::now_us() + 1500000; }
      }
      dirty = true;
  }
}


void ReversiApp::dialog_key(const Platform::Event& e) {
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
          else if (dialogRow == 1) humanColor = humanColor == BLACK ? WHITE : BLACK;
          else level = std::max(1, std::min(5, level + d));
      }
      dirty = true;
      return;
  case GAME_OVER:
      if (k == K_ENTER || k == K_F2) press(B_DLG_NEW_GAME);
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

// ---------------------------------------------------------------- drawing

void ReversiApp::draw_disc(double cx, double cy, double radius, int color, double squeeze, int alpha) {
  // A disc seen edge-on as it flips is an ellipse; drawn as stacked rows of
  // an antialiased circle scaled horizontally.
  bool white = color == WHITE;
  Rgb base = white ? WhiteDisc : BlackDisc;
  Rgb lit = white ? Rgb(255, 255, 255) : Rgb(88, 90, 100);
  Rgb rim = white ? Rgb(196, 192, 182) : Rgb(10, 10, 12);
  if (squeeze >= 0.98) {
      scene.circle(cx + radius * 0.06, cy + radius * 0.10, radius, Rgb(0, 0, 0), alpha * 70 / 255);
      scene.circle(cx, cy, radius, rim, alpha);
      scene.circle(cx, cy - radius * 0.03, radius * 0.92, base, alpha);
      scene.circle(cx - radius * 0.28, cy - radius * 0.32, radius * 0.38, lit, alpha * (white ? 120 : 70) / 255);
      return;
  }
  int top = int(cy - radius) - 1, bottom = int(cy + radius) + 2;
  double rx = std::max(0.5, radius * squeeze);
  for (int y = top; y < bottom; ++y) {
      double dy = (y + 0.5 - cy) / radius;
      if (dy <= -1 || dy >= 1) continue;
      double half = rx * std::sqrt(1 - dy * dy);
      int x0 = int(std::floor(cx - half)), x1 = int(std::ceil(cx + half));
      for (int x = x0; x <= x1; ++x) {
          double cover = std::min(1.0, std::min(x + 1 - (cx - half), (cx + half) - x));
          if (cover <= 0) continue;
          scene.blend(x, y, dy < -0.2 ? lit : base, int(alpha * std::min(1.0, cover)));
      }
  }
}


void ReversiApp::draw_board() {
  int pad = std::max(6, sq / 10);
  scene.shadow(bx - pad, by - pad, 8 * sq + 2 * pad, 8 * sq + 2 * pad, 12, 14, 110);
  scene.round_rect(bx - pad, by - pad, 8 * sq + 2 * pad, 8 * sq + 2 * pad, 12, Frame);
  scene.gradient(bx, by, 8 * sq, 8 * sq, Felt, FeltDark);
  for (int i = 0; i <= 8; ++i) {
      scene.fill(bx, by + i * sq - (i == 8), 8 * sq, 2, Grid);
      scene.fill(bx + i * sq - (i == 8), by, 2, 8 * sq, Grid);
  }
  for (int r = 2; r <= 6; r += 4)
      for (int c = 2; c <= 6; c += 4)
          scene.circle(bx + c * sq + 1, by + r * sq + 1, std::max(2, sq / 22), Grid);
  // coordinates on the frame
  for (int i = 0; i < 8; ++i) {
      scene.text_centered(font_coord, bx + i * sq + sq / 2, by - pad / 2 - 1, std::string(1, char('a' + i)), Rgb(200, 180, 150));
      scene.text_right(font_coord, bx - 3, by + i * sq + sq / 2 + 4, std::string(1, char('1' + i)), Rgb(200, 180, 150));
  }

  double radius = sq * 0.40;
  struct move mv[32];
  int n = (!over && !animating) ? legal_moves(board, turn, mv) : 0;

  for (int r = 0; r < 8; ++r)
      for (int c = 0; c < 8; ++c) {
          double cx = bx + c * sq + sq / 2.0 + 1, cy = by + r * sq + sq / 2.0 + 1;
          int now = board[r * 8 + c];
          if (animating) {
              int was = animBefore[r * 8 + c];
              if (r == animR && c == animC) {
                  // the new disc drops in
                  double t = std::min(1.0, animT * 2.2);
                  draw_disc(cx, cy, radius * (0.6 + 0.4 * t), now, 1.0, int(255 * t));
                  continue;
              }
              if (was != EMPTY && was != now) {
                  // a captured disc turns over: narrows, changes colour, widens
                  double t = std::min(1.0, std::max(0.0, (animT - 0.15) / 0.85));
                  double squeeze = std::fabs(std::cos(t * 3.14159265));
                  draw_disc(cx, cy, radius, t < 0.5 ? was : now, squeeze);
                  continue;
              }
          }
          if (now != EMPTY) {
              draw_disc(cx, cy, radius, now);
              if (r == lastR && c == lastC && !animating)
                  scene.circle(cx, cy, std::max(2.5, sq * 0.06), Bad);
          }
      }

  if (human_to_move() && dialog == NONE && !animating && !thinking) {
      int hr = -1, hc = -1;
      bool hover = !cursorVisible && square_at(mouseX, mouseY, hr, hc);
      for (int i = 0; i < n; ++i) {
          double cx = bx + mv[i].c * sq + sq / 2.0 + 1, cy = by + mv[i].r * sq + sq / 2.0 + 1;
          bool hot = (hover && hr == mv[i].r && hc == mv[i].c)
                  || (cursorVisible && cursorR == mv[i].r && cursorC == mv[i].c);
          if (hot)
              draw_disc(cx, cy, radius, turn, 1.0, 130);
          else if (hints)
              scene.circle(cx, cy, sq * 0.11, turn == BLACK ? Rgb(10, 20, 14) : Rgb(235, 245, 238), 110);
      }
  }
  if (cursorVisible && dialog == NONE && !over)
      scene.round_frame(bx + cursorC * sq + 3, by + cursorR * sq + 3, sq - 4, sq - 4, 6,
                        std::max(3, sq / 22), Accent, 230);
}


// Black's share of the discs, as a bar beside the board.
void ReversiApp::draw_score_bar() {
  int b, w;
  count_discs(board, &b, &w);
  int h = 8 * sq;
  scene.round_rect(barX, by, barW, h, barW / 2, WhiteDisc);
  int blackH = (b + w) ? h * b / (b + w) : h / 2;
  scene.round_rect(barX + 2, by + 2, barW - 4, std::max(0, blackH - 4), (barW - 4) / 2, BlackDisc);
  scene.fill_alpha(barX, by + h / 2, barW, 1, Accent, 170);
}


void ReversiApp::draw_player_card(int x, int y, int w, int h, int color) {
  bool toMove = !over && turn == color;
  scene.round_rect(x, y, w, h, 12, Panel);
  if (toMove) scene.round_frame(x, y, w, h, 12, 2, Accent, 220);
  draw_disc(x + 30, y + h / 2.0, 17, color);
  int b, wh;
  count_discs(board, &b, &wh);
  std::string name, sub;
  if (mode == TWO_PLAYERS) { name = color == BLACK ? "Black" : "White"; sub = "Player"; }
  else if (mode == WATCH || color != humanColor) {
      name = "Computer";
      sub = std::string("Level ") + std::to_string(level) + " " + MidDot + " " + Levels[level].name;
  } else { name = "You"; sub = color == BLACK ? "Black" : "White"; }
  scene.text(font_bold, x + 58, y + h / 2 - 3, name, Text);
  scene.text(font_small, x + 58, y + h / 2 + 14, sub, Dim);
  scene.text_right(font_title, x + w - 16, y + h / 2 + 10, std::to_string(color == BLACK ? b : wh), Text);
  if (toMove && thinking) {
      int dots = int((Platform::now_us() - thinkStart) / 250000) % 4;
      scene.text_right(font_small, x + w - 60, y + h / 2 + 6, std::string("thinking") + std::string(dots, '.'), Accent);
  }
}


void ReversiApp::draw_log(int x, int y, int w, int h) {
  scene.round_rect(x, y, w, h, 12, Panel);
  scene.text(font_bold, x + 14, y + 22, "Moves", Dim);
  int rowH = 23, top = y + 34;
  int rows = std::max(1, (h - 44) / rowH);
  int lines = int(log.size() + 1) / 2;
  int first = std::max(0, lines - rows);
  int colW = (w - 54) / 2;
  for (int line = first; line < lines; ++line) {
      int yy = top + (line - first) * rowH;
      scene.text_right(font_move, x + 44, yy + 16, std::to_string(line + 1) + ".", Faint);
      for (int k = 0; k < 2; ++k) {
          size_t idx = size_t(line * 2 + k);
          if (idx >= log.size()) continue;
          const LogEntry& e = log[idx];
          int tx = x + 54 + k * colW;
          bool latest = idx + 1 == log.size();
          if (latest) scene.round_rect(tx - 6, yy, colW - 6, rowH - 2, 6, PanelHi);
          draw_disc(tx + 6, yy + 11, 6, e.color);
          std::string s = e.pass ? "pass" : std::string(1, char('a' + e.c)) + char('1' + e.r);
          scene.text(latest ? font_moveb : font_move, tx + 18, yy + 16, s, latest ? Text : Rgb(208, 212, 222));
      }
  }
  if (log.empty()) scene.text(font_body, x + 14, top + 16, "No moves yet", Faint);
}


void ReversiApp::button(int x, int y, int w, int h, int id, const std::string& label,
                        const std::string& hint, bool primary, bool enabled) {
  bool hover = enabled && mouseX >= x && mouseY >= y && mouseX < x + w && mouseY < y + h
            && Platform::mouse_present();
  Rgb bg = primary ? (hover ? Rgb(248, 184, 98) : Accent) : (hover ? PanelLine : PanelHi);
  Rgb fg = primary ? AccentDark : (enabled ? Text : Faint);
  scene.round_rect(x, y, w, h, 9, bg);
  if (hint.empty())
      scene.text_centered(font_bold, x + w / 2, y + h / 2 + 5, label, fg);
  else {
      scene.text_centered(font_bold, x + w / 2, y + h / 2, label, fg);
      scene.text_centered(font_small, x + w / 2, y + h / 2 + 15, hint, primary ? AccentDark : Dim);
  }
  if (enabled) buttons.push_back({ x, y, w, h, id });
}


void ReversiApp::draw_panel() {
  int x = panelX, w = panelW;
  int pad = std::max(6, sq / 10);
  int y = by - pad, bottom = by + 8 * sq + pad;
  scene.text(font_title, x, y + 24, "Reversi", Text);
  scene.text(font_small, x + Canvas::text_width(font_title, "Reversi") + 10, y + 23, "Ember", Dim);

  int cardH = 64;
  draw_player_card(x, y + 40, w, cardH, WHITE);

  int sy = y + 40 + cardH + 10;
  std::string status, detail;
  Rgb color = Text;
  int b, wh;
  count_discs(board, &b, &wh);
  if (over) {
      status = b > wh ? "Black wins" : wh > b ? "White wins" : "A draw";
      detail = std::to_string(b) + " to " + std::to_string(wh);
      color = Accent;
  } else if (thinking) {
      status = "Computer is thinking";
      detail = std::string("Level ") + std::to_string(level) + " " + MidDot + " " + Levels[level].name;
  } else {
      if (mode == VS_COMPUTER) status = turn == humanColor ? "Your move" : "Computer's move";
      else status = turn == BLACK ? "Black to move" : "White to move";
      detail = std::to_string(moves_for(turn)) + " legal moves  " + MidDot + "  click a square, or type d3";
  }
  if (Platform::now_us() < noticeUntil) detail = notice;
  scene.text(font_large, x + 2, sy + 20, status, color);
  scene.text(font_small, x + 2, sy + 40, detail, Dim);

  int bh = 46, bgap = 8;
  int buttonsY = bottom - 2 * bh - bgap;
  int card2Y = buttonsY - 12 - cardH;
  int logY = sy + 54;
  draw_log(x, logY, w, card2Y - 10 - logY);
  draw_player_card(x, card2Y, w, cardH, BLACK);

  int bw = (w - 2 * bgap) / 3;
  button(x, buttonsY, bw, bh, B_NEW, "New game", "F2");
  button(x + bw + bgap, buttonsY, bw, bh, B_UNDO, "Undo", "F3", false, !history.empty());
  button(x + 2 * (bw + bgap), buttonsY, bw, bh, B_HINTS, hints ? "Hints on" : "Hints off", "F4");
  int y2 = buttonsY + bh + bgap;
  int lx = x + bw + bgap;
  button(x, y2, bw, bh, B_HELP, "Help", "F1");
  scene.round_rect(lx, y2, bw, bh, 9, PanelHi);
  scene.text_centered(font_bold, lx + bw / 2, y2 + bh / 2, "Level " + std::to_string(level), Text);
  scene.text_centered(font_small, lx + bw / 2, y2 + bh / 2 + 15, "\x83 / +", Dim);
  buttons.push_back({ lx, y2, bw / 2, bh, B_LEVEL_DOWN });
  buttons.push_back({ lx + bw / 2, y2, bw - bw / 2, bh, B_LEVEL_UP });
  scene.round_rect(x + 2 * (bw + bgap), y2, bw, bh, 9, Panel);
  scene.text_centered(font_small, x + 2 * (bw + bgap) + bw / 2, y2 + bh / 2 + 4,
                      mode == TWO_PLAYERS ? "Two players" : mode == WATCH ? "Watching" : "vs computer", Faint);
}


void ReversiApp::draw_dialog() {
  if (dialog == NONE) return;
  scene.fill_alpha(0, 0, scene.w, scene.h, Rgb(6, 8, 12), 150);
  int w = 460, h = 300;
  if (dialog == NEW_GAME) h = 340;
  if (dialog == HELP) { w = 560; h = 400; }
  if (dialog == QUIT) { w = 380; h = 170; }
  if (dialog == GAME_OVER) { w = 420; h = 250; }
  int x = (scene.w - w) / 2, y = (scene.h - h) / 2;
  scene.shadow(x, y, w, h, 16, 18, 150);
  scene.round_rect(x, y, w, h, 16, Panel);
  scene.round_frame(x, y, w, h, 16, 1, PanelLine);

  auto segment = [&](int row, int sx, int sy, int sw, const std::vector<std::string>& labels, int chosen, int firstId) {
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
      scene.text_right(font_small, x + w - 28, y + 44, "Arrows and Enter work too", Faint);
      int lx = x + 28, lw = w - 56;
      scene.text(font_small, lx, y + 82, "Opponent", Dim);
      segment(0, lx, y + 90, lw, { "Computer", "Two players", "Watch" }, int(mode), B_MODE0);
      scene.text(font_small, lx, y + 150, "You play", Dim, mode == VS_COMPUTER ? 255 : 110);
      segment(1, lx, y + 158, lw, { "Black (moves first)", "White" }, humanColor == BLACK ? 0 : 1, B_SIDE0);
      scene.text(font_small, lx, y + 218, "Computer's level", Dim);
      scene.round_rect(lx, y + 226, lw, 36, 8, PanelHi);
      buttons.push_back({ lx, y + 226, 60, 36, B_DLG_LVL_DOWN });
      buttons.push_back({ lx + lw - 60, y + 226, 60, 36, B_DLG_LVL_UP });
      scene.text(font_large, lx + 20, y + 250, "\x84", Dim);
      scene.text_right(font_large, lx + lw - 20, y + 250, "\x85", Dim);
      scene.text_centered(font_bold, lx + lw / 2, y + 249, std::to_string(level) + "  " + MidDot + "  " + Levels[level].name, Text);
      if (dialogRow == 2) scene.round_frame(lx - 4, y + 222, lw + 8, 44, 11, 2, Rgb(255, 255, 255), 150);
      button(x + w - 28 - 130, y + h - 58, 130, 40, B_DLG_START, "Start", "", true);
      button(x + w - 28 - 130 - 10 - 110, y + h - 58, 110, 40, B_DLG_CANCEL, "Cancel", "");
      break;
  }
  case GAME_OVER: {
      int b, wh;
      count_discs(board, &b, &wh);
      int winner = b > wh ? BLACK : wh > b ? WHITE : EMPTY;
      std::string title = winner == EMPTY ? "A draw" : winner == BLACK ? "Black wins" : "White wins";
      std::string who;
      if (mode == VS_COMPUTER && winner != EMPTY) who = winner == humanColor ? "You win!" : "The computer wins";
      scene.text_centered(font_huge, x + w / 2, y + 66, title, Text);
      if (!who.empty()) scene.text_centered(font_large, x + w / 2, y + 102, who, Accent);
      {
          int half = Canvas::text_width(font_title, std::to_string(b) + "  \x86  " + std::to_string(wh)) / 2;
          draw_disc(x + w / 2 - half - 26, y + 142, 16, BLACK);
          draw_disc(x + w / 2 + half + 26, y + 142, 16, WHITE);
      }
      scene.text_centered(font_title, x + w / 2, y + 152, std::to_string(b) + "  \x86  " + std::to_string(wh), Text);
      button(x + w / 2 + 6, y + h - 62, 150, 42, B_DLG_NEW_GAME, "New game", "", true);
      button(x + w / 2 - 6 - 150, y + h - 62, 150, 42, B_DLG_CLOSE, "Look at board", "");
      break;
  }
  case HELP: {
      scene.text(font_title, x + 28, y + 46, "How to play", Text);
      static const char* rows[][2] = {
        { "Goal", "have more discs than the other side at the end" },
        { "A move", "a disc that traps a line of the other colour" },
        { "Click a square", "or type it: d3 plays at d3" },
        { "Arrows, Enter", "move the cursor and play" },
        { "F2 or N", "new game: opponent, colour, level" },
        { "F3 or U", "undo (your move and the computer's reply)" },
        { "F4 or S", "show or hide the legal moves" },
        { "+ and \x83", "the computer's level, 1 to 5" },
        { "Esc or Q", "leave" },
      };
      int ry = y + 86;
      for (auto& r : rows) {
          scene.text(font_bold, x + 28, ry, r[0], Accent);
          scene.text(font_body, x + 170, ry, r[1], Text);
          ry += 28;
      }
      scene.text(font_small, x + 28, y + h - 42, "A side with no move passes. The game ends when neither can move.", Dim);
      button(x + w - 28 - 110, y + h - 66 - 30, 110, 40, B_DLG_CLOSE, "Close", "", true);
      break;
  }
  case QUIT: {
      scene.text(font_large, x + 28, y + 44, "Leave the game?", Text);
      scene.text(font_body, x + 28, y + 72, "Back to Ember.", Dim);
      button(x + w - 28 - 120, y + h - 62, 120, 40, B_DLG_QUIT, "Leave", "", true);
      button(x + w - 28 - 120 - 10 - 100, y + h - 62, 100, 40, B_DLG_STAY, "Stay", "");
      break;
  }
  default: break;
  }
}


static const char* Arrow[] = {
  "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
  "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
  "X.X X..X    ", "XX  X..X    ", "X    X..X   ", "     X..X   ", "      XX    ",
};


void ReversiApp::draw() {
  buttons.clear();
  scene.no_clip();
  scene.gradient(0, 0, scene.w, scene.h, BgTop, BgBottom);
  draw_score_bar();
  draw_board();
  draw_panel();
  if (dialog != NONE) { buttons.clear(); draw_dialog(); }
  dirty = false;
}


void ReversiApp::present_full() {
  memcpy(front.px, scene.px, size_t(scene.w) * scene.h * 4);
  if (Platform::mouse_present()) {
      for (int j = 0; j < 17; ++j)
          for (int i = 0; i < 12; ++i)
              if (Arrow[j][i] != ' ')
                  front.blend(mouseX + i, mouseY + j, Arrow[j][i] == 'X' ? Rgb(0, 0, 0) : Rgb(255, 255, 255), 255);
      drawnMouseX = mouseX; drawnMouseY = mouseY;
  }
  Platform::present(screen, 0, 0, screen.width, screen.height);
}


void ReversiApp::present_cursor() {
  int ox = drawnMouseX, oy = drawnMouseY;
  if (ox >= 0)
      for (int j = 0; j < 17; ++j)
          for (int i = 0; i < 12; ++i) {
              int xx = ox + i, yy = oy + j;
              if (xx >= 0 && yy >= 0 && xx < scene.w && yy < scene.h)
                  front.px[yy * scene.w + xx] = scene.px[yy * scene.w + xx];
          }
  for (int j = 0; j < 17; ++j)
      for (int i = 0; i < 12; ++i)
          if (Arrow[j][i] != ' ')
              front.blend(mouseX + i, mouseY + j, Arrow[j][i] == 'X' ? Rgb(0, 0, 0) : Rgb(255, 255, 255), 255);
  drawnMouseX = mouseX; drawnMouseY = mouseY;
  if (ox >= 0) Platform::present(screen, ox, oy, ox + 12, oy + 17);
  Platform::present(screen, mouseX, mouseY, mouseX + 12, mouseY + 17);
}


int ReversiApp::run() {
  if (!Platform::open_screen(screen)) {
      Platform::print("REVERSI needs a VESA graphics mode of at least 640x480 in 16, 24 or 32-bit colour.");
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
  srand(unsigned(Platform::now_us()));
  start_game();
  if (showDialog) dialog = NEW_GAME;

  uint64_t lastMove = 0;
  while (running) {
      Platform::Event e;
      bool got = false;
      while (Platform::next_event(e)) { handle(e); got = true; }

      if (exitWhenOver && overAt && Platform::now_us() - overAt > 1500000) break;

      if (computer_to_move()) {
          if (mode == WATCH && Platform::now_us() - lastMove < 300000) { Platform::idle(); continue; }
          computer_move();
          lastMove = Platform::now_us();
      }

      if (Platform::now_us() < noticeUntil + 100000) dirty = true;
      if (dirty) { draw(); present_full(); }
      else if (mouseX != drawnMouseX || mouseY != drawnMouseY) present_cursor();
      else if (!got) Platform::idle();
  }

  Platform::close_screen();
  Platform::print("Thanks for playing Reversi.");
  return 0;
}

} // namespace


int main(int, char**);

extern "C" void ember_run_constructors(void);

int main(int argc, char** argv) {
  ember_run_constructors();
  Platform::init();
  reversi_set_double_precision();
  ReversiApp app(Platform::command_line(argc, argv));
  return app.run();
}

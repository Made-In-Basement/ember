# Reversi: a browser game that became an N32

`REVERSI.N32` is Othello for Ember. It began as
[borkit/reversi](https://github.com/borkit/reversi), a browser game in one
`game.js` (checked against commit `87e6008`). Ember has no browser and no JavaScript,
so the game was split along the line that was already in the file: the
engine above `// ===== UI =====` was transcribed to C, and the DOM below it
was replaced with VGA mode 13h.

![Reversi, one move each in](screenshots/reversi.png)

## Playing it

Build it (below), then type `REVERSI` at the prompt.

| Argument | Effect |
|---|---|
| *(none)* | You play black against the computer at level 3 |
| `1` to `5` | Computer level; 1 is easy, 5 searches five plies |
| `2P` | Two players at one keyboard |
| `W` | You play white; the computer opens |
| `A` | The computer plays both sides; `Esc` between moves leaves |

| Key | Effect |
|---|---|
| Arrows, or `A`-`H` and `1`-`8` | Move the cursor. The letters and digits are the board's own notation, so `F` `5` goes to F5 |
| `Enter` or `Space` | Play at the cursor |
| `U` | Undo. Against the computer it also takes back the computer's reply, so it is your turn again |
| `N` | New game |
| `L` | Level, cycling 1-5 |
| `M` | Switch between vs-computer and two players; starts a new game |
| `S` | Show or hide the legal-move dots |
| `Esc` or `Q` | Back to the prompt |

The browser game uses `H` for hints. Here `A`-`H` already move the cursor, so
the commands use letters outside that range.

The screen is 320x200. On the left is the board: legal moves are gold dots,
the last move is ringed in blue, and the cursor is an orange square. On the
right is the panel: disc counts, whose move it is (or `thinking...`, or who
won), the mode, the level, whether hints are on, and the last five moves.
A side with no legal move passes automatically and the log shows `pass`.
The game ends when neither side can move.

## How it runs

The shell finds `REVERSI` by trying `.COM`, `.EXE`, `.BAT` and then `.N32`.
`pm32.asm` reads the 32-byte NX32 header, copies the image above 1 MB and
enters it in flat 32-bit protected mode. The header `start.S` writes for
this build is:

| Field | Value |
|---|---|
| load | `0x110000` |
| entry | `0x110020` |
| data end | `0x119B30` (39,728 bytes of file) |
| bss end | `0x12B630` (72,448 bytes of bss) |
| stack | 65,536 bytes |

Most of the bss is the 64,000-byte back buffer. The rest is 80 undo
snapshots (6,400 bytes) and the move log. Nothing is allocated from the heap.

At startup the program:

1. **Sets the x87 to 53-bit precision.** Scores are `double`, as in
   JavaScript. The x87 would otherwise keep 80-bit intermediates and round
   differently from the browser.
2. **Gets the BIOS 8x8 font.** It calls `INT 10h AX=1130h BH=03h` through
   `sys_bios()`, which runs the call in real mode and returns ES:BP. The
   panel text is drawn from that table.
3. **Switches to mode 13h** with `sys_set_video_mode(0x13)`, then programs 14
   DAC entries directly on ports `3C8h`/`3C9h`.

Every frame is drawn into the back buffer and copied to `0xA0000` in one
`memcpy`, so nothing flickers. Keys come from the runtime's IRQ 1 queue.
`sys_getkey()` halts until a key arrives, so the program uses no CPU while
it waits for you. The computer's move is computed inline: the board is
drawn with `thinking...` first, then the search runs and the reply is drawn.
On exit the program restores mode 03h and prints `Thanks for playing
Reversi.`

## The engine

`nano/reversi/engine.c` follows `game.js` function for function:

- **`flips_for`, `legal_moves`, `apply_move`**: the eight-direction scan,
  on a flat 64-cell board.
- **`evaluate`**: the same 8x8 positional weights (corners 120, the
  X-squares -40), mobility weighted early and disc difference weighted late,
  blended by the same endgame factor.
- **`minimax`**: alpha-beta, with moves sorted by positional weight so
  corners are searched first. A side with no move passes into the next ply.
  A finished game scores ±10,000 per disc.
- **`choose_ai_move`**: at level 1, a random pick from the four
  best-weighted moves (`srand(0x1234)`, so the same game each run). At levels
  2-5, a search that many plies deep.

`engine.c` and `engine.h` use nothing from Ember. With `-DREVERSI_NATIVE`
they compile against a host libc, which is how they were checked against
the original.

### Checked against game.js

A small harness drives both engines through the same 60 games. A shared
xorshift32 picks random moves, and on some turns asks the AI instead. At
every position it prints the board, each legal move with its flip count,
and the AI's choice at depth 3, plus the final score of each game.

The C and JavaScript outputs are **byte-identical: 3,598 positions and 60
results, 3,658 lines**. The harness needs `game.js`, so it lives outside
this tree:
[parity.c](https://github.com/borkit/ember-contrib/blob/main/reversi/parity.c) ·
[parity.mjs](https://github.com/borkit/ember-contrib/blob/main/reversi/parity.mjs).

## Building

```bash
python3 tools/build_reversi.py            # root/REVERSI.N32
python3 tools/build_reversi.py --image    # and build/ember.img with it
```

`tools/build_reversi.py` finds the tree from its own location and takes
`find_zig()` and `finish_image()` from `build_doom.py`, so it looks for Zig
the same way (`$ZIG`, then WinGet's install directory, then `PATH`). It:

1. Compiles `nano/start.S`, `sys.c` and `libc.c` with `engine.c` and
   `reversi.c` using `zig cc -target x86-freestanding -O2 -ffreestanding
   -fno-builtin -nostdinc -mno-sse -mno-sse2 -mno-mmx -fno-pic -fno-pie`,
   with `nano/include` and Zig's own resource headers on the include path.
2. Links with `nano/link.ld` (`start.o` first, so the header leads the image).
3. Runs `zig objcopy -O binary`, then `finish_image()`. objcopy drops the
   trailing zeros of `.data`, and the loader checks the file size against
   the header, so `finish_image()` pads the file back and verifies the size.
4. With `--image`, runs `build.py`, which copies everything in `root/` onto
   the disk.

`build.py` does not run this script, just as it does not run
`build_doom.py`, so the image still builds without Zig. `root/*.N32` is
already in `.gitignore`.

## Testing

Everything below ran on Ember `main` at `2c07764` in a container (Ubuntu
24.04, Zig 0.13.0, NASM 2.16.01, QEMU 8.2.2, Python 3.12). The container is
[ember-contrib/docker](https://github.com/borkit/ember-contrib/tree/main/docker).
Each case booted `build/ember.img` fresh with `tools/qemu_test.py`, typed
the keys, and saved a screenshot.

| Case | Keys | Result |
|---|---|---|
| Start | `reversi` | Opening position, black to move, four gold dots, level 3 |
| Play | `reversi`, `Enter` | Black D3, computer answers E3, 3-3 |
| Board notation | `reversi`, `F` `5` `Enter` | Black F5, computer answers F4 |
| Hints | `reversi`, `S` | Dots gone, panel shows `hints off` |
| Level | `reversi`, `L` `L` `L` | Level 3 → 4 → 5 → 1 |
| Undo | `reversi`, `Enter`, `U` | Both moves taken back, black to move, 2-2 |
| Two players | `reversi 2p`, `Enter` | Black D3, then *White to move* with no computer reply |
| Play white | `reversi w` | Computer opens D3 as black, white to move |
| Level 5 | `reversi 5`, `Enter` | Computer answers C3 within 10 s, even under emulation |
| Mode | `reversi`, `M` | New game, panel shows `two players` |
| Computer vs computer | `reversi a` | A full game to the end, a 32-32 draw, within 2 minutes |
| Esc | `reversi`, `Esc` | Text mode restored, `Thanks for playing Reversi.` |
| Q | `reversi`, `Q` | Same |
| Docs | `type readme.txt` | The REVERSI entry is listed under Programs |

The first build used `D` for level and `H` for hints. The same run showed
neither worked: the column keys `A`-`H` are handled before the commands, so
both keys only moved the cursor. They are now `L` and `S`.

It has not yet been run on real hardware. It needs a VGA-compatible BIOS
for mode 13h and the 8x8 font, which a BIOS or CSM boot provides.

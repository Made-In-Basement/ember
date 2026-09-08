# Ember

An operating system written from scratch: its own boot sector, FAT
filesystem, DOS-compatible interrupts, sound driver, 32-bit runtime and
graphical desktop. It boots from a USB stick on any PC that still offers a
legacy BIOS / CSM boot mode, runs real DOS programs, and plays music.

![desktop](docs/screenshots/ember-desktop.png)

| Boot splash | The crystal, broken open |
|---|---|
| ![splash](docs/screenshots/ember-splash.png) | ![menu](docs/screenshots/ember-menu.png) |

The desktop runs at 1280x1024 in true colour with anti-aliased type, drawn
by a 32-bit program of its own (`nano/gui/`). The start menu is an ember
crystal at the top of the screen: click it and it breaks in half, the
pieces slide apart, and the menu falls out of the gap.

Underneath is a 16-bit kernel in NASM assembly (`src/`), a DOS-compatible
`INT 21h`, and a 32-bit runtime (`nano/`) that loads flat protected-mode
programs above 1 MB and lends them the BIOS. Doom runs natively on it, with
sound.

Everything is here: the assembly kernel, the C programs, a Python build
script that assembles a bootable image, and a QEMU harness that types keys,
drives the mouse, captures audio and takes screenshots.

## Features

**Kernel / DOS side**
- 512-byte FAT boot sector that loads the kernel with INT 13h LBA reads
  (USB and hard disks) and falls back to CHS (floppies, old BIOSes).
- FAT12 and FAT16 driver: subdirectories, cluster chains, paths such as
  `\DOCS\FILE.TXT`, `..`, free-space accounting, and writing: files can be
  created, extended and deleted, with every change to the allocation table
  written through to both copies immediately. Volumes up to 2 GB (the FAT16
  limit), with the cluster size chosen to suit.
- Long file names, the way Windows put them on a FAT disk: the extra
  directory entries are read, checked against their short name and shown
  by `DIR`, the file manager and the graphical shell. Names can be typed
  too, in quotes when they contain spaces: `TYPE "Release Notes.txt"`,
  `CD "My Documents"`. DOS programs are unaffected: they still see the 8.3
  name (`RELEAS~1.TXT`), which is what they expect.
- Command shell: `DIR`, `CD`, `TYPE`, `COPY`, `DEL`, `REN`, `MKDIR`,
  `RMDIR`, `ECHO`, `CLS`,
  `COLOR`, `TIME`, `DATE`, `MEM`, `VER`, `BEEP`, `SPEAKER`, `PAUSE`, `REM`,
  `REBOOT`, `SHUTDOWN`
  (APM power-off), `WIN`, plus batch files (`AUTOEXEC.BAT` runs at boot).
  Tab completes file and directory names at the prompt: one match is filled
  in, several are extended as far as they agree and then listed.
- A real DOS program environment: `.COM` and `.EXE` (MZ, with relocations)
  loading, DOS-style memory blocks (MCBs) with allocate/resize/free, PSPs
  with environment blocks and command tails, nested `EXEC` (a program can
  run another and get its exit code), and the `INT 21h` calls programs use:
  console I/O, files (open/read/seek/close, FindFirst/FindNext, attributes,
  IOCTL), directories (make, remove, rename/move), date/time, vectors,
  version. `INT 21h` runs on its
  own kernel stack, as DOS does. Enough for DOS/4GW-based 32-bit programs:
  **Doom runs** (see below).
- Demo programs: `HELLO.COM` (prints its arguments) and `GUESS.COM` (a
  number guessing game); `IOBPTEST.COM` and `BEEPTEST.COM` probe the
  speaker bridge.
- The PC speaker, heard on machines that do not have one: `SPEAKER ON`
  arms the processor's hardware I/O breakpoints on the timer and speaker
  ports, so every note a program writes arrives as a debug trap. The bridge
  keeps a shadow of what the timer was told and refills the HD Audio ring
  buffer with the matching square wave, which the controller loops until
  the note changes. Nothing runs periodically and the timer is never read,
  so a game that uses it for its own timing is undisturbed. `BEEP`, `PLAY`
  notes and 1980s games such as Alley Cat are audible on a modern laptop.
  If the traps ever cost real processor time the bridge gives up watching
  the port being polled, and only then stands down. It is not started
  automatically: type it when you want it. `IOBPTEST` reports whether a
  machine supports the traps, and `SPEAKER` on its own reports what a game
  did and what it cost.
- Resident modules: the kernel fits in one 64 KB segment and that segment
  is full, so a driver that stays resident lives in a segment of its own.
  `LOAD name` reads `NAME.MOD` (from the current directory or the root)
  into a block of memory that belongs to the system, calls its init, and
  keeps it; `UNLOAD name` takes it out again, and `LOAD` alone lists what
  is loaded. A module is a flat binary with a 32-byte header naming its
  init, unload and event entries; the kernel tells every module when the
  shell is back at its prompt and when a program starts or ends, and
  hands each a table of services (printing, memory, the log) to far-call.
  The format is in `modules/ember.inc`. The first module is `XMS.MOD`
  (`modules/xms.asm`): extended memory for DOS programs, XMS 3.0 as
  HIMEM.SYS answers it, with the pool taken from the upper half of
  extended memory so it never meets the 32-bit programs loaded at 1 MB,
  and copies made through the firmware's block move. `XMSTEST.COM`
  checks it the way a program would. `AUTOEXEC.BAT` loads it at boot;
  hold Shift to boot without it.
- A two-panel file manager: `FM` gives two directory panels side by side.
  Tab switches, Enter opens a directory or runs a program, and the function
  keys copy (F5), rename or move (F6), make a directory (F7), delete (F8)
  and view a file (F3), with F1 for help. Enter on an `.MP3` or `.WAV`
  opens the music player, on a `.BAT` runs its lines, and on anything else
  opens the viewer. It is a plain `.COM` program (`programs/fm.asm`), so it
  can run other programs and get them back.
- A desktop background of your own: right-click the desktop for five drawn
  washes or a picture off the disk. JPEG and BMP are both read, so a
  photograph can be dropped on the stick with no conversion. JPEG is also
  the faster of the two here: reads go one sector at a time through the
  BIOS, so a 6 MB bitmap costs twelve thousand of them where the same
  picture as a JPEG costs a twelfth of that. The choice is kept in
  `EMBER.CFG`.
- A boot splash: `SPLASH picture.bin [music.wav]` shows a full-screen
  800x600 picture, plays a WAV under it, and holds it for a key or three
  seconds. `tools/mksplash.py image.jpg` makes the file from any picture
  (fitted and letterboxed, reduced to 256 colours, run-length encoded);
  `AUTOEXEC.BAT` shows `SPLASH.BIN` with the chime at every boot.
- A music player with a graphical interface: `PLAYER` (or `PLAYER dir`,
  `PLAYER file.mp3`) lists the MP3 and WAV files in a directory and plays
  them in turn, with a level meter, progress, pause, skipping and volume. It
  is a 32-bit program (`PLAYER.N32`) built from `nano/player/` with the
  public-domain minimp3 decoder; it draws in 640x480 through the card's
  linear framebuffer and uses the font in the BIOS ROM. A `MUSIC` folder on
  the image holds two test tones.
- Sound: `PLAY file.wav` streams PCM WAV files (8/16-bit, mono/stereo,
  8-48 kHz) through the Intel HD Audio controller with a driver written for
  real mode (PCI probe, unreal-mode MMIO, CORB/RIRB codec commands, DMA ring
  buffer); `PLAY C E G > C` plays note strings on the PC speaker. A start-up
  chime plays from `AUTOEXEC.BAT`.

**Graphical shell (`WIN`)**
- VESA 800x600x256 by default, with `WIN 1024` (1024x768), `WIN 640`
  (640x480) and `WIN LOW` (VGA 320x200); each mode falls back to the next
  smaller one if the card refuses it.
- Desktop icons, taskbar with Start button, task buttons and a clock.
- Start menu, overlapping draggable windows, close buttons, Win95 3D look.
- Mouse (own PS/2 driver) *and* full keyboard control:
  `Ctrl+Esc` Start menu, `Esc` close, `Tab` switch windows, `F1` About.
- Apps: **My Computer** (file browser; opens text files in Notepad, runs
  `.COM`/`.BAT` files and returns), **Notepad** (viewer with scrollbar),
  **Calculator**, **About**.

## Building

Requirements: Python 3 and NASM. A copy of NASM 3.02 for Windows is bundled
in `tools/nasm/`; on Linux/macOS install `nasm` from your package manager.

```bash
python build.py
```

This assembles the boot sector, kernel and programs and writes
`build/ember.img`: a 32 MB image with an MBR partition table and one FAT16
partition, ready for a USB stick (about 30 MB stay free for your own files).
`--size 512` (or `--size 2048`, the FAT16 maximum) makes it bigger, which is
worth doing if the stick has room: the image is written sparsely, so a 2 GB
image builds in a couple of seconds and only the used part is real data.
`--floppy` builds a 1.44 MB superfloppy image for floppy emulation instead.

## Running in QEMU

```bash
python build.py --run
```

boots the image as a hard disk, which is how a USB stick looks to the
BIOS. Any emulator works: the image is a plain raw disk image (VirtualBox
and VMware need it converted to their own formats).

## Booting a real PC from a USB stick

1. Write `build/ember.img` to the stick **as a raw image** (this
   erases the stick):
   - Windows: [Rufus](https://rufus.ie) → select the `.img`, mode
     "DD Image"; or balenaEtcher; or Win32DiskImager.
   - Linux/macOS: `sudo dd if=build/ember.img of=/dev/sdX bs=1M`
     (double-check the device name).
2. In the PC's firmware setup enable **Legacy Boot / CSM** (Compatibility
   Support Module) and disable **Secure Boot**. Ember is a BIOS-style OS:
   it does not have a UEFI loader, so a machine that offers *only* UEFI boot
   (some laptops made after ~2020) cannot start it.
3. Open the boot menu (usually `F12`, `F11`, `F8` or `Esc` during power-on)
   and pick the USB stick. Some firmwares list it twice; choose the entry
   *without* "UEFI" in front of it.

The stick stays a normal FAT volume: plug it back into Windows and copy your
own `.COM` programs or text files onto it. The kernel lives in reserved
sectors that the filesystem never touches.

What you will see: the boot sector prints a dot per kernel sector, the
kernel greets you, `AUTOEXEC.BAT` runs and starts the graphical shell.
**Hold Shift while it says "Starting Ember"** to stay at the DOS prompt
instead (type `WIN` later; `WIN 1024`, `WIN 640` or `WIN LOW` pick another
resolution if the default 800x600 misbehaves on your graphics card or
monitor). "Exit to DOS" in the Start menu returns to the prompt;
"Shut Down" powers the machine off through APM.

The mouse is driven directly through the PS/2 controller (IRQ12), which
covers real PS/2 mice and touchpads and USB mice under the BIOS's "USB legacy
support". Without one the shell is fully usable from the keyboard; `WIN
NOMOUSE` skips the mouse driver entirely, and the About window reports
whether a mouse was found.

**Tested on real hardware:** a Lenovo Yoga laptop (Legacy Support enabled
in the BIOS) boots the USB image, runs the graphical shell at 800x600, and
uses a USB wireless mouse through the BIOS-assisted path. Its built-in
touchpad is an I2C device and is not reachable from a BIOS-level OS.

**Sound on real hardware:** the HD Audio driver probes every controller on
the PCI bus (laptops usually have two: HDMI audio on the graphics chip and
the chipset one that drives the speakers), picks the codec's speaker (or
headphone / line-out) pin, follows its connection list to a DAC, and
upsamples in software when the codec rejects the file's sample rate. Codecs
differ between machines; `SOUND` shows what was found and `SOUND DEBUG`
prints every command and response of the probe.

**The boot log:** the image carries a 16 KB `EMBER.LOG` that the kernel
overwrites in place with a log of the boot (video mode, mouse detection,
the complete sound-driver trace, playback details). It is written after
every `SOUND`/`PLAY` command and when the GUI exits, so after a boot on a
real PC you can plug the stick back into Windows and read exactly what the
hardware said. `tools/readfile.py build/ember.img EMBER.LOG` reads it
out of an image after a QEMU run; `tools/qemu_test.py --audio out.wav`
captures the emulated audio and `tools/analyze_wav.py` summarises it.

## Running DOS programs, including Doom

![doom](docs/screenshots/doom.png)

Copy DOS programs onto the stick and type their name; `.COM`, `.EXE`,
`.BAT` and `.N32` are found in that order.

Most real-mode DOS software runs: `.COM` files, `MZ` executables with
relocations, text mode and VGA graphics, command-line arguments, and
programs that launch other programs. Doom's own `SETUP.EXE` and the
`DOOM.EXE` bound with the DOS/4GW extender both work. What is missing is
mostly about memory: there is no EMS and no DPMI host, so DJGPP-built
programs and anything that asks for a DOS extender of its own will refuse
to start. Extended memory is there once `LOAD XMS` has run (which
`AUTOEXEC.BAT` does), the way HIMEM.SYS provides it. There is no Sound Blaster, no mouse driver interface
and no networking, but a game that makes sound the 1980s way, through the
PC speaker, is audible: see `SPEAKER` above. That bridge owns the
real-time clock interrupt and the debug registers while it runs, so hand
them back with `SPEAKER OFF` before a program that wants those itself. Programs can create, write and delete files, but not
append to one or rename it, and a directory cannot be created. The shareware Doom (`DOOM.EXE`, `DOOM1.WAD`)
lives in `root/DOOM/`; the image ships with a `DOOM.BAT` at the root, so
typing `DOOM` at the prompt starts the game and returns to the prompt when
you quit (F10, then Y). Retail Doom / Doom II work the same way: put their
`DOOM.EXE`/`DOOM2.EXE` and `.WAD` in a folder on the stick.

The original `DOOM.EXE` runs too (type `DOOM.EXE`), but it expects a Sound
Blaster, which nothing in a real-mode OS can emulate, so it only has
PC-speaker effects (`DEFAULT.CFG` sets `snd_sfxdevice 1`). That is why
`DOOM` now starts **NDOOM**, a native port with real sound.

### NDOOM: Doom compiled for Ember

![native doom](docs/screenshots/doom_native.png)

`root/DOOM/NDOOM.N32` is id Software's GPL Doom source (linuxdoom-1.10)
built for Ember's 32-bit program format. Its platform layer
(`nano/doom/`) draws through VGA mode 13h, reads the keyboard from the
IRQ 1 handler, times the game with a 140 Hz timer interrupt, and mixes
sound effects itself into a 44.1 kHz stereo stream on the HD Audio driver,
so the effects come out of the laptop speakers. Music is not there yet (it
needs an OPL synthesizer). Saved games work, and the settings you change
in the menus are kept in `NDOOM.CFG`. `-warp 1 1`
starts a level directly; `-skill 1..5` picks the difficulty. Game time
comes from the CPU cycle counter and the keyboard is polled as well as
interrupt-driven, so the port does not depend on interrupt delivery
quirks of a particular PC. NDOOM writes a few diagnostic lines (CPU
clock, timer interrupt rate, sound stream) into `EMBER.LOG`.

The 32-bit runtime (`src/pm32.asm`) loads an `.N32` file above 1 MB,
switches to protected mode and hands the program a flat 4 GB address
space. Timer and keyboard interrupts are delivered to handlers the program
registers, and `INT 80h` asks the kernel to run any real-mode interrupt
(BIOS or DOS) with a given register set, which is how the C library in
`nano/` does file I/O, console output and video-mode changes. Building
needs [Zig](https://ziglang.org) for its bundled clang and linker:
`winget install zig.zig`, then `python tools/build_doom.py` (`--player`
builds the music player, `--hello` a runtime self-test). `build.py` picks
the results up from `root/`.

Programs get about 522 KB of conventional memory, plus all extended memory
for 32-bit programs, which see everything above 1 MB directly. The sound
ring and the graphical shell's scratch are borrowed from that pool only
while they are in use, so `SPEAKER ON` costs a program 64 KB and running
something from inside the graphical shell costs it 48 KB. A game that
complains about memory will usually be happy if it is started from a plain
prompt with `SPEAKER OFF`.

`EXETEST.EXE` on the image is a small self-test of the DOS services (run
`EXETEST a b`). `TRACE` toggles logging of every `INT 21h` call into
`EMBER.LOG`, useful when a program misbehaves.

## Using the command line

```
C:\>DIR
C:\>CD DOCS
C:\DOCS>TYPE COMMANDS.TXT
C:\DOCS>CD ..
C:\>HELLO these are arguments
C:\>GUESS
C:\>WIN
```

`HELP` lists everything. Batch files support `@`, `ECHO OFF`, `REM`,
`PAUSE` and labels. A program is run by typing its name; `.COM` is assumed,
then `.BAT`.

### Writing programs for it

Assemble with NASM (`nasm -f bin -o MYPROG.COM myprog.asm`), `[ORG 0x100]`,
and use the classic DOS calls. Supported `INT 21h` functions:

| AH | Function |
|---|---|
| `01h` `07h` `08h` | read a key (with / without echo) |
| `02h` `06h` `09h` `40h` | write a character / direct console I/O / `$`-string / handle write (stdout, stderr) |
| `0Ah` `3Fh` (handle 0) | buffered line input |
| `0Bh` `0Ch` | keyboard status / flush |
| `0Eh` `19h` | select drive / current drive |
| `25h` `35h` | set / get interrupt vector |
| `2Ah` `2Ch` | date / time |
| `30h` | version (reports 5.0 for compatibility) |
| `3Dh` `3Eh` `3Fh` `42h` | open / close / read / seek |
| `3Ch` `40h` `41h` `68h` | create / write / delete / commit |
| `48h` `49h` `4Ah` | memory functions (allocation always fails, resize succeeds) |
| `4Ch` `00h` | exit; also `INT 20h` |
| `62h` | get PSP segment |

Programs load at `2000:0100` with a PSP at `2000:0000`; the command tail is
at `PSP:80h` as usual.

## How it works

```
USB image:      sector 0 = MBR with one active partition (src/mbr.asm), which
                loads the partition's boot sector; the FAT volume starts at
                sector 2048.  Floppy image: the FAT volume starts at sector 0.
volume sector 0 boot sector: BPB + loader (src/boot.asm)
sectors 1..44   kernel, loaded to 0800:0000  (src/kernel.asm + modules)
                FAT #1, FAT #2, root directory, data clusters (normal FAT volume)
```

| Module | Role |
|---|---|
| `src/mbr.asm` | master boot record for the USB image: finds the active partition and chain-loads its boot sector |
| `src/boot.asm` | BPB, INT 13h extension detection, LBA/CHS sector reads with retries, kernel load |
| `src/console.asm` | BIOS teletype output, keyboard, line editor, number printing, tick delays |
| `src/disk.asm` | sector reads (LBA with CHS fallback), multi-sector reads across 64 KB boundaries |
| `src/fat.asm` | BPB parsing, FAT12/16 cluster chains with a 2-sector FAT cache, directory iteration, path resolution |
| `src/dos.asm` | processes: `.COM`/`.EXE` loading, PSP and environment, EXEC, `INT 21h`/`2Fh` services, FindFirst/Next, file handles |
| `src/shell.asm` | command parser, built-in commands, batch interpreter |
| `src/mem.asm` | conventional memory: MCB arena for programs, service buffers placed at the top |
| `src/log.asm` | in-memory boot log, written over the pre-allocated `EMBER.LOG` |
| `src/sound.asm` | PC speaker note player; HD Audio driver (PCI scan, unreal mode, codec path discovery, DMA streaming) and the WAV player |
| `src/gfx.asm` | VBE/VGA mode setup, banked framebuffer primitives, BIOS font text, palette |
| `src/mouse.asm` | PS/2 mouse driver on the 8042 controller (IRQ12), with timeouts so it degrades to keyboard-only |
| `src/icons.asm` | icons and the mouse cursor as ASCII art |
| `src/gui.asm` | desktop, taskbar, Start menu, window manager, event loop |
| `src/apps.asm` | File Manager, Notepad, Calculator, About |

Memory map: `0x08000` kernel (code, data, bss, stack), `0x18000` batch-file
buffer, `0x20000` program segment (64 KB), `0x30000` GUI scratch (directory
listings, Notepad text, line table), `0xA0000` video memory.

The graphics code draws through the 64 KB VGA window at `A000:0000` and
switches VESA banks as needed, so the same routines serve every 8-bit VESA
mode (640x480, 800x600, 1024x768) and plain VGA mode 13h; the default
window layout is designed for 640x480 and stretched to the real screen.
Text uses the BIOS ROM font (8x16 or 8x8).
Window moves show an XOR outline while dragging, as Windows 95 did.

## Testing without hardware

`tools/qemu_test.py` boots the image in headless QEMU, types keys, moves
and clicks the mouse, reads the text screen straight out of video memory and
saves PNG screenshots:

```bash
python tools/qemu_test.py --keys "dir{ret}type readme.txt{ret}"
python tools/qemu_test.py --usb --wait 6 --png shot.png
python tools/qemu_test.py --wait 5 "--mouse=-270,-210,dblclick" --keys2 "{down}{ret}" --png notepad.png
```

The image is attached as an IDE disk by default, `--usb` makes it a USB
mass-storage device (SeaBIOS boots it just like a real stick), `--floppy`
boots a floppy image. `--shift` holds Shift during boot to skip
`AUTOEXEC.BAT`, `--audio file.wav` captures the sound output.

## Limitations and ideas

- Files can be created, written and deleted, but not appended to or
  renamed, and there is no way to make a directory. A file's directory
  entry is rewritten as it grows, so pulling the stick out mid-write
  costs at most the file being written.
- Writing is exercised on FAT16, the format the USB image uses. The FAT12
  side of the driver is written but has not been tested on hardware.
- One program at a time (plus what it EXECs); no TSRs (drivers that stay
  resident are modules, see Features), no EMS, no DPMI host of its own
  (extenders such as DOS/4GW bring their own).
- Everything goes through the BIOS: no protected mode, no real drivers.
  That is what makes it boot on almost anything with a CSM.
- Long file names are ignored (8.3 names only, as in DOS).
- Nice next steps: FAT writing, a text editor in Notepad, `.EXE` loading,
  a Minesweeper clone, window minimise/resize, a UEFI loader.

## Layout

```
build.py            assembles everything, builds the images (--run boots QEMU)
src/                the operating system
programs/           .COM demo programs (assembled into the image root)
modules/            resident modules (XMS.MOD), assembled into the image root
root/               files copied into the image (README.TXT, AUTOEXEC.BAT, DOCS\)
tools/nasm/         NASM 3.02 for Windows (official build)
tools/qemu_test.py  headless QEMU test harness
docs/screenshots/   pictures taken with the harness
build/              output: boot.bin, kernel.bin, *.COM, ember.img, ember-usb.img
```

Welcome to Ember!

Ember is a tiny DOS-like operating system written from scratch in
x86 assembly language.  It boots from a floppy image or a USB stick on
any PC with a legacy (BIOS / CSM) boot mode, reads files from the FAT12
or FAT16 volume it booted from, and has a Windows-style graphical shell.

The graphical shell (WIN)
-------------------------
  Double-click an icon or use the Start menu to open a program:
    My Computer   browse the disk; Enter opens, Backspace goes up,
                  .COM files run, .TXT files open in Notepad
    Notepad       view text files (arrows / PgUp / PgDn to scroll)
    Calculator    click the buttons or type
    About         a few words about the system
    DOS Prompt    back to the command line
  Keys: Ctrl+Esc = Start menu, Esc = close window, Tab = next window,
        F1 = About.  Windows can be dragged by their title bars.
  WIN uses 800x600.  WIN 1024, WIN 640 and WIN LOW (320x200) pick other
  resolutions if your graphics card or monitor prefers them.
  WIN NOMOUSE skips the PS/2 mouse driver (keyboard only).  The About
  window shows whether a mouse was found.

The command line
----------------
  HELP          list of commands
  DIR           see the files on this disk
  TYPE file     show a text file (try: TYPE DOCS\COMMANDS.TXT)
  CD DOCS       change directory, CD .. goes back up
  HELLO         run HELLO.COM
  GUESS         play a number guessing game
  WIN           start the graphical shell

Sound
-----
  PLAY CHIMES.WAV plays a WAV file through the HD Audio chip (drop your
  own PCM WAV files on the disk), PLAY C E G > C plays notes on the PC
  speaker, and SOUND reports what the sound driver found.

Programs
--------
DOS programs (.COM and .EXE) can be copied onto the disk and run by
typing their name.  Type DOOM to play Doom (F10 then Y quits).  Batch
files (.BAT) work too: AUTOEXEC.BAT runs at every boot and starts the
graphical shell.  Hold Shift while booting to skip it and stay at the
DOS prompt.  EXETEST runs a self-test of the DOS services.

  CHESS         play chess against Stockfish 11 (built by
                tools/build_chess.py).  The new game screen picks the
                opponent, your colour and the level (1-8).  CHESS WHITE,
                CHESS BLACK 5, CHESS 2P and CHESS WATCH skip it.
  Keys: drag or click pieces, or type a move like e2e4.  F1 help,
        F2 new game, F3 undo, F4 flip the board, F5 hint, + and -
        change the level, Esc leaves.  Games are saved to CHESS.PGN.

Have fun exploring!

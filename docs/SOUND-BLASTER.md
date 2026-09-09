# A Sound Blaster that is not there: where it stands

This laptop has no Sound Blaster and no OPL chip. It has Intel HD Audio, and
now two ways of convincing a program that the card it was written for is
present: a DPMI host for programs that go into protected mode themselves,
and a virtual-8086 monitor for the ones that do not - which is nearly all of
them.

## The fault is the whole trick

The task state segment carries a permission map saying which ports a program
may touch. Every bit is clear - the program drives the timer, the keyboard
and the display itself - except the sixteen a Sound Blaster answers to, the
two the synthesiser answers to, and channel 1 of the transfer controller.
Those raise a general protection fault instead.

That fault is the mechanism, and the reason this could not be built on the
debug registers the speaker bridge uses. **A fault arrives before the
instruction runs**, so a read can be given a value that no chip supplied; a
debug trap arrives after, when the processor has already latched whatever an
empty port returned. Detection needs a read to come back `AAh`, so the
difference decides whether the thing is possible at all.

`io_decode.inc` reads the faulting instruction. The eight I/O opcodes share
one shape - `E4`-`E7` and `EC`-`EF`, where bit 3 says the port is in DX
rather than an immediate byte, bit 1 says it is an OUT and bit 0 says the
operand is wide - so the decoder is shorter than the prefix loop in front of
it. `dpmi_io.inc` is the card itself. Both are shared by the two hosts; the
only thing that differs between them is how wide a word is.

## The monitor (`SB.MOD`, `modules/sb.asm`)

A program that never heard of protected mode cannot be put under a
permission map by asking it to. So it is put in virtual-8086 mode instead.
Nothing it can see changes: a megabyte of memory addressed as segment and
offset, the interrupt vector table at zero, DOS through INT 21h. But it is
now running at ring 3 under a task state segment, and the same fault works.

No paging, no memory remapped, no extender, no descriptors for the program
to know about. It is the smallest thing that turns a fault into a sound
card.

**What runs in virtual-8086 mode is not only the game.** The kernel's own
INT 21h, the BIOS, every interrupt handler, everything from the moment a
program is about to start until it has ended. That is safe because none of
it needs what virtual-8086 mode takes away: the kernel reaches above a
megabyte through a 4 GB GS only in the sound driver, and the sound driver
does not run while a program does. A 32-bit program of ours is the
exception - it runs the processor itself - so the kernel says so
(`MOD_EV_NATIVE`, fired in `exec_program` where the file turns out to be
NX32) and the monitor stands down before it starts.

The two ends:

    v86_enter   from real mode; comes back at the caller's next instruction,
                in virtual-8086 mode, with everything else unchanged.
    v86_leave   the same journey the other way, asked for by writing to a
                port no machine has ever had (2FFh).

Both are transparent mode changes, so they can sit inside the module event
the kernel already fires around a program.

### Why the privilege level is nought

The program runs with IOPL 0, so CLI, STI, PUSHF, POPF, INT and IRET all
fault into the monitor rather than execute, and the monitor does them on the
program's behalf. That costs a fault per DOS call and per interrupt, which
on this processor is nothing.

What it buys is that **nothing the program does can arrive on the vectors
the interrupt controller has been moved to**. The controller is remapped to
20h-2Fh for as long as the monitor is up, so a hardware interrupt is never
confused with an exception - the ambiguity the DPMI host has to resolve by
asking the controller what is in service, every time. With IOPL 3 an `INT
21h` from the program would land on the same vector as IRQ 1 and there
would be no way to tell them apart.

The processor's own virtual-8086 extensions (CR4.VME and the interrupt
redirection bitmap in the TSS) would make every INT free of charge. QEMU's
TCG does not implement them - `CPUID.1:EDX` bit 1 is clear even with
`-cpu max`, which `CPUIDT.COM` will show you - and being able to test on
the desk is worth more than the microseconds.

## What works

- **Detection.** The reset handshake answers `AAh`, the version reports 4.5,
  and the synthesiser's timer status reads `C0h`. `SBREAL.COM` runs the
  sequence a game's setup runs and is told there is a card. `BLASTER=A220 I5
  D1 T3` appears in a program's environment, but only while `SB.MOD` is
  loaded: a game reads that and believes it.
- **Programming.** The transfer controller is shadowed alongside the card,
  because a Sound Blaster does not fetch its own samples - the game programs
  the 8237 and the card is only told to start. A buffer address arrives as
  five separate writes: two halves of an address sequenced by a flip-flop
  another port resets, plus a page register whose channels are in no sensible
  order. Only channel 1's own registers are watched; the three every channel
  shares are watched and then let through to the real controller, so a
  floppy can still reach memory.
- **Sound.** The host holds the HD Audio stream for as long as a program
  runs, and on each timer interrupt walks the game's buffer at the game's own
  rate in 16.16 fixed point, laying each byte into the 44.1 kHz stereo ring
  as many times as it needs.
- **The card's own interrupt.** A game that starts a transfer waits to be
  told the block has been played. Nothing raises that - no chip is on the
  line - so the monitor raises it itself, nested inside the timer interrupt
  whose tick noticed: the game enters its handler for the card first and its
  timer handler after, exactly the order a real one would have arrived in.
  The controller is virtualised just enough for that - the program's mask is
  remembered and its line kept masked in the hardware, and the
  end-of-interrupt it sends for an interrupt no chip raised is swallowed
  rather than clearing whatever really is in service.
- **The speaker.** Timer channel 2 and the gate in port 61h are watched the
  same way, and the square wave they describe is mixed into the same stream.
  That replaces the debug-register bridge in `spkbrdg.asm` for as long as the
  monitor is up, and is better than it in every way: the fault arrives before
  the instruction rather than after, the processor's four breakpoints stay
  free, and there is no storm guard to stand down. Alley Cat's sound comes
  out of the codec.
- **Real programs.** Alley Cat runs under the monitor, with sound. The
  desktop still runs. `DPMITEST` still passes every check and `SBTEST` still
  plays its tone through the DPMI host.
- **The music.** `nano/opl/opl3.c` is a complete OPL3, written for Doom and
  already known to play a tune. It is compiled freestanding by
  `tools/build_opl.py`, linked flat at offset `6000h` (`nano/opl/opl.ld`), and
  placed at that same offset inside `SB.MOD` by NASM. Every address in it is
  therefore an offset in the module - and the monitor's own selectors are
  based on the module, so the whole thing is right wherever the kernel puts
  it. No fixed physical address, no relocation, no second module to find.
  That is why the monitor's stack segment is `SEL_DATA32` rather than the
  flat selector: C wants SS and DS based the same way, and everything the
  monitor addresses by name is an offset in the module. `GS` stays flat, for
  the program's memory and the audio ring. Register writes go to the chip as
  well as the shadow, and `io_pump` renders a frame at a time and mixes it
  in. `ADLIB.COM` plays four notes through it.
- **Standing aside.** A program that loads a descriptor table or a control
  register means to run the processor itself, and nothing in virtual-8086
  mode can stand in for that. The monitor leaves instead - back to real mode,
  resuming *at* the instruction rather than after it - and the program has
  the machine. It gets no card that way, but it runs. That makes the module
  safe to load on a kernel that has never heard of `MOD_EV_NATIVE`, which is
  how it can be tried on a machine without rewriting its disk.

## How it is tested

    python build.py --size 48 --out build/sbv86.img
    python tools/qemu_test.py --img build/sbv86.img --mem 24 \
        --keys "load sb{ret}" --keys2 "sbreal{ret}" \
        --wait 25 --after 15 --audio build/sb.wav --png build/sb.png

`SBREAL.COM` is an ordinary `.COM` file doing what a game's setup does and
then what its sound code does: find the card, program the controller, tell
the DSP to play, and wait to be told the block has been played. It prints
how many times the card interrupted it, which is the number worth reading -
a game that never gets that hangs. `SBREAL /Q` stops after the handshake,
which is how the two halves are told apart when something goes wrong.

QEMU only attaches an HD Audio device when `--audio` is given, so without it
the host finds no stream and the card stays silent. The capture's header is
written on close and QEMU is killed, so parse from byte 44 rather than with
a WAV reader.

The card counts itself - writes, reads, pump calls, frames laid down, blocks
played, interrupts raised - and reports at the program's exit, to the screen
and the log. Every bug in the audio path so far was found that way in one
run each. Make the thing say what it did.

## Three bugs worth remembering

**The big bit survives.** Leaving protected mode reloaded DS, ES, FS and GS
but not SS. A segment register keeps the descriptor it was last loaded with
until it is loaded again, and real mode does not change that - it only
rewrites the base. The flat selector is a *big* segment, so every PUSH, POP,
CALL and RET after the switch used ESP rather than SP, with whatever the
ring-0 stack had left in the high half. Memory operands still computed the
right addresses, which is why a dump of the stack printed exactly the right
values one instruction before a RET went to zero. The symptom was a machine
that rebooted the moment a program ended.

**Nine bits, not eight.** `EFL_PROGRAM` is the mask of flags a program is
allowed to set for itself, applied on every emulated `POPF` and `IRET`. It
was written `0DD5h`. The right answer is `0FD5h`: bit 9 is the interrupt
flag, and without it every `POPF` and every `IRET` switched interrupts off
again. So the first interrupt a program took was the last one it ever got.
The symptom was a program that ran, printed, made sound - and then waited
forever for a clock that had stopped, which looks nothing like a
one-hex-digit constant. What found it was making the monitor keep a ring of
the last thirty-two things that touched the flag and reading it out of
memory with `--pmemsave` while the machine sat there hung.

**A task is never marked free.** The processor marks a task busy when it is
loaded and leaves it that way, because a task is normally left by a task
switch and this one is left by walking out of protected mode. So the second
program to run met a busy task and `LTR` refused it with `#GP(0030)`. The
monitor clears the busy bit itself before every `LTR`.

## What is next

1. **How loud the music is.** The synthesiser's output peaks around 4000
   where the digital side reaches 16384, so a game's music sits well under
   its sound effects. Whether that wants a gain here or is right as it
   stands is a question for a real game on real speakers.
2. **The BIOS in virtual-8086 mode.** Under QEMU the BIOS's disk services
   work from inside V86 (`DIR` and loading a program both work). Some
   firmware does things in its INT 13h that V86 will not allow, especially
   for USB. If that bites on the laptop, the answer is a real-mode excursion
   around that one interrupt - out of V86, run it, back in - which is what
   the DPMI host already does for everything.
3. **DOS/4GW.** Still stops after its banner (see `DPMI-STATUS.md`). That is
   the protected-mode path and it is now the less important of the two.

## What is ruled out

- Nothing, any more. Real-mode games were the thing this could not reach,
  and the monitor is how it reaches them.

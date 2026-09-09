# A Sound Blaster that is not there: where it stands

This laptop has no Sound Blaster and no OPL chip. It has Intel HD Audio, and
a DPMI host that runs a DOS program at ring 3. Between those two facts there
is room to convince a game that the card it was written for is present.

## How it works

The task state segment carries a permission map saying which ports a client
may touch. Every bit is clear — the client drives the timer, the keyboard
and the display itself — except the sixteen a Sound Blaster answers to, the
two the synthesiser answers to, and the transfer controller's registers.
Those raise a general protection fault instead.

That fault is the whole mechanism, and the reason this could not be built on
the debug registers the speaker bridge uses. **A fault arrives before the
instruction runs**, so a read can be given a value that no chip supplied; a
debug trap arrives after, when the processor has already latched whatever an
empty port returned. Detection needs a read to come back `AAh`, so the
difference decides whether the thing is possible at all.

`io_emulate` in `modules/dpmi_pm.inc` decodes the faulting instruction. The
eight I/O opcodes share one shape — `E4`-`E7` and `EC`-`EF`, where bit 3 says
the port is in DX rather than an immediate byte, bit 1 says it is an OUT and
bit 0 says the operand is wide — so the decoder is shorter than the prefix
loop in front of it. `modules/dpmi_io.inc` is the card itself.

## What works

- **Detection.** The reset handshake answers `AAh`, the version reports 4.5,
  and the synthesiser's timer status reads `C0h`. `SBTEST.COM` runs the
  sequence a game's setup program runs and is told there is a card.
- **Programming.** The transfer controller is shadowed alongside the card,
  because a Sound Blaster does not fetch its own samples — the game programs
  the 8237 and the card is only told to start. A buffer address arrives as
  five separate writes: two halves of an address sequenced by a flip-flop
  another port resets, plus a page register whose channels are in no sensible
  order. The host reads back the same address it was given.
- **Sound.** The host holds the HD Audio stream for as long as a client runs,
  and on each timer interrupt walks the game's buffer at the game's own rate
  in 16.16 fixed point, laying each byte into the 44.1 kHz stereo ring as
  many times as it needs. A square wave written at about 275 Hz comes back
  out of QEMU's capture at 279.

The digital path needs no synthesiser: it is a copy and some arithmetic, so
it lives in the host beside the trap. That is why it did not have to wait on
the question of where C code can live resident.

## How it is tested

    python build.py --size 48 --out build/sbtest.img
    python tools/qemu_test.py --img build/sbtest.img --mem 24 \
        --keys "load dpmi{ret}sbtest{ret}" --wait 20 --after 20 \
        --audio build/sb.wav --png build/sb.png

QEMU only attaches an HD Audio device when `--audio` is given, so without it
the host finds no stream and the card stays silent. The capture's header is
written on close and QEMU is killed, so parse from byte 44 rather than with
a WAV reader.

The card counts itself — writes, reads, pump calls, frames laid down — and
reports at client exit, to the screen and the log. Both bugs in the audio
path were found that way in one run each, after four runs of guessing at a
similar problem earlier the same day. Make the thing say what it did.

## What is next

1. **The completion interrupt.** A game that starts a transfer waits to be
   told the buffer has been played, and hangs without it. The host would
   deliver IRQ 5 to the client's own handler. `deliver_int` ends with a jump
   to `int_exit` and takes its vector from the frame, so injecting a second
   interrupt inside the timer's own delivery means restructuring it to nest —
   the client should enter the card's handler and, on its IRET, still reach
   the timer's. Worth doing carefully rather than quickly.
2. **The synthesiser.** Register writes to `0388h` are shadowed but nothing
   renders them. `nano/opl/opl3.c` is a complete OPL3 and deliberately
   depends on nothing but stdint, but it is C and the host is assembly in a
   resident module. That is the open architectural question: either a
   C-capable resident driver, or the host captures and something else plays.
3. **Real games.** All of this rides on the DPMI host, and DOS/4GW still
   stops after its banner (see `DPMI-STATUS.md`). Until that is fixed the
   only clients are ones we write.

## What is ruled out

- Real-mode games. All of the above depends on running the game at ring 3,
  which needs the DPMI host. A real-mode game owns the machine and would need
  a V86 monitor instead. Even SBEMU, which does this properly, hands that
  case to QEMM or JEMM.

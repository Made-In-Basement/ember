# DPMI host: where it stands (2026-09-09)

`DPMI.MOD` (`modules/dpmi.asm`, `dpmi_pm.inc`, `dpmi_31.inc`) is a DPMI 0.9
host for 32-bit clients. It is **off by default**: `LOAD DPMI` advertises it
(INT 2Fh AX=1687h), and until it is loaded DOS/4GW keeps using its own raw
mode, which is what makes Hexen and Doom run today.

## What works end to end

- `DPMITEST.COM` (`programs/dpmitest.asm`) enters as a 32-bit client and
  passes every check: version, a descriptor onto the screen, 1 MB of
  extended memory written and read back, 4 KB of DOS memory through a
  selector, a real-mode interrupt with registers both ways, an exception
  handler that steps over a division by zero, the timer hooked in protected
  mode and chained to the default handler, a real-mode callback reached
  from a real-mode procedure, and the virtual interrupt flag. It leaves
  through INT 21h 4Ch and the shell and keyboard are fine afterwards.
- DOS/4GW (the `DOOM.EXE` in `root/DOOM`) takes the host: its 16-bit
  kernel initialises, allocates and frees DOS and extended memory, moves
  its descriptor table into extended memory, loads its 32-bit loader, and
  prints its banner. Three host bugs were found and fixed on the way: the
  flags a client's INT 21h handler returns must reach the caller (CF is
  how DOS says no), the PSP's environment field must hold a selector while
  a client runs, and function 0306h (raw mode switch) must exist because
  DOS/4GW stores its answer without checking for an error.

## On real hardware

Everything above is QEMU.  The module was then run on the Broadwell laptop,
where `LOAD DPMI` announced itself and `XMSTEST` confirmed the modules are
resident in the high memory area, but `DPMITEST` stopped with

    DPMI: unhandled exception 138 at 0008:000019C2 error 00000000

138 is 0x80 | 10: the host's own report of #TS, taken inside host code
(`SEL_HCODE32`).  0x19C2 is the module's ORG plus 0x9C2, which is the `iretd`
at the end of `pm_first_entry` - the instruction that first puts the client
at ring 3.  An IRET raises #TS with error code 0 in exactly one way: with NT
set it is a task *return*, and it reads the back link of the current TSS,
which is zero.

NT arrives from real mode.  The client far-calls the entry point, the host
switches to protected mode with a far jump, and EFLAGS crosses over
untouched; in real mode NT means nothing, so nobody had cleared it.  QEMU
happened to have it clear and the laptop's firmware did not.  The kernel's
own 32-bit mode (`src/pm32.asm`) is not exposed to this: its IRETs are all
inside handlers entered through interrupt gates, and a gate clears NT.

The fix is to start from a known EFLAGS on every arrival from real mode: the
`FLAGS_KNOWN` macro (`push dword 2` / `popfd`).  It needs a stack, so it goes
*after* the ring-0 stack pointer is loaded and never before - once at
`pm_first_entry`, and once in each of `pm_reenter`'s five branches, since each
of those chooses its own ESP and a single copy at the top would either push
into whatever linear address real mode left in ESP or, hoisted to `r0_top`,
tread on the outermost frame when an excursion is nested.  The interrupt
controller's in-service read in `int_common` gained the settling delay between
the OCW3 write and the read that a real 8259 wants, which QEMU never needed.

With that in, the laptop got one instruction further and stopped with

    DPMI: unhandled exception 13 at 0007:000001A9 error 00000000

0007 is the client's own CS at ring 3 and 1A9 is `DPMITEST`'s first
instruction, a write through DS - so the host had handed it a DS it could
not write to.  `desc_new` was the reason: it took the limit apart with
`shr ecx, 16` in place, so ECX came back as 0, and `pm_first_entry` set the
limit once and let it stand across the calls that follow.  CS got 0FFFFh;
DS and SS got a segment one byte long.

QEMU passed the whole self-test anyway, because **QEMU does not enforce the
limit of a data segment** - only the base.  The LDT it built during a clean
run says so outright:

    idx 0 sel 0007  base 00018060 limit 0FFFF  acc FB   <- CS
    idx 1 sel 000F  base 00018060 limit 00000  acc F3   <- DS
    idx 2 sel 0017  base 00018060 limit 00000  acc F2   <- SS
    idx 3 sel 001F  base 00018060 limit 000FF  acc F3   <- PSP
    idx 4 sel 0027  base 00018010 limit 0FFFF  acc F2   <- environment
    idx 5 sel 002F  base 00116000 limit 07FFF  acc F2   <- locked stack

(`tools/qemu_test.py --pmemsave 0x110000,64,build/ldt.bin` - the client's
region starts at `LOW_BASE` and the LDT is at the front of it, so the
descriptors are still readable after the client has left.)  `desc_new` now
gives EAX and ECX back untouched, and `pm_first_entry` states the limit at
every call rather than leaving one standing.

Eight of the twelve checks then passed and the ninth stopped with

    DPMI: unhandled exception 13 at 0007:00000554 error 00000000

which is `DPMITEST`'s own fault, not the host's: its three handlers each
noted their visit with a write through CS.  A code segment is never
writable, whatever its R bit says, and QEMU does not enforce that either.
They load DS from a read through CS instead - `push ds` / `mov ds,
[cs:pm_ds]` / write / `pop ds` - which is what a handler should do anyway,
since a tick or a callback can arrive with anything in DS.  The pair is
balanced before the exception handler touches its frame, so `[esp+12]` still
means what it did.  Nothing else in the tree writes through a code selector:
`dpmi_pm.inc`, `dpmi_31.inc` and `src/pm32.asm` have no CS-relative access at
all, and the other programs that do are real-mode only, where it is legal.

The tick test's patience went from 4000000h to 40000000h at the same time.
Four ticks is 220 ms and the old budget ran out at three often enough to
fail a good host - and a real processor spins that loop faster than QEMU
does, so it would have failed on the laptop as well.

Ten of twelve then, stopping at the callback with #GP(0) on `cb_proc`'s read
of the real-mode stack it is handed.  `desc_set_base_limit` popped in the
order it had pushed, so `pop ecx` took the saved ESI and the limit written
was whatever ESI held on entry - and its one caller, `callback_enter`,
arrives straight out of a `rep movsw`, so the callback's stack selector came
out about 54DEh long instead of FFFFh.  The pops run in LIFO order now and
EAX, ECX and ESI all come back untouched.

## Descriptors are checked against what was asked for

Three hardware trips went to descriptor routines that scramble a register:
QEMU builds the same wrong descriptor and never complains, because it
enforces a data segment's base and nothing else.  So `desc_new` and
`desc_set_base_limit` now end in `desc_check`, which reads the descriptor
back through the same `desc_addr` path, reassembles base and limit, and
stops the client if either differs from what the caller asked for (a
page-granular limit counts as wrong: every descriptor the host builds for
itself is byte granular).  The report comes out in real mode, like a fault:

    DPMI: descriptor 0047 was asked for base 000FFFF0 limit 0000FFFF, reads back base 000FFFF0 limit 000054DE

That line is from QEMU, with the `desc_set_base_limit` bug deliberately put
back - the class of bug that used to need the laptop now fails on the
desk.  Descriptors the *client* sets through INT 31h 0007h and 0008h are
written inline and are not checked: they are the client's business, and
0008h is allowed to be page granular.

## The exact symptom (DOS/4GW)

After the banner, DOS/4GW's 32-bit loader (a 16-bit code segment at
selector 87h, base 1204E0h) tries to load a selector it does not own
(#GP, error code 1520h, selector 1522h). Its own #GP handler redirects
execution to a fix-up routine that asks the DOS/16M kernel through
`INT FCh` (AX=0580h, BX=1522h). Under this host that vector is hooked
neither in protected mode (no 0205h for FCh; the 33 hooks are 10h-2Eh,
75h and the exceptions 00, 01, 03, 23) nor in real mode (no 0201h; the
real-mode vector is the BIOS's dummy IRET), so the interrupt is reflected
to real mode, returns empty, the loader retries seven times, starts
printing an error with INT 21h AH=06h, and its error path then jumps into
empty memory at 2FFF:0C90 with interrupts off. `info pic` at that point
shows IRQ1 unmasked (mask B8h) with IRQ0 and IRQ4 pending: it is a jump
into zeros, not a wait for a key.

## Ruled out

- Extended memory above 16 MB (a 24 MB machine fails the same way; the
  client's memory now comes from the low region above the HMA anyway).
- Any INT 31h call failing: only 0A00h (vendor API) does, by design.
- Masked or lost interrupts during real-mode excursions.
- The frame layout of exception handlers and interrupt delivery (the
  self-test exercises both, and the kernel's handlers run).

## Ruled out since

- **The mode flags at 0400h.** The host answers `BX=0001`: 32-bit clients,
  and a reflected interrupt drops to *real* mode rather than V86, which is
  true of it and unusual among hosts. Claiming V86 instead (`BX=0003`)
  changes nothing: DOS/4GW stops at the same place. Whatever tells its
  loader it may use the raw-mode `INT FCh` services, it is not this.

## It runs Doom (2026-09-09)

DOS/4GW gets all the way into the game now: the attract-mode demo plays
under this host, with the full heads-up display and a working timer. Two
things were wrong, and neither was the INT FCh everything pointed at.

**A client's BIOS calls were going to its exception handlers.** Below 32, an
interrupt a client *makes* and an exception it *takes* are the same thing at
the gate: the processor delivers both through the one vector and says
nothing about which. INT 10h is the video BIOS and vector 16. INT 11h is the
equipment list and vector 17. This host asked whether the client had an
exception handler before asking whether it had an interrupt handler, and
DOS/4GW installs exception handlers for 06h-11h - so every video call it
made went to its own #MF handler, whose first instruction is `int FCh` into
a DOS/16M kernel API that nothing under DPMI installs. All of the INT FCh
archaeology below is downstream of that. `was_software_int` reads the two
bytes in front of the saved EIP: an INT leaves `CD nn` behind it, a fault
leaves EIP on the instruction that faulted.

Vector 17 also stopped being treated as arriving with an error code. It is
alignment check, which needs CR0.AM and EFLAGS.AC and so never happens,
while INT 11h lands on the same vector constantly - and assuming an error
code there puts the whole frame four bytes out.

**And the client could not get its interrupts back.** After that fix
DOS/4GW loaded its WAD and then sat spinning with five hardware interrupts
in a hundred seconds. The controller was ready - `irr=11`, IRQ 0 unmasked,
nothing in service - so the processor simply was not taking them: IF was
clear and stayed clear.

At CPL 3 with IOPL 0, CLI and STI fault, so a host can emulate them. POPF
and IRET do not: they **silently ignore** the interrupt flag, with no fault
for a host to step in on. A client that says PUSHF, CLI, ... POPF never
gets its interrupts back, and DOS/4GW says exactly that.

The two things this host wants are exclusive on this processor:

    the permission map is consulted only when CPL is *greater* than IOPL
    POPF and IRET honour IF only when CPL is *not greater* than IOPL

So the client runs at IOPL 3 by default and DOS/4GW works; `LOAD DPMI TRAP`
runs it at nought instead, which is how `SBTEST` is run and the only way to
watch a protected-mode client's ports. Real-mode games need neither choice:
virtual-8086 mode consults the map whatever the privilege level, which is
why `SB.MOD` has both at once.

**What that leaves open:** a DOS/4GW game runs but has no sound, because the
host cannot both watch its ports and let it keep its interrupts. That is now
the whole of what stands between Hexen and a Sound Blaster.

## What the trace says (2026-09-09)

`-DTRACE_FIRST` and `-DTRACE_VECTORS` (see `modules/dpmi_31.inc`) keep the
*first* five hundred INT 31h calls including the 02xx ones, which is where a
client decides what it has found. DOS/4GW's opening moves:

    0   000Bh, 000Ch    two descriptors set up
    4   0A00h           the vendor API, refused (by design)
    5   0305h           save/restore addresses - answered
    6   0306h           raw mode switch addresses - answered
    7   0003h           selector increment: 8
    8   0000h           six LDT descriptors, from 0037h
    ...
    7   0204h x 256     every protected-mode vector, read
    264 0202h x 32      every exception handler, read
    296 0203h x 13      exception handlers 06h-11h, set
    309 0205h x 41      00, 01, 03, 23, 10-15, 17-1A, 1D-1F,
                        20-22, 25-2E, 21, 10, 75 - set

So it surveys the whole interrupt table and then hooks a list. **FCh is not
on that list and never becomes so**, and the real-mode vector for FCh is
still the BIOS's dummy `IRET` at `F000:FF53` at the moment it stalls -
checked, not assumed. Nothing in either mode has ever installed the API the
32-bit loader then calls. The dummy `IRET` returns the flags it was handed,
which reads as success, and the loader walks on.

Making INT FCh fail honestly (CF=1, no reflection) changes nothing: by the
time it is called the road has been chosen. The raw mode switch whose
addresses were asked for at call 6 is never used either - no `resume`
excursions appear in the trace at all.

Two things worth following:

- **Something writes a bad far pointer into the real-mode interrupt table.**
  `INT 2Fh` is `FFFF:10E8` (this host's own hook) after `LOAD DPMI` and
  `A700:0068` at the stall. `A700h` is video memory; no real-mode code lives
  there. Selector `00A7h` exists in the client with base `00131FF0`. A
  protected-mode pointer written where a real-mode one belongs is exactly
  the confusion the selector `1522h` fault is made of.
- **The 32-bit loader's INT FCh thunks** are a table of thirty-four six-byte
  entries at linear `1208E0h`-`1209A5h`, each `call rel16` / `db n` /
  `int FCh`. Finding what calls them, and what branch chose that path over
  the DPMI one, is the disassembly that would settle this.

## What to try next

1. How does the DOS/16M kernel expect `INT FCh` to reach it in DPMI mode?
   The 32-bit loader carries a table of `INT FCh` stubs (loader offset
   420h-470h). Either the kernel hooks FCh through a path this host does
   not offer (compare the 0205h list against a run under HDPMI or
   CWSDPMI), or the loader was told it is *not* under DPMI and used the
   raw-mode API: check what the kernel hands the loader as its mode flag.
2. Why the selector 1522h is wanted at all: 15220h lies inside the
   kernel's own segment. DOS/4GW may be walking a DOS structure (the list
   of lists from INT 21h 52h, or the SFT) that this kernel fills
   differently; the earlier `4400h` IOCTL answers are already synthetic.
3. The error path's jump into zeros is a second bug, probably in how a
   nested 0302h from inside the exception handler context returns.

## Tooling that made this possible

`tools/qemu_test.py --qemulog` (`-d int`), `--logmask exec,nochain,int`
switched on just before the last key, `--moncmd 'info pic'`, `--mem`;
`tools/dpmitrace.py` decodes the host's trace page at 7C00h (real-mode
excursions, failed INT 31h calls) and the ring of INT 31h calls the host
keeps in the client's region; `tools/tbtrace.py` turns the block trace
between two events into a disassembly against a memory dump.

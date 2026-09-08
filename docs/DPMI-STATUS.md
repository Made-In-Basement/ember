# DPMI host: where it stands (2026-09-07)

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

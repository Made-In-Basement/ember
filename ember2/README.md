# Ember 2.0

Ember on a machine with no BIOS.

Ember is a real-mode kernel.  It boots off an MBR, reads its disk through
`INT 13h`, draws through `INT 10h`, takes keys through `INT 16h`.  A laptop
that boots only UEFI has none of those, and most laptops made since 2020 boot
only UEFI.  Ember 2.0 is the same kernel, unchanged, on top of a small BIOS of
its own.

## What is in here

- `stub.c` — the UEFI application.  Reads `EMBER.IMG` from the stick into
  memory, finds the framebuffer and the memory map, chooses where everything
  goes, leaves boot services, and calls the shim.
- `shim.asm` — the BIOS.  Drops the processor from long mode to real mode,
  builds an interrupt table that points into itself, and boots the image.
  `INT 13h` is served from the copy in memory; `INT 10h` draws an 80×25
  console onto the framebuffer through a paging window, because on these
  machines the framebuffer sits above four gigabytes where real mode cannot
  reach; `INT 16h` runs the keyboard controller; `INT 15h` says how much
  memory there is.

## Making a stick

    python build.py
    python tools/build_ember2.py

then copy the contents of `build/ember2` onto a FAT32 stick — two things:
`EFI\BOOT\BOOTX64.EFI` and `EMBER.IMG` in the root.  Boot the stick from the
firmware's one-time boot menu.  Secure Boot has to be off; nothing here is
signed.

To test without a stick:

    python tools/ember2_test.py --keys "dir{ret}" --png build/e2.png

boots it under EDK II in QEMU and prints what the console said.

## What works, under EDK II in QEMU

Boot to the prompt, the keyboard, scrolling, `DIR` and `MEM`, reads and
writes to the disk (a write to `EMBER.LOG` changes exactly the two sectors it
should in the copy), the DPMI host with all twelve of `DPMITEST`'s checks,
and `EMBER` declining cleanly with "no true-colour mode with a linear
framebuffer".

## What it is, and is not, yet

The disk is a copy.  Anything written — `EMBER.LOG`, saved files — lasts until
the machine is turned off.  The stick itself is never written.

The V86 monitor (`LOAD SB`) is not usable yet.  Its rule for a real-mode
handler that touches `CR0` or the GDT — which is exactly how this shim draws
— is to stand down for good, so a program that prints anything runs the rest
of the way natively and its card is never emulated.  The fix belongs in the
monitor: a real-mode excursion for BIOS vectors, out of V86 and back, using
its own `v86_leave` and `v86_enter`.  Until then `HELLO` prints and `SBREAL`
reports "no monitor", both correctly.

The screen is a text console.  Programs that draw into VGA memory at `A000h`
— every DOS game — get nothing on machines without a VGA core, which is every
Tiger Lake and Alder Lake laptop.  The shim reports no VESA and refuses
graphics modes, so Ember's desktop declines cleanly rather than drawing into
the dark.  Putting `A000h` behind the same paging window and blitting it is
the next piece, and a bounded one.

The keyboard has to be on an 8042.  `uefi/probe.c` finds out whether it is
before any of this is tried; on the machines it was run on, it was.

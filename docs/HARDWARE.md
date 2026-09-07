# What the hardware said

Facts gathered from real machines with the probe programs (`PCI`, `VMODES`,
`MOUSE`), kept here because they decide what is worth writing.

## Lenovo Yoga 3 Pro-1370 (Broadwell-U, Core M)

PCI (`PCI.TXT`, 2026-09-05):

    00:02.0 8086:161E  0300   graphics
    00:03.0 8086:160C  0403   audio (display)
    00:14.0 8086:9CB1  0C03   xHCI USB controller - the only USB host
    00:16.0 8086:9CBA  0780   management engine
    00:1B.0 8086:9CA0  0403   HD Audio - what the sound driver uses
    00:1F.2 8086:9C83  0106   SATA (AHCI)
    00:1F.3 8086:9CA2  0C05   SMBus
    01:00.0 14E4:43B1  0280   Broadcom wireless

There is no class 0C80 device: the Intel LPSS I2C controllers that the
touchpad and (probably) the touchscreen hang off are in ACPI mode, hidden
from PCI.  Reaching them means reading the ACPI tables for their addresses,
then writing an I2C master driver and an I2C-HID driver.  The USB mouse is
reachable two ways: through the firmware's PS/2 impersonation (flaky, see
below) or with an xHCI driver of our own.

Screen modes (`VMODES.TXT`): the panel is 3200x1800.  8-bit modes offered:
640x480, 800x600, 1024x768, 1280x1024, 1600x1200, 1920x1440 and the native
3200x1800 (017Dh); the same sizes at 16 and 32 bits.  No other widescreen
size, so the boot splash either takes 3200x1800 or fills a 4:3 mode and
lets the panel stretch it.

Mouse: a USB mouse impersonated as PS/2 by the firmware.  Reported as
moving in random directions with phantom right clicks under the desktop's
first drivers; `MOUSE.TXT` will say why.

## The same laptop, seen from Windows 11 (2026-09-05)

`Get-PnpDevice` and the allocated resources settle how the input devices are
attached:

    touchpad      ACPI\SYNA2B22   Synaptics, HID over I2C      interrupt line 26
    touchscreen   ACPI\ATML1000   Atmel maXTouch, HID over I2C  interrupt line 27
    I2C host 0    ACPI\INT3432    Intel Serial IO (9CE1)  memory FE103000  IRQ 7
    I2C host 1    ACPI\INT3433    Intel Serial IO (9CE2)  memory FE105000  IRQ 7
    GPIO          ACPI\INT3437    Intel Serial IO GPIO    port 0800
    mouse         USB\VID_046D&PID_C52B  a Logitech Unifying receiver

So both touch devices are HID-over-I2C behind Intel's LPSS controllers,
which are DesignWare I2C blocks at fixed memory addresses.  Those are
reachable from the kernel the same way the HD Audio registers are (unreal
mode).  The interrupt lines the touch devices use are GPIO pins; a driver
that polls needs none of that.

The "PS/2 mouse" the firmware impersonates is a Logitech Unifying
receiver, which carries a keyboard and a mouse on one USB device.  That is
a harder thing to impersonate than a plain mouse and may be why the
impersonation misbehaves; a plain wired USB mouse is a cheap experiment.

From the DSDT (dsdt.reg exported by Windows, decoded here):

    touchpad     SYNA2B22  on \_SB.PCI0.I2C0 (FE103000)  address 2Ch  400 kHz  HID descriptor at register 0020h
    touchscreen  ATML1000  on \_SB.PCI0.I2C1 (FE105000)  address 4Ah  400 kHz  HID descriptor at register 0000h
    (ATML7000 is an alternative touchscreen at the same address; ATML2000 at 26h is its boot bridge)

The hosts' _PS0 (method LPD0) powers one up by clearing bits 0-1 of the
dword at its private area + 84h, i.e. FE103884h / FE105884h; _PS3 sets
them to 3.  The bus timing counts (SSCN/FMCN) are in a secondary table
that was not exported; the probe uses the textbook values for a 100 MHz
clock at standard speed.

`I2C.COM` (programs/i2c.asm) is the probe built from these facts: it
checks each host's identity register (44570140h), powers it up if need
be, reads the HID descriptor from each device, then switches the device
on, resets it and lists the reports it gives while touched.

## The touchpad, working (2026-09-05, I2C.TXT run 3)

Waking host 0 through its configuration page (FE104084h, clear bits 0-1)
brought the block up: identity 44570140h, version "*511", FIFOs of 32.
The pad answered at 2Ch with its HID descriptor: Synaptics 06CB:2714,
report descriptor 133 bytes, input register 0024h, command register
0022h, data register 0023h, reports up to 32 bytes.  In its default mode
it sends a mouse report: `06 00 01 00 dx dy` - length 6, report 1,
buttons, then signed x and y.  Polling it with plain reads works; an idle
pad answers with a zero length.  nano/gui/touch.c is the driver.

The touchscreen (host 1, 4Ah) answered its descriptor too - Atmel
03EB:8A10, input register 00D3h, reports up to 20 bytes - but has not yet
been seen to report; nobody had touched it during its window.

## The framebuffer's memory type (2026-09-06)

The Monitor measured pushing pixels to the card at about 20 MB/s: 35.7 ms
for a window-sized region at 1600x1200, against 9.4 ms of drawing.  The
cause was the memory type.  The firmware's table (default UC, 10
variable registers, 5 in use):

    0: 000000000  16384 MB  WB     all RAM, remap included
    1: 09D000000     16 MB  UC     holes for the devices, from TOLUD = 9D000000
    2: 09E000000     32 MB  UC
    3: 0A0000000    512 MB  UC
    4: 0C0000000   1024 MB  UC

The screen sits inside both the WB range and a UC hole; UC wins.  A WC
range over WB is undefined, so nano/gui/mtrr.c redraws the table on
start-up: RAM to 9D000000 as WB blocks 2 GB + 256 + 128 + 64 + 16 MB, the
holes left to the default, and 8 MB of framebuffer WC.  Result, in the
user's words: "WOW it's so fast now".  The firmware's table is restored on
exit.

## The graphics engine (2026-09-06, GPU.N32 runs 1-8)

HD Graphics 5300, PCI 0:2.0, device 8086:161E (Broadwell-Y GT2), revision 9.
Memory decoding and bus mastering on.  BAR0 (registers) C0000000, 16 MB;
BAR2 (aperture) B0000000, 256 MB.  GGC 01C1: 32 MB of stolen memory at
9E000000 (the 32 MB uncacheable MTRR hole), an 8 MB global page table
(1 M entries, 4 GB of GPU address space).  The stolen memory lies above
TOLUD (9D000000): the processor cannot read it directly, only through the
aperture.

The global page table is at BAR0 + 8 MB.  The firmware maps the stolen
memory from GPU address 0 linearly (entry 0 = 9E000001, entry 1 =
9E001001).  The VESA framebuffer B0000000 is the aperture, which is GPU
address 0, which is the stolen memory: three names for the same pages.
GFX_FLSH_CNTL (101008) = 1 after writing entries.

Forcewake works: FORCEWAKE_MT (A188) := 00010001, ack at 130044 bit 0
after ~230 reads.  RC control (A090) was 0.  Both rings idle and empty;
the render engine in legacy ring mode (GFX_MODE 2800, execlists off).
The blitter ring starts and runs: MI_MODE stop, HWS, START, CTL = 1, MI_MODE
run; XY_COLOR_BLT (7 dwords) and XY_SRC_COPY_BLT (10 dwords) with 64-bit
addresses execute, MI_FLUSH_DW after them, head reaches tail.

The cache attribute table (PPAT, 40E0/40E4) is left by the firmware as
03030303 03030303: every entry write-back.  Engine writes to the screen
therefore stayed in the LLC, and the display, which reads memory, showed
only the lines that had been evicted: horizontal 64-byte dashes.  The PAT
index bits of a global-table entry (3, 4, 7) are NOT honoured: mappings
marked uncached, write-combining and write-through behaved exactly like
write-back.  Setting PPAT entry 0 to 00 (uncached) made engine writes land
in memory at once.  MI_FLUSH_DW bit 9 ("LLC flush") did nothing.  A CPU
wbinvd also settles everything.  Reads through the aperture see the LLC,
so they say "written" while the display still shows the old memory.

The display plane: pipe A, plane A (cntr 98000000: on, format 6 =
XRGB8888, linear), stride 12800, surface 0, in the 3200x1800 mode (017F);
PIPECONF reads 40000000 in every mode.  DSPASURF (7019C) can be pointed at
any GPU address; DSPASURFLIVE (701AC) shows the surface actually being
scanned, and follows within a frame.  The display reads memory, not the
LLC, whatever the attributes: processor writes to a scanout buffer need
clflush (or wbinvd) and nothing else.  Measured: clflush of 22.5 MB, 16.2
ms (about 45 ns a line, whatever its state); a 400x400 region drawn and
flushed, 1.2 ms.  TSC 1396 MHz.

The cursor plane works: CURACNTR (70080) := 27 (64x64 ARGB), CURAPOS
(70088) = y<<16 | x with sign bits 31 and 15, CURABASE (70084) := the GPU
address of a 16 KB image (four pages); the base write applies the others.
Per-pixel alpha is honoured.

What the desktop does with it (nano/gui/gpu.c): the back buffer, page
aligned, is entered into the table at GPU address 10000000 and the plane
pointed at it; present = flush the damaged lines (wbinvd above 2 MB); the
pointer is the cursor sprite.  The firmware's surface is restored on exit.
Not yet used: the blitter (would need PPAT entry 0 uncached, and then all
GGTT traffic is uncached, so the source must be flushed too).

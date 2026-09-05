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

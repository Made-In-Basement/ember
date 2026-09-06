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

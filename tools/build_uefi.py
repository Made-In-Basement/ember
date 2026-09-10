#!/usr/bin/env python3
"""uefi/probe.c, built as a UEFI application.

Ember boots as an MBR disk and needs a BIOS.  A UEFI-only machine has none,
and the plan for such a machine is a stub that stands in for one - but whether
that plan is worth writing depends on what the machine still has.  probe.c
goes and looks; this builds it.

Writes build/BOOTX64.EFI.  Put it on a FAT32 stick as \\EFI\\BOOT\\BOOTX64.EFI
and boot from it; the machine's own boot menu will offer the stick.  Secure
boot has to be off, because nothing here is signed.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build')
SRC = os.path.join(ROOT, 'uefi', 'probe.c')

sys.path.insert(0, os.path.join(ROOT, 'tools'))


def build(quiet=False):
    from build_opl import find_zig
    zig = find_zig()
    if zig is None:
        sys.exit('no zig found: see tools/build_doom.py for where it looks')
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, 'BOOTX64.EFI')
    # -target x86_64-uefi-msvc gives a PE32+ with subsystem 10 and the
    # Microsoft calling convention, which is what the firmware calls with.
    # The entry point zig's linker looks for is EfiMain.
    r = subprocess.run([zig, 'cc', '-target', 'x86_64-uefi-msvc',
                        '-ffreestanding', '-nostdlib', '-fno-stack-protector',
                        '-fshort-wchar', '-Wall', '-Wextra',
                        '-mno-red-zone',        # firmware may use interrupts
                        '-O2', SRC, '-o', out],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('building the probe failed:\n' + r.stderr[:4000])
    if r.stderr.strip() and not quiet:
        print(r.stderr.strip()[:2000])
    if not quiet:
        print('  UEFI probe: %d bytes -> build/BOOTX64.EFI'
              % os.path.getsize(out))
        print('  copy to a FAT32 stick as \\EFI\\BOOT\\BOOTX64.EFI')
    return out


if __name__ == '__main__':
    build()

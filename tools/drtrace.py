#!/usr/bin/env python3
"""Decode the card's watch page, which modules/dpmi.asm writes when DRLOG is
defined in it.  Without that define the module writes nothing there and this
prints zeroes.

    python tools/qemu_test.py ... --pmemsave 0x7800,0x400,build/dr.bin
    python tools/drtrace.py build/dr.bin

The page holds three counts and a ring of what was written to, or read from,
the emulated card: which port, which opcode it was, and what was in AL at the
time.  The synthesiser's own two ports are left out - a game writing music
makes tens of thousands of those and they crowd out everything else.  Past
the ring is what the card and the pump thought they were doing as of the last
time the pump ran.
"""
import struct
import sys

RING = 0x20
RING_MAX = 128
AUX = 0x240


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "build/dr.bin"
    d = open(path, "rb").read()
    seen, ours, unknown, noted = struct.unpack_from("<4I", d, 0)
    print(f"debug traps {seen}, the card's {ours}, undecodable {unknown}")

    calls, ready, work, frames = struct.unpack_from("<4I", d, 16)
    print(f"the pump ran {calls} times, {ready} with a stream, {work} with "
          f"something to say; {frames} frames")

    err, have, writes, live = struct.unpack_from("<4I", d, AUX)
    playing, blocks, speaker, loudest = struct.unpack_from("<4I", d, AUX + 16)
    ring, size, lpib, wpos = struct.unpack_from("<4I", d, AUX + 32)
    print(f"the stream: {'held' if have else 'NOT held'} (INT 21h F0h said "
          f"{err:04X}h), ring {ring:08X} of {size} bytes, playing at {lpib}, "
          f"writing at frame {wpos}")
    print(f"the synthesiser: {writes} register writes, "
          f"{'live' if live else 'silent'}")
    print(f"the transfer: {'running' if playing else 'stopped'}, {blocks} "
          f"blocks played, speaker {'on' if speaker else 'off'}")
    print(f"the loudest frame the pump laid down: {loudest}")

    print(f"the card was touched {noted} times outside the synthesiser"
          + (f" (the last {RING_MAX})" if noted > RING_MAX else "") + ":")
    for k in range(max(0, noted - RING_MAX), noted):
        port, op, val = struct.unpack_from("<HBB", d, RING + (k % RING_MAX) * 4)
        way = "wrote" if op & 2 else "read "
        print(f"  {port:04X}h {way} {val:02X}")


if __name__ == "__main__":
    main()

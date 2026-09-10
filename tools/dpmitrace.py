#!/usr/bin/env python3
"""Decode the DPMI host's trace page (7C00h..7FFFh), saved with
    python tools/qemu_test.py ... --pmemsave 0x7c00,0x400,build/trace.bin
Real-mode excursions the host made for the client, then the INT 31h calls
that failed."""
import struct
import sys

KINDS = {0: "int", 1: "call", 2: "call/iret", 3: "terminate", 4: "fault", 5: "resume"}


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "build/trace.bin"
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d, 0)[0]
    print(f"real-mode excursions: {n}" + (" (the last 40)" if n > 40 else ""))
    for k in range(max(0, n - 40), n):
        i = k % 40
        kind, vec, ax_in, bx_in, ax_out, fl = struct.unpack_from("<BBHHHH", d, 4 + i * 12)
        what = KINDS.get(kind, str(kind))
        target = f"INT {vec:02X}h" if kind == 0 else what
        print(f"  {target:10} AX={ax_in:04X} BX={bx_in:04X} -> AX={ax_out:04X} "
              f"{'CF' if fl & 1 else '  '} flags={fl:04X}")
    base = 0x7E00 - 0x7C00
    if len(d) > base + 4:
        m = struct.unpack_from("<I", d, base)[0]
        print(f"exceptions the client took: {m}")
        for k in range(min(m, 30)):
            vec, err, cs, eip = struct.unpack_from("<4I", d, base + 16 + k * 16)
            print(f"  exception {vec:2} error {err:04X} at {cs:04X}:{eip:08X}")
    r = open(sys.argv[2], "rb").read() if len(sys.argv) > 2 else b""
    total = struct.unpack_from("<I", r, 0)[0] if r else 0
    print(f"INT 31h calls: {total}; the last {min(total, 500)}:")
    first = max(0, total - 500)
    for k in range(first, total):
        e = 4 + (k % 500) * 16
        fn, bi, ci, di, ao, bo, co, do_ = struct.unpack_from("<8H", r, e)
        bad = "FAILED " if fn & 0x8000 else ""
        if fn & 0x7FFF in (0x000B, 0x000C):
            b = r[e + 8:e + 16]
            base = b[2] | (b[3] << 8) | (b[4] << 16) | (b[7] << 24)
            lim = b[0] | (b[1] << 8) | ((b[6] & 15) << 16)
            print(f"  {fn & 0x7FFF:04X}h BX={bi:04X} from {di:04X} {bad}descriptor base={base:08X} "
                  f"limit={lim:05X} access={b[5]:02X} flags={b[6] >> 4:X}")
            continue
        print(f"  {fn & 0x7FFF:04X}h BX={bi:04X} CX={ci:04X} from {di:04X} -> "
              f"{bad}AX={ao:04X} BX={bo:04X} CX={co:04X} DX={do_:04X}")
    n = struct.unpack_from("<I", d, 0x200)[0]
    print(f"failed INT 31h calls: {n}")
    for i in range(min(n, 32)):
        err, fn = struct.unpack_from("<HH", d, 0x204 + i * 4)
        print(f"  function {fn:04X}h -> error {err:04X}h")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Read a QEMU log made with -d exec,int (see qemu_test.py --logmask) and show
the blocks executed between two interrupt events, disassembled from a memory
dump of the code.

    python tools/tbtrace.py build/exec.log build/d16m.bin 0x24C20 \
        --from "EAX=0001000c" --to "EAX=00010006" --skip 0

The dump covers [base, base + len(dump)); blocks outside it are named by
address only.  --from picks the Nth (--skip) matching interrupt line as the
start; --to the next matching line after it."""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ND = ROOT / "tools" / "nasm" / "ndisasm.exe"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("dump")
    ap.add_argument("base", type=lambda s: int(s, 0))
    ap.add_argument("--from", dest="start", required=True)
    ap.add_argument("--to", dest="end", required=True)
    ap.add_argument("--skip", type=int, default=0)
    ap.add_argument("--last", action="store_true", help="the last --from match")
    ap.add_argument("--bits", type=int, default=16)
    ap.add_argument("--max", type=int, default=400)
    args = ap.parse_args()

    code = open(args.dump, "rb").read()
    lines = open(args.log, errors="replace").read().splitlines()
    starts = [i for i, l in enumerate(lines) if args.start in l and "v=" in l]
    if not starts:
        sys.exit("no start event")
    s = starts[-1] if args.last else starts[args.skip]
    e = next((i for i in range(s + 1, len(lines)) if args.end in lines[i] and "v=" in lines[i]),
             len(lines))
    print(f"window: log lines {s}..{e}")
    pcs = []
    for l in lines[s:e]:
        m = re.match(r"Trace \d+: [0-9a-fx]+ \[[0-9a-f]+/([0-9a-f]+)/", l)
        if m:
            pc = int(m.group(1), 16)
            if not pcs or pcs[-1] != pc:
                pcs.append(pc)
    print(f"{len(pcs)} blocks")
    for pc in pcs[:args.max]:
        off = pc - args.base
        if 0 <= off < len(code):
            piece = code[off:off + 24]
            tmp = ROOT / "build" / "tb.bin"
            tmp.write_bytes(piece)
            out = subprocess.run([str(ND), "-b", str(args.bits), "-o", hex(off), str(tmp)],
                                 capture_output=True, text=True).stdout.splitlines()
            print(f"  {off:04X}: " + " | ".join(x[28:].strip() for x in out[:3]))
        else:
            print(f"  (outside) {pc:08X}")


if __name__ == "__main__":
    main()

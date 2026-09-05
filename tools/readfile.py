#!/usr/bin/env python3
"""Read a file out of a Ember image (superfloppy or MBR-partitioned FAT12/16).

    python tools/readfile.py build/ember-usb.img EMBER.LOG
"""
import struct
import sys


def read_file(img_path, name):
    d = open(img_path, "rb").read()
    base = 0
    if d[0x1FE:0x200] == b"\x55\xAA" and d[0x1BE] == 0x80 and d[11:13] != b"\x00\x02":
        base = struct.unpack_from("<I", d, 0x1BE + 8)[0] * 512   # MBR: first partition
    bps, spc, reserved, fats, root_entries, total16 = struct.unpack_from("<HBHBHH", d, base + 11)
    fat_secs = struct.unpack_from("<H", d, base + 22)[0]
    fat_start = base + reserved * bps
    root_start = fat_start + fats * fat_secs * bps
    root_secs = (root_entries * 32 + bps - 1) // bps
    data_start = root_start + root_secs * bps
    total = total16 or struct.unpack_from("<I", d, base + 32)[0]
    clusters = (total - reserved - fats * fat_secs - root_secs) // spc
    fat12 = clusters < 4085

    def fat_entry(n):
        if fat12:
            off = fat_start + n + n // 2
            v = struct.unpack_from("<H", d, off)[0]
            return (v >> 4) if n & 1 else (v & 0xFFF)
        return struct.unpack_from("<H", d, fat_start + n * 2)[0]

    base8, _, ext = name.upper().partition(".")
    want = (base8.ljust(8) + ext.ljust(3)).encode()
    for i in range(root_entries):
        e = d[root_start + i * 32: root_start + i * 32 + 32]
        if e[0] == 0:
            break
        if e[:11] == want and not (e[11] & 0x08):
            cluster = struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            out = b""
            while 2 <= cluster < (0xFF8 if fat12 else 0xFFF8) and len(out) < size:
                off = data_start + (cluster - 2) * spc * bps
                out += d[off: off + spc * bps]
                cluster = fat_entry(cluster)
            return out[:size]
    raise SystemExit(f"{name} not found in {img_path}")


if __name__ == "__main__":
    data = read_file(sys.argv[1], sys.argv[2])
    sys.stdout.write(data.decode("cp437", "replace").rstrip(" ") + "\n")

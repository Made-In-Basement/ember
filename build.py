#!/usr/bin/env python3
"""
build.py - assemble Ember and produce a bootable FAT12/FAT16 disk image.

    python build.py                # 32 MB USB image (MBR + FAT16) -> build/ember.img
    python build.py --size 64      # bigger USB image
    python build.py --run          # build, then boot the image in QEMU
    python build.py --floppy       # 1.44 MB superfloppy image for floppy emulation

The USB image has a normal MBR partition table with one active FAT16
partition, which is what PC firmware and Windows expect from a stick.  Write
it with Rufus (DD mode), balenaEtcher, Win32DiskImager or dd; Windows then
mounts the partition as a FAT volume and extra files can simply be copied
onto it.  EMBER.LOG on the image is rewritten by the kernel with a boot
log (video mode, mouse, sound driver trace).
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC = ROOT / "src"
PROGRAMS = ROOT / "programs"
MODULES = ROOT / "modules"
FILES = ROOT / "root"
# root files replaced in a particular image, by name (see --retro)
OVERRIDES = {}

# The NanoDOS image: the video's "before".  The same kernel under the old
# name, a boot that scrolls three screens of invented trouble, and the old
# desktop putting up an error for every key.  A dramatisation, and labelled
# as one in the video; nothing in it is a record of anything.
RETRO_AUTOEXEC = (b"@ECHO OFF\r\n"
                  b"RETRO\r\n"
                  b"ECHO Type WIN to start the desktop.\r\n")
BUILD = ROOT / "build"
SECTOR = 512


# --------------------------------------------------------------------------- #
# tools
# --------------------------------------------------------------------------- #
def check_kernel_layout(kernel):
    """The kernel lives in one 64 KB segment with its stack at the top:
    read the marker it carries and refuse an image whose BSS would run into
    that stack, since the failure looks like random corruption at boot."""
    import struct
    i = kernel.find(struct.pack("<I", 0x4B4C4159))
    if i < 0:
        sys.exit("kernel layout marker not found")
    bss_start, bss_end, bss_top = struct.unpack_from("<III", kernel, i + 4)
    room = 0x10000 - bss_top
    print(f"  kernel: {len(kernel)} bytes of code and data, BSS to 0x{bss_top:04X}, "
          f"{room // 1024} KB left for the stack")
    if room < 3 * 1024:
        sys.exit(f"kernel too big: only {room} bytes between the BSS and the stack")


def find_nasm():
    for cand in (ROOT / "tools" / "nasm" / "nasm.exe", ROOT / "tools" / "nasm" / "nasm"):
        if cand.exists():
            return str(cand)
    found = shutil.which("nasm")
    if found:
        return found
    sys.exit("NASM not found: put nasm in tools/nasm/ or on PATH (https://www.nasm.us)")


def find_qemu():
    for cand in (r"C:\Program Files\qemu\qemu-system-i386.exe", "qemu-system-i386",
                 "qemu-system-x86_64"):
        if os.path.isabs(cand) and os.path.exists(cand):
            return cand
        found = shutil.which(cand)
        if found:
            return found
    return None


def assemble(nasm_exe, src, out, include_dir=None, defines=()):
    cmd = [nasm_exe, "-f", "bin", "-o", str(out), str(src)]
    for d in defines:
        cmd += ["-D" + d]
    if include_dir:
        for d in (include_dir if isinstance(include_dir, (list, tuple))
                  else [include_dir]):
            cmd += ["-i", str(d) + os.sep]
    print(f"  nasm {src.relative_to(ROOT)} -> {out.relative_to(ROOT)}")
    if subprocess.run(cmd).returncode:
        sys.exit(f"assembly failed: {src}")
    return out.read_bytes()


# --------------------------------------------------------------------------- #
# FAT image writer
# --------------------------------------------------------------------------- #
def fat_timestamp(ts):
    t = time.localtime(ts)
    date = ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    tm = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return date, tm


KEEP83 = "!#$%&()-@^_`{}~" + chr(39)


def short_name(name):
    """Host filename -> 11-byte 8.3 FAT name (upper case, space padded)."""
    base, dot, ext = name.upper().rpartition(".")
    if not dot:
        base, ext = name.upper(), ""
    keep = lambda s: "".join(c for c in s if c.isalnum() or c in KEEP83)
    base, ext = keep(base)[:8], keep(ext)[:3]
    if not base:
        raise ValueError("cannot make an 8.3 name from %r" % name)
    return (base.ljust(8) + ext.ljust(3)).encode("ascii")


def fits_83(name):
    """Is this already a plain 8.3 name, needing no long entries?"""
    base, dot, ext = name.rpartition(".")
    if not dot:
        base, ext = name, ""
    if len(base) > 8 or len(ext) > 3 or not base or name != name.upper():
        return False
    ok = lambda s: all(c.isalnum() or c in KEEP83 for c in s)
    return ok(base) and ok(ext)


def alias_name(name, taken):
    """An 8.3 alias for a long name: LONGNA~1.TXT, avoiding collisions."""
    base, dot, ext = name.upper().rpartition(".")
    if not dot:
        base, ext = name.upper(), ""
    keep = lambda s: "".join(c for c in s if c.isalnum() or c in KEEP83)
    base, ext = keep(base) or "FILE", keep(ext)[:3]
    for i in range(1, 1000):
        tail = "~%d" % i
        cand = ((base[:8 - len(tail)] + tail)[:8].ljust(8) + ext.ljust(3)).encode("ascii")
        if cand not in taken:
            return cand
    raise ValueError("cannot find a free short name for %r" % name)


def lfn_checksum(n11):
    s = 0
    for b in n11:
        s = (((s >> 1) | ((s & 1) << 7)) + b) & 0xFF
    return s


def lfn_parts_needed(name):
    return (len(name) + 13) // 13


def lfn_entries(long_name, n11):
    """The entries carrying a long name, in the order they go on disk:
    the last part first, each tagged with its place in the sequence."""
    chars = [ord(c) if ord(c) < 0x10000 else ord("_") for c in long_name]
    chars.append(0)
    while len(chars) % 13:
        chars.append(0xFFFF)
    parts = len(chars) // 13
    csum = lfn_checksum(n11)
    out = []
    for p in range(parts, 0, -1):
        block = chars[(p - 1) * 13:p * 13]
        e = bytearray(32)
        e[0] = p | (0x40 if p == parts else 0)
        e[11] = 0x0F
        e[13] = csum
        for i, off in enumerate((1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)):
            e[off:off + 2] = struct.pack("<H", block[i])
        out.append(bytes(e))
    return out


class SparseImage:
    """A disk image that is only ever held where something was written.

    The volume can be gigabytes, of which a few hundred kilobytes are real
    data; keeping the whole thing in memory would need as much RAM as the
    stick has flash.  Writes are collected and laid into the file at the
    end, leaving the gaps for the filesystem to fill later.
    """

    def __init__(self, size):
        self.size = size
        self.parts = []

    def __setitem__(self, where, data):
        assert isinstance(where, slice) and where.step is None
        data = bytes(data)
        assert where.start + len(data) <= self.size, "write past the end of the image"
        self.parts.append((where.start, data))

    def __len__(self):
        return self.size


def write_image(path, size, parts):
    """Lay the collected writes into a file of the given size."""
    with open(path, "wb") as f:
        f.truncate(size)
        for off, data in parts:
            f.seek(off)
            f.write(data)
        f.truncate(size)


class FatImage:
    def __init__(self, total_sectors, kernel_sectors, floppy, hidden=0):
        self.total = total_sectors
        self.reserved = 1 + kernel_sectors
        self.num_fats = 2
        self.floppy = floppy
        self.hidden = hidden                    # LBA of the volume on the disk
        if floppy:
            self.spt, self.heads, self.media, self.root_entries = 18, 2, 0xF0, 224
        else:
            self.spt, self.heads, self.media, self.root_entries = 63, 255, 0xF8, 512
        self.root_secs = (self.root_entries * 32 + SECTOR - 1) // SECTOR
        self._choose_layout()
        self.image = SparseImage(self.total * SECTOR)
        self.fat = bytearray(self.fat_secs * SECTOR)
        self.fat_set(0, (0xF00 if self.fat12 else 0xFF00) | self.media)
        self.fat_set(1, self.eoc)
        self.next_free = 2
        self.root = bytearray(self.root_secs * SECTOR)
        self.root_used = 0
        self.file_count = 0

    def _choose_layout(self):
        """Pick cluster size / FAT type so the cluster count is legal."""
        for fat12, spcs in ((True, (1, 2, 4, 8)), (False, (1, 2, 4, 8, 16, 32, 64))):
            for spc in spcs:
                fat_secs = 1
                for _ in range(16):  # iterate until the FAT size is stable
                    data = self.total - self.reserved - self.num_fats * fat_secs - self.root_secs
                    clusters = data // spc
                    fat_bytes = (clusters + 2) * (3 if fat12 else 4) // (2 if fat12 else 2)
                    if fat12:
                        fat_bytes = ((clusters + 2) * 3 + 1) // 2
                    else:
                        fat_bytes = (clusters + 2) * 2
                    new = (fat_bytes + SECTOR - 1) // SECTOR
                    if new == fat_secs:
                        break
                    fat_secs = new
                limit = 4084 if fat12 else 65524
                if clusters <= limit:
                    self.fat12, self.spc, self.fat_secs, self.clusters = fat12, spc, fat_secs, clusters
                    self.eoc = 0xFFF if fat12 else 0xFFFF
                    self.fat_start = self.reserved
                    self.root_start = self.fat_start + self.num_fats * fat_secs
                    self.data_start = self.root_start + self.root_secs
                    return
        sys.exit("image too large for FAT12/FAT16")

    # -- FAT entries ------------------------------------------------------- #
    def fat_set(self, n, val):
        if self.fat12:
            off = n + n // 2
            if n & 1:
                self.fat[off] = (self.fat[off] & 0x0F) | ((val << 4) & 0xF0)
                self.fat[off + 1] = (val >> 4) & 0xFF
            else:
                self.fat[off] = val & 0xFF
                self.fat[off + 1] = (self.fat[off + 1] & 0xF0) | ((val >> 8) & 0x0F)
        else:
            struct.pack_into("<H", self.fat, n * 2, val)

    # -- clusters ---------------------------------------------------------- #
    @property
    def cluster_bytes(self):
        return self.spc * SECTOR

    def cluster_offset(self, c):
        return (self.data_start + (c - 2) * self.spc) * SECTOR

    def alloc(self, count):
        if count == 0:
            return 0
        first = self.next_free
        if first + count - 2 > self.clusters:
            sys.exit("image is full - use --size to make a bigger image")
        for i in range(count):
            self.fat_set(first + i, first + i + 1 if i < count - 1 else self.eoc)
        self.next_free += count
        return first

    def write_chain(self, first, data):
        for i in range(0, len(data), self.cluster_bytes):
            c = first + i // self.cluster_bytes
            off = self.cluster_offset(c)
            chunk = data[i:i + self.cluster_bytes]
            self.image[off:off + len(chunk)] = chunk

    def store(self, data):
        n = (len(data) + self.cluster_bytes - 1) // self.cluster_bytes
        first = self.alloc(n)
        self.write_chain(first, data)
        return first

    # -- directories ------------------------------------------------------- #
    @staticmethod
    def entry(name11, attr, cluster, size, ts):
        date, tm = fat_timestamp(ts)
        return struct.pack("<11sBBBHHHHHHHI", name11, attr, 0, 0, tm, date, date,
                           0, tm, date, cluster, size)

    def add_tree(self, host_dir, dirbuf, self_cluster):
        """Add every file/folder of host_dir into the directory buffer."""
        items = sorted(host_dir.iterdir(), key=lambda p: (p.is_dir(), p.name.lower()))
        used = [0]
        if self_cluster:  # subdirectory: "." and ".." come first
            used[0] = 64
        names = set()

        def put(e):
            if used[0] + 32 > len(dirbuf):
                sys.exit(f"directory {host_dir} has too many entries")
            dirbuf[used[0]:used[0] + 32] = e
            used[0] += 32

        for item in items:
            if item.name.startswith("."):
                continue
            if fits_83(item.name):
                n11, long_name = short_name(item.name), None
                if n11 in names:
                    print(f"  ! skipping {item}: 8.3 name collision")
                    continue
            elif len(item.name) > 255:
                print(f"  ! skipping {item}: name too long")
                continue
            else:
                n11, long_name = alias_name(item.name, names), item.name
            names.add(n11)
            if long_name:
                for e in lfn_entries(long_name, n11):
                    put(e)
            ts = item.stat().st_mtime
            if item.is_dir():
                count = 2 + sum(1 + (0 if fits_83(p.name) else lfn_parts_needed(p.name))
                                for p in item.iterdir() if not p.name.startswith("."))
                nclust = max(1, (count * 32 + self.cluster_bytes - 1) // self.cluster_bytes)
                cl = self.alloc(nclust)
                sub = bytearray(nclust * self.cluster_bytes)
                sub[0:32] = self.entry(b".          ", 0x10, cl, 0, ts)
                sub[32:64] = self.entry(b"..         ", 0x10, self_cluster, 0, ts)
                self.add_tree(item, sub, cl)
                self.write_chain(cl, sub)
                put(self.entry(n11, 0x10, cl, 0, ts))
                print(f"  + {item.relative_to(FILES) if FILES in item.parents else item.name}/")
            else:
                data = OVERRIDES.get(item.name) if item.parent == FILES else None
                if data is None:
                    data = item.read_bytes()
                cl = self.store(data)
                put(self.entry(n11, 0x20, cl, len(data), ts))
                self.file_count += 1
                print(f"  + {n11.decode().strip():12s} {len(data):8d} bytes")
        if not self_cluster:
            self.root_used = used[0]
        return used[0]

    def add_root_file(self, name, data, ts=None):
        n11 = short_name(name)
        cl = self.store(data)
        e = self.entry(n11, 0x20, cl, len(data), ts or time.time())
        self.root[self.root_used:self.root_used + 32] = e
        self.root_used += 32
        self.file_count += 1
        print(f"  + {n11.decode().strip():12s} {len(data):8d} bytes")

    # -- assemble ---------------------------------------------------------- #
    def bpb(self, label):
        total16 = self.total if self.total < 65536 else 0
        total32 = 0 if total16 else self.total
        return struct.pack("<8sHBHBHHBHHHII", b"EMBER ", SECTOR, self.spc, self.reserved,
                           self.num_fats, self.root_entries, total16, self.media,
                           self.fat_secs, self.spt, self.heads, self.hidden, total32) + \
            struct.pack("<BBBI11s8s", 0x00 if self.floppy else 0x80, 0, 0x29,
                        int(time.time()) & 0xFFFFFFFF, label.ljust(11).encode()[:11],
                        (b"FAT12   " if self.fat12 else b"FAT16   "))

    def finish(self, boot, kernel, label="EMBER"):
        # boot sector: NASM code with our BPB patched in (bytes 3..61)
        bs = bytearray(boot)
        bpb = self.bpb(label)
        bs[3:3 + len(bpb)] = bpb
        self.image[0:SECTOR] = bs
        # kernel in the reserved sectors
        self.image[SECTOR:SECTOR + len(kernel)] = kernel
        # FATs
        for i in range(self.num_fats):
            off = (self.fat_start + i * self.fat_secs) * SECTOR
            self.image[off:off + len(self.fat)] = self.fat
        # root directory (volume label first)
        vol = self.entry(label.ljust(11).encode()[:11], 0x08, 0, 0, time.time())
        root = vol + bytes(self.root[:self.root_used])
        off = self.root_start * SECTOR
        self.image[off:off + len(root)] = root
        return self.image


PART_START = 2048                       # partition starts 1 MB in (aligned)


def lba_to_chs(lba, heads=255, spt=63):
    """Pack an LBA into the 3-byte CHS form used by partition tables."""
    c, rem = divmod(lba, heads * spt)
    h, s = divmod(rem, spt)
    s += 1
    if c > 1023:
        c, h, s = 1023, heads - 1, spt
    return bytes((h, ((c >> 2) & 0xC0) | s, c & 0xFF))


def make_mbr(code, start, sectors, fat12):
    """MBR code + one active partition entry."""
    entry = bytes((0x80,)) + lba_to_chs(start) + bytes((0x01 if fat12 else 0x06,)) \
        + lba_to_chs(start + sectors - 1) + struct.pack("<II", start, sectors)
    mbr = bytearray(code)
    mbr[446:446 + 16] = entry
    mbr[462:510] = bytes(48)
    mbr[510:512] = b"\x55\xAA"
    return bytes(mbr)


# --------------------------------------------------------------------------- #
def write_build_stamp():
    """So a running system can say which build it is: see tools/stamp.py."""
    subprocess.check_call([sys.executable, str(ROOT / "tools" / "stamp.py")])


def main():
    ap = argparse.ArgumentParser(description="Build the Ember disk image")
    ap.add_argument("--size", type=float, default=0, help="USB image size in MB (default 32)")
    ap.add_argument("--floppy", action="store_true",
                    help="build a 1.44 MB superfloppy image instead of the USB image")
    ap.add_argument("--out", default=str(BUILD / "ember.img"))
    ap.add_argument("--run", action="store_true", help="boot the image in QEMU afterwards")
    ap.add_argument("--hd", action="store_true", help="(kept for compatibility; disk is the default)")
    ap.add_argument("--retro", action="store_true",
                    help="the NanoDOS image for the video: old name, a boot full of "
                         "invented errors, the old desktop complaining at every key")
    args = ap.parse_args()
    defines = []
    if args.retro:
        defines.append("NANODOS")
        OVERRIDES["AUTOEXEC.BAT"] = RETRO_AUTOEXEC
        if args.out == str(BUILD / "ember.img"):
            args.out = str(BUILD / "nanodos.img")

    BUILD.mkdir(exist_ok=True)
    nasm_exe = find_nasm()
    write_build_stamp()

    print("Assembling:")
    boot = assemble(nasm_exe, SRC / "boot.asm", BUILD / "boot.bin", defines=defines)
    kernel = assemble(nasm_exe, SRC / "kernel.asm", BUILD / "kernel.bin", SRC, defines)
    mbr = assemble(nasm_exe, SRC / "mbr.asm", BUILD / "mbr.bin")
    if len(boot) != 512 or len(mbr) != 512:
        sys.exit("boot sector and MBR must be exactly 512 bytes")
    if len(kernel) > 0xE000:
        sys.exit("kernel too large (max 56 KB)")
    programs = []
    for src in sorted(PROGRAMS.glob("*.asm")):
        # foo.asm -> FOO.COM;  foo_exe.asm -> FOO.EXE (hand-built MZ header)
        if src.stem == "retro" and not args.retro:
            continue                            # the dramatisation stays in its own image
        stem = src.stem.upper()
        out = BUILD / (stem[:-4] + ".EXE" if stem.endswith("_EXE") else stem + ".COM")
        programs.append((out.name, assemble(nasm_exe, src, out)))
    # The synthesiser SB.MOD carries: C, compiled and linked flat, included
    # into the module by NASM.  Without a compiler the module is built without
    # it and a game's music stays silent, which is what it did before.
    try:
        sys.path.insert(0, str(ROOT / "tools"))
        import build_opl
        build_opl.build()
    except SystemExit:
        raise
    except Exception as e:
        print(f"  (no synthesiser: {e})")
    for src in sorted(MODULES.glob("*.asm")):
        # resident modules: foo.asm -> FOO.MOD, loaded with LOAD FOO
        out = BUILD / (src.stem.upper() + ".MOD")
        programs.append((out.name, assemble(nasm_exe, src, out,
                                            [MODULES, BUILD])))

    kernel_sectors = (len(kernel) + SECTOR - 1) // SECTOR
    check_kernel_layout(kernel)

    def make_image(path, total_sectors, floppy, quiet=False):
        """Floppy: the FAT volume is the whole image.  Otherwise: MBR partition
        table + a FAT volume starting at sector PART_START (USB-HDD style)."""
        hidden = 0 if floppy else PART_START
        img = FatImage(total_sectors - hidden, kernel_sectors, floppy=floppy, hidden=hidden)
        if not quiet:
            print("Adding files:")
        img.add_tree(FILES, img.root, 0)
        for name, data in programs:
            img.add_root_file(name, data)
        # placeholder the kernel overwrites in place with its boot/sound log
        img.add_root_file("EMBER.LOG", b"(no log written yet)\r\n" + b" " * (16384 - 22))
        volume = img.finish(boot, kernel)
        if floppy:
            size, parts = volume.size, volume.parts
        else:
            base = PART_START * SECTOR
            size = base + volume.size
            parts = [(0, make_mbr(mbr, PART_START, volume.size // SECTOR, img.fat12))]
            parts += [(base + off, data) for off, data in volume.parts]
        write_image(path, size, parts)
        print(f"\nImage: {path}")
        print(f"  {size // 1024} KB, {'FAT12' if img.fat12 else 'FAT16'}, "
              f"{img.spc * SECTOR}-byte clusters, {img.clusters} clusters"
              + ("" if floppy else f", MBR partition at sector {PART_START}"))
        print(f"  boot sector + {kernel_sectors} kernel sectors ({len(kernel)} bytes) reserved, "
              f"{img.file_count} files")

    if args.floppy:
        # 1.44 MB superfloppy layout (emulators, real floppies)
        make_image(args.out, 2880, floppy=True)
    else:
        # MBR-partitioned FAT16 image for USB sticks (default 32 MB)
        size_mb = args.size or 32
        make_image(args.out, int(size_mb * 1024 * 1024) // SECTOR, floppy=False)

    if args.run:
        qemu = find_qemu()
        if not qemu:
            sys.exit("QEMU not found (qemu-system-i386)")
        drive = f"file={args.out},format=raw,if={'floppy' if args.floppy else 'ide'}"
        cmd = [qemu, "-m", "64", "-rtc", "base=localtime", "-drive", drive]
        print("Running:", " ".join(cmd))
        subprocess.run(cmd)


if __name__ == "__main__":
    main()

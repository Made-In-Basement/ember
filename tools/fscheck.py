"""Check a Ember image's FAT structures and dump a directory.

Usage: python tools/fscheck.py <image> [path]
Verifies that every file's cluster chain is in range, terminated and not
shared with another file, then lists the directory.
"""
import struct, sys

img = open(sys.argv[1], 'rb').read()
want = sys.argv[2].upper().strip('\\') if len(sys.argv) > 2 else ''

part = struct.unpack_from('<I', img, 446 + 8)[0] if img[510:512] == b'\x55\xaa' else 0
base = part * 512
b = img[base:]
bps, spc, res = struct.unpack_from('<HBH', b, 11)
nfat, rootents, tot16 = struct.unpack_from('<BHH', b, 16)
fatsecs = struct.unpack_from('<H', b, 22)[0]
tot32 = struct.unpack_from('<I', b, 32)[0]
total = tot16 or tot32
root_start = res + nfat * fatsecs
root_secs = (rootents * 32 + bps - 1) // bps
data_start = root_start + root_secs
clusters = (total - data_start) // spc
fat16 = clusters >= 4085
print('%s: %d sectors, %d bytes/cluster, %d FATs of %d sectors, FAT%d, %d clusters'
      % (sys.argv[1], total, spc * bps, nfat, fatsecs, 16 if fat16 else 12, clusters))


def sec(n, count=1):
    return b[n * bps:(n + count) * bps]


fat = sec(res, fatsecs)


def entry(c):
    if fat16:
        return struct.unpack_from('<H', fat, c * 2)[0]
    o = c + c // 2
    v = struct.unpack_from('<H', fat, o)[0]
    return (v >> 4) if (c & 1) else (v & 0xFFF)


eoc = 0xFFF8 if fat16 else 0xFF8
used = {}
problems = []


def chain(first, name):
    out = []
    c = first
    while 2 <= c < eoc:
        if c - 2 >= clusters:
            problems.append('%s: cluster %d is outside the volume' % (name, c))
            break
        if c in used:
            problems.append('%s: shares cluster %d with %s' % (name, c, used[c]))
            break
        used[c] = name
        out.append(c)
        if len(out) > 100000:
            problems.append('%s: chain does not end' % name)
            break
        c = entry(c)
    else:
        if first and c < 2:
            problems.append('%s: chain ends on a free cluster' % name)
    return out


def read_dir(cluster, path):
    if cluster == 0:
        raw = sec(root_start, root_secs)
    else:
        raw = b''
        for c in chain(cluster, path + '/.'):
            raw += sec(data_start + (c - 2) * spc, spc)
    out = []
    for i in range(0, len(raw), 32):
        e = raw[i:i + 32]
        if not e or e[0] == 0:
            break
        if e[0] == 0xE5 or e[11] & 0x0F == 0x0F or e[11] & 0x08:
            continue
        name = e[0:8].decode('latin-1').rstrip()
        ext = e[8:11].decode('latin-1').rstrip()
        full = name + ('.' + ext if ext else '')
        first, size = struct.unpack_from('<H', e, 26)[0], struct.unpack_from('<I', e, 28)[0]
        out.append((full, e[11], first, size, path + '\\' + full))
    return out


def walk(cluster, path, depth=0):
    for full, attr, first, size, fp in read_dir(cluster, path):
        if attr & 0x10:
            if full not in ('.', '..'):
                if not want or fp.upper().startswith('\\' + want) or want.startswith(full):
                    print('%s%-12s <DIR>' % ('  ' * depth, full))
                walk(first, fp, depth + 1)
            continue
        cl = chain(first, fp)
        need = (size + spc * bps - 1) // (spc * bps)
        if len(cl) < need:
            problems.append('%s: %d bytes needs %d clusters but the chain has %d'
                            % (fp, size, need, len(cl)))
        if not want or want in fp.upper():
            print('%s%-12s %8d bytes  first cluster %5d  chain %d' %
                  ('  ' * depth, full, size, first, len(cl)))


walk(0, '')
free = sum(1 for c in range(2, clusters + 2) if entry(c) == 0)
print('%d clusters in use, %d free' % (len(used), free))
if problems:
    print('PROBLEMS:')
    for p in problems:
        print('  ' + p)
    sys.exit(1)
print('filesystem structures are consistent')

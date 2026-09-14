#!/usr/bin/env python3
"""Build REVERSI.N32, Othello ported from borkit/reversi (root/REVERSI.N32).

nano/reversi/engine.c is the browser game's engine transcribed to C;
nano/reversi/reversi.c is a VGA mode 13h front end for it.  Both are linked
with the runtime in nano/, using Zig's bundled clang/lld as the freestanding
i386 toolchain, like tools/build_doom.py.

    python3 tools/build_reversi.py [--image]

--image also runs build.py, so build/ember.img has the game in its root.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'nano', 'reversi')
OBJ = os.path.join(ROOT, 'build', 'reversi')
OUT = os.path.join(ROOT, 'root', 'REVERSI.N32')

sys.path.insert(0, os.path.join(ROOT, 'tools'))
from build_doom import find_zig, finish_image

CFLAGS = ['-target', 'x86-freestanding', '-O2', '-w', '-ffreestanding',
          '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-fno-pie',
          '-mno-sse', '-mno-sse2', '-mno-mmx',
          '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-nostdinc',
          '-I' + os.path.join(ROOT, 'nano', 'include'), '-I' + SRC]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', action='store_true', help='also rebuild build/ember.img')
    args = ap.parse_args()

    zig = find_zig()
    cflags = list(CFLAGS)
    res = subprocess.run([zig, 'cc', '-target', 'x86-freestanding', '-print-resource-dir'],
                         capture_output=True, text=True)
    if res.returncode == 0 and res.stdout.strip():
        cflags.append('-I' + os.path.join(res.stdout.strip(), 'include'))

    shutil.rmtree(OBJ, ignore_errors=True)
    os.makedirs(OBJ)

    # start.S, sys.c and libc.c are the 32-bit runtime; the rest is the game.
    sources = [os.path.join(ROOT, 'nano', s) for s in ('start.S', 'sys.c', 'libc.c')]
    sources += [os.path.join(SRC, s) for s in ('engine.c', 'reversi.c')]
    objs = []
    for s in sources:
        o = os.path.join(OBJ, os.path.splitext(os.path.basename(s))[0] + '.o')
        r = subprocess.run([zig, 'cc'] + cflags + ['-c', s, '-o', o],
                           capture_output=True, text=True)
        if r.returncode:
            print('---- %s' % os.path.basename(s))
            print(r.stderr[:3000])
            sys.exit('%s failed to compile' % os.path.basename(s))
        objs.append(o)

    elf = os.path.join(OBJ, 'reversi.elf')
    objs.sort(key=lambda p: 0 if p.endswith('start.o') else 1)   # header first
    r = subprocess.run([zig, 'cc', '-target', 'x86-freestanding', '-nostdlib', '-static',
                        '-Wl,-T,' + os.path.join(ROOT, 'nano', 'link.ld'),
                        '-o', elf] + objs, capture_output=True, text=True)
    if r.returncode:
        sys.exit('link failed:\n' + r.stderr[:4000])
    subprocess.check_call([zig, 'objcopy', '-O', 'binary', elf, OUT])

    # objcopy drops trailing zero padding; the loader checks the size against
    # the NX32 header, so pad it back.
    finish_image(OUT)

    if args.image:
        subprocess.check_call([sys.executable, 'build.py'], cwd=ROOT)


if __name__ == '__main__':
    main()

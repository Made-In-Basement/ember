#!/usr/bin/env python3
"""Build REVERSI.N32, Othello ported from borkit/reversi (root/REVERSI.N32).

    python3 tools/build_reversi.py [--image]

nano/reversi/engine.c is the browser game's engine transcribed to C, checked
against game.js move for move (docs/REVERSI.md).  nano/reversi/app.cpp is the
game on the screen: VESA graphics, mouse and keyboard.  Its drawing, platform
and C++ runtime files are the same as Chess's.  C++ is compiled against
libc++'s headers and linked only with the runtime in nano/, using Zig's
bundled clang/lld, with the x87 for floating point and no SSE.

Needs Zig 0.13 and Python 3 with Pillow: the text is drawn at build time from
the DejaVu fonts (tools/mkreversiart.py), downloaded to build/src_dl and
checked against their SHA-256.  --image also runs build.py, so
build/ember.img has the game in its root.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'nano', 'reversi')
BUILD = os.path.join(ROOT, 'build', 'reversi')
OUT = os.path.join(ROOT, 'root', 'REVERSI.N32')
DOWNLOADS = os.environ.get('REVERSI_DOWNLOADS') or os.path.join(ROOT, 'build', 'src_dl')

sys.path.insert(0, os.path.join(ROOT, 'tools'))
from build_doom import find_zig, finish_image

FONT_URL = ('https://github.com/dejavu-fonts/dejavu-fonts/releases/download/'
            'version_2_37/dejavu-fonts-ttf-2.37.tar.bz2')
FONT_SHA256 = 'fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7'

CXXFLAGS = ['-std=c++11', '-O2', '-DNDEBUG', '-fno-exceptions', '-fno-rtti',
            '-fno-threadsafe-statics', '-fno-stack-protector',
            '-fno-asynchronous-unwind-tables', '-fno-unwind-tables',
            '-ffunction-sections', '-fdata-sections', '-w']
# 32-bit x86 with the x87 for floating point: Ember does not enable SSE.
X87 = ['-march=i686', '-mno-sse', '-mno-sse2', '-mno-mmx', '-fno-pic', '-fno-pie']
CFLAGS = ['-target', 'x86-freestanding', '-O2', '-w', '-ffreestanding', '-fno-builtin',
          '-fno-stack-protector', '-fno-asynchronous-unwind-tables',
          '-fno-unwind-tables', '-ffunction-sections', '-fdata-sections',
          '-nostdinc'] + X87

# The search is 5 plies at most; 1 MB leaves the game's frames plenty of room.
STACK_BYTES = 1024 * 1024


def log(msg):
    print('reversi: ' + msg, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch(url, sha, name):
    dest = os.path.join(DOWNLOADS, name)
    if os.path.exists(dest) and sha256(dest) == sha:
        return dest
    os.makedirs(DOWNLOADS, exist_ok=True)
    log('downloading ' + url)
    with urllib.request.urlopen(url) as r, open(dest + '.part', 'wb') as f:
        shutil.copyfileobj(r, f)
    got = sha256(dest + '.part')
    if got != sha:
        sys.exit('%s: SHA-256 %s, expected %s' % (url, got, sha))
    os.replace(dest + '.part', dest)
    return dest


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(' '.join(cmd))
        print(r.stderr[-6000:])
        sys.exit('command failed')
    return r.stdout


def compile_all(jobs):
    failed = False
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        for (cmd, out), r in zip(jobs, ex.map(lambda j: subprocess.run(j[0], capture_output=True, text=True), jobs)):
            if r.returncode:
                failed = True
                print('---- ' + os.path.basename(out))
                print(r.stderr[-5000:])
    if failed:
        sys.exit('compile failed')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', action='store_true', help='also rebuild build/ember.img')
    args = ap.parse_args()
    zig = find_zig()

    # The fonts, drawn into build/reversi/gen/assets.h.
    font_tar = fetch(FONT_URL, FONT_SHA256, 'dejavu-fonts-ttf-2.37.tar.bz2')
    fonts = os.path.join(BUILD, 'fonts')
    if not os.path.exists(os.path.join(fonts, 'dejavu-fonts-ttf-2.37')):
        shutil.rmtree(fonts, ignore_errors=True)
        with tarfile.open(font_tar, 'r:bz2') as t:
            t.extractall(fonts)
    gen = os.path.join(BUILD, 'gen')
    os.makedirs(gen, exist_ok=True)
    import mkreversiart
    mkreversiart.generate(os.path.join(fonts, 'dejavu-fonts-ttf-2.37', 'ttf'), gen)

    obj = os.path.join(BUILD, 'obj')
    shutil.rmtree(obj, ignore_errors=True)
    os.makedirs(obj)
    jobs, objs = [], []

    def add(cmd, out):
        jobs.append((cmd, out))
        objs.append(out)

    # C++ is compiled for 32-bit Linux with musl only to get libc++'s and the
    # C library's headers; nothing from either library is linked.  The calls
    # land in Ember's runtime (malloc, memcpy, ...) and nano/reversi/rt.cpp.
    cxx = [zig, 'c++', '-target', 'x86-linux-musl'] + X87 + CXXFLAGS + [
        '-DEMBER', '-I' + SRC, '-I' + gen,
        '-I' + os.path.join(ROOT, 'nano', 'include'), '-I' + os.path.join(ROOT, 'nano', 'gui')]
    for name in ['app', 'gfx', 'platform_ember', 'rt']:
        o = os.path.join(obj, name + '.o')
        add(cxx + ['-c', os.path.join(SRC, name + '.cpp'), '-o', o], o)

    # The engine, Ember's runtime, and the desktop's mouse and touchpad
    # drivers, in C.  start.S is copied so the program's header can ask for a
    # bigger stack.
    start = open(os.path.join(ROOT, 'nano', 'start.S')).read()
    start, n = re.subn(r'\.long\s+65536(\s+/\* stack size \*/)', '.long   %d\\1' % STACK_BYTES, start)
    if n != 1:
        sys.exit('start.S: stack size field not found')
    start_s = os.path.join(obj, 'start.S')
    open(start_s, 'w').write(start)
    res = run([zig, 'cc', '-target', 'x86-freestanding', '-print-resource-dir']).strip()
    cinc = ['-I' + os.path.join(ROOT, 'nano', 'include'), '-I' + os.path.join(res, 'include'),
            '-I' + os.path.join(ROOT, 'nano', 'gui'), '-I' + SRC]
    for s in [start_s, os.path.join(ROOT, 'nano', 'sys.c'), os.path.join(ROOT, 'nano', 'libc.c'),
              os.path.join(ROOT, 'nano', 'gui', 'input.c'), os.path.join(ROOT, 'nano', 'gui', 'touch.c'),
              os.path.join(SRC, 'engine.c')]:
        o = os.path.join(obj, 'c_' + os.path.splitext(os.path.basename(s))[0] + '.o')
        add([zig, 'cc'] + CFLAGS + cinc + ['-c', s, '-o', o], o)

    log('compiling %d files' % len(jobs))
    compile_all(jobs)

    # The linker script, plus the table of global constructors C++ needs.
    ld = open(os.path.join(ROOT, 'nano', 'link.ld')).read()
    ld, n = re.subn(r'(\.data\s*:\s*\{[^}]*\})',
                    r'\1\n    .init_array : { __init_array_start = .; '
                    r'KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array)) '
                    r'__init_array_end = .; }', ld)
    if n != 1:
        sys.exit('link.ld: .data section not found')
    ld_path = os.path.join(obj, 'reversi.ld')
    open(ld_path, 'w').write(ld)

    objs.sort(key=lambda p: 0 if p.endswith('c_start.o') else 1)      # header first
    elf = os.path.join(obj, 'reversi.elf')
    # X87 on the link line too: Zig links its own compiler runtime and builds
    # it for the CPU named here.  Without it that runtime uses SSE and faults.
    # -z norelro keeps lld from asking .init_array to sit with RELRO sections,
    # which a flat image has no use for.
    r = subprocess.run([zig, 'cc', '-target', 'x86-freestanding'] + X87
                       + ['-nostdlib', '-static', '-Wl,-z,norelro', '-Wl,-T,' + ld_path,
                          '-Wl,--gc-sections', '-o', elf] + objs, capture_output=True, text=True)
    if r.returncode:
        sys.exit('link failed:\n' + r.stderr[:6000])
    run([zig, 'objcopy', '-O', 'binary', elf, OUT])
    # objcopy drops trailing zero padding; the loader checks the size against
    # the NX32 header, so pad it back.
    finish_image(OUT)
    log('%s: %d bytes' % (os.path.relpath(OUT, ROOT), os.path.getsize(OUT)))

    if args.image:
        subprocess.check_call([sys.executable, 'build.py'], cwd=ROOT)


if __name__ == '__main__':
    main()

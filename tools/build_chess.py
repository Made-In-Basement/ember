#!/usr/bin/env python3
"""Build CHESS.N32 (root/CHESS.N32): chess against Stockfish 11.

    python3 tools/build_chess.py [--image]

Stockfish is GPLv3 and its source is not kept in this tree, as with Doom.
The build downloads the sf_11 release to build/src_dl (checked against its
SHA-256), unpacks a fresh copy, applies the changes Ember needs (see
apply_patches() and nano/chess/port/), and compiles it with Zig's clang for
Ember's 32-bit runtime.  nano/chess/ is the game: board, input, rules, and
the replacements for the parts of Stockfish Ember cannot run.  Everything in
nano/chess/ is GPLv3 too (nano/chess/COPYING).

Needs Zig 0.13 and Python 3 with Pillow: the pieces and text are drawn at
build time from the DejaVu fonts (tools/mkchessart.py), also downloaded and
checked.  --image also runs build.py, so build/ember.img has the game.

The tests that compare this build with the untouched Stockfish 11 (bench,
perft, 1500 games refereed by python-chess, scripted games in QEMU) live in
ember-contrib, with a Linux build of the same code: docs/CHESS.md.
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(ROOT, 'nano', 'chess')
PORT = os.path.join(GAME, 'port')
BUILD = os.path.join(ROOT, 'build', 'chess')
DOWNLOADS = os.environ.get('CHESS_DOWNLOADS') or os.path.join(ROOT, 'build', 'src_dl')

sys.path.insert(0, os.path.join(ROOT, 'tools'))
from build_doom import find_zig, finish_image

SF_URL = 'https://github.com/official-stockfish/Stockfish/archive/refs/tags/sf_11.tar.gz'
SF_SHA256 = '802261cc601b67bed00c0ef7d21e2125959630f0852a06db9fc9bd74f440b199'
SF_DIR = 'Stockfish-sf_11'

FONT_URL = ('https://github.com/dejavu-fonts/dejavu-fonts/releases/download/'
            'version_2_37/dejavu-fonts-ttf-2.37.tar.bz2')
FONT_SHA256 = 'fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7'

# Stockfish's own files that are compiled unchanged, or with the patches below.
SF_SOURCES = ['bitbase', 'bitboard', 'endgame', 'evaluate', 'material', 'movegen',
              'movepick', 'pawns', 'position', 'psqt', 'search', 'timeman', 'tt']
# Stockfish files replaced by the ones in nano/chess/port/.
PORT_SOURCES = ['benchmark', 'misc', 'thread', 'uci', 'ucioption', 'tbprobe']
GAME_SOURCES = ['app', 'game', 'gfx', 'main', 'platform_ember']

CXXFLAGS = ['-std=c++11', '-O2', '-DNDEBUG', '-fno-exceptions', '-fno-rtti',
            '-fno-threadsafe-statics', '-fno-stack-protector',
            '-fno-asynchronous-unwind-tables', '-fno-unwind-tables',
            '-ffunction-sections', '-fdata-sections', '-w']
# 32-bit x86 with the x87 for floating point: Ember does not enable SSE.
X87 = ['-march=i686', '-mno-sse', '-mno-sse2', '-mno-mmx', '-fno-pic', '-fno-pie']
CFLAGS_EMBER = ['-target', 'x86-freestanding', '-O2', '-w', '-ffreestanding', '-fno-builtin',
                '-fno-stack-protector', '-fno-asynchronous-unwind-tables',
                '-fno-unwind-tables', '-ffunction-sections', '-fdata-sections',
                '-nostdinc'] + X87

# The search needs more stack than the default 64 KB: each ply is a frame of
# a few kilobytes, and a search may go 246 plies deep.
STACK_BYTES = 8 * 1024 * 1024


def log(msg):
    print('chess: ' + msg, flush=True)


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


def unpack(archive, into, mode):
    shutil.rmtree(into, ignore_errors=True)
    os.makedirs(into)
    with tarfile.open(archive, mode) as t:
        t.extractall(into)


# ------------------------------------------------------------------ patches
#
# Each patch replaces exact text and fails the build if that text is not
# found the expected number of times, so a different Stockfish source cannot
# be patched silently into something else.

def patch(path, old, new, count=1):
    s = open(path, encoding='utf-8', newline='').read()
    n = s.count(old)
    if n != count:
        sys.exit('patch %s: expected %d of %r, found %d' % (path, count, old[:60], n))
    open(path, 'w', encoding='utf-8', newline='').write(s.replace(old, new))


STREAM_NAMES = ('istringstream|ostringstream|stringstream|ostream|istream|cout|cerr|endl|'
                'getline|skipws|noskipws|setw|setfill|setprecision|fixed|showpoint|'
                'noshowpoint|showpos|noshowpos|dec|hex|uppercase|left|right|flush')


def use_estd(path):
    """Point the file's stream use at nano/chess/port/estd.h."""
    s = open(path, encoding='utf-8', newline='').read()
    s = re.sub(r'#include <(iostream|sstream|fstream|istream|ostream|iomanip)>',
               '#include "estd.h"', s)
    s, m = re.subn(r'\bstd::(%s)\b' % STREAM_NAMES, r'estd::\1', s)
    if m and '#include "estd.h"' not in s:
        s = s.replace('#include "', '#include "estd.h"\n#include "', 1)
    open(path, 'w', encoding='utf-8', newline='').write(s)


def use_sfmath(path):
    s = open(path, encoding='utf-8', newline='').read()
    s, n = re.subn(r'\bstd::(log|exp|pow)\(', r'sf_\1(', s)
    if n:
        s = s.replace('#include "', '#include "sfmath.h"\n#include "', 1)
    open(path, 'w', encoding='utf-8', newline='').write(s)


def apply_patches(src):
    j = lambda *p: os.path.join(src, *p)

    for f in ['misc.h', 'position.h', 'position.cpp', 'search.cpp', 'evaluate.cpp',
              'evaluate.h', 'tt.cpp', 'uci.h', 'syzygy/tbprobe.h', 'bitboard.h',
              'bitboard.cpp', 'types.h', 'endgame.cpp', 'thread.h', 'search.h', 'timeman.cpp']:
        use_estd(j(f))
    for f in ['search.cpp', 'timeman.cpp']:
        use_sfmath(j(f))

    # misc.h: time from Ember's clock rather than <chrono>; the console is estd's.
    patch(j('misc.h'), 'typedef std::chrono::milliseconds::rep TimePoint; // A value in milliseconds',
          'typedef int64_t TimePoint; // A value in milliseconds\n'
          'int64_t ember_now_ms();    // nano/chess: the platform clock')
    patch(j('misc.h'), '''inline TimePoint now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>
        (std::chrono::steady_clock::now().time_since_epoch()).count();
}''', 'inline TimePoint now() { return ember_now_ms(); }')
    patch(j('misc.h'), 'estd::ostream& operator<<(estd::ostream&, SyncCout);\n', '')
    patch(j('misc.h'), '#define sync_cout estd::cout << IO_LOCK', '#define sync_cout estd::cout')
    patch(j('misc.h'), '#define sync_endl estd::endl << IO_UNLOCK', '#define sync_endl estd::endl')

    # thread.h: no OS threads, so no mutex, condition variable or thread handle.
    patch(j('thread.h'), '#include <condition_variable>\n#include <mutex>\n#include <thread>\n', '')
    patch(j('thread.h'), '#include "thread_win32_osx.h"\n', '')
    patch(j('thread.h'), '  std::mutex mutex;\n  std::condition_variable cv;\n', '')
    patch(j('thread.h'), '  NativeThread stdThread;\n', '')

    # tt.cpp: clear the table in one pass instead of one thread per slice.
    patch(j('tt.cpp'), '#include <thread>\n', '')
    patch(j('tt.cpp'), '''  std::vector<std::thread> threads;

  for (size_t idx = 0; idx < Options["Threads"]; ++idx)
  {
      threads.emplace_back([this, idx]() {

          // Thread binding gives faster search on systems with a first-touch policy
          if (Options["Threads"] > 8)
              WinProcGroup::bindThisThread(idx);

          // Each thread will zero its part of the hash table
          const size_t stride = clusterCount / Options["Threads"],
                       start  = stride * idx,
                       len    = idx != Options["Threads"] - 1 ?
                                stride : clusterCount - start;

          std::memset(&table[start], 0, len * sizeof(Cluster));
      });
  }

  for (std::thread& th: threads)
      th.join();''', '  std::memset(table, 0, clusterCount * sizeof(Cluster));')

    # search.cpp: let the game see the search every 1024 nodes - to keep the
    # screen alive and to stop the search when the player asks.
    patch(j('search.cpp'), '''  callsCnt = Limits.nodes ? std::min(1024, int(Limits.nodes / 1024)) : 1024;
''', '''  callsCnt = Limits.nodes ? std::min(1024, int(Limits.nodes / 1024)) : 1024;

  ember_search_poll(); // nano/chess: the game's look at a running search
''')
    patch(j('search.cpp'), 'void MainThread::check_time() {',
          'void ember_search_poll();\n\nvoid MainThread::check_time() {')

    # search.cpp: "is anything written yet" without the stream buffer.
    patch(j('search.cpp'), 'if (ss.rdbuf()->in_avail()) // Not at first line',
          'if (!ss.str().empty()) // Not at first line')

    # uci.h: the game hands Stockfish commands as strings.
    patch(j('uci.h'), 'void loop(int argc, char* argv[]);',
          'void loop(int argc, char* argv[]);\nvoid execute(const std::string& cmd);')


# ------------------------------------------------------------------ build

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(' '.join(cmd))
        print(r.stderr[-6000:])
        sys.exit('command failed')
    return r.stdout


def compile_all(jobs):
    from concurrent.futures import ThreadPoolExecutor
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
    import json
    zig_lib = json.loads(run([zig, 'env']))['lib_dir']

    # Fresh, patched Stockfish, and the artwork.
    sf_tar = fetch(SF_URL, SF_SHA256, 'sf_11.tar.gz')
    font_tar = fetch(FONT_URL, FONT_SHA256, 'dejavu-fonts-ttf-2.37.tar.bz2')
    unpack(sf_tar, os.path.join(BUILD, 'sf'), 'r:gz')
    src = os.path.join(BUILD, 'sf', SF_DIR, 'src')
    apply_patches(src)
    log('patched Stockfish 11 in ' + src)
    fonts = os.path.join(BUILD, 'fonts')
    if not os.path.exists(os.path.join(fonts, 'dejavu-fonts-ttf-2.37')):
        unpack(font_tar, fonts, 'r:bz2')
    gen = os.path.join(BUILD, 'gen')
    os.makedirs(gen, exist_ok=True)
    import mkchessart
    mkchessart.generate(os.path.join(fonts, 'dejavu-fonts-ttf-2.37', 'ttf'), gen)

    obj = os.path.join(BUILD, 'obj')
    shutil.rmtree(obj, ignore_errors=True)
    os.makedirs(obj)
    jobs, objs = [], []

    def add(cmd, out):
        jobs.append((cmd, out))
        objs.append(out)

    # C++ is compiled for 32-bit Linux with musl only to get libc++'s and the
    # C library's headers; nothing from either library is linked.  The calls
    # land in Ember's runtime (malloc, memcpy, ...) and nano/chess/port/rt.cpp.
    cxx = [zig, 'c++', '-target', 'x86-linux-musl'] + X87 + CXXFLAGS
    for name in SF_SOURCES:
        add(cxx + ['-I' + PORT, '-I' + src, '-c', os.path.join(src, name + '.cpp'),
                   '-o', os.path.join(obj, 'sf_' + name + '.o')], os.path.join(obj, 'sf_' + name + '.o'))
    for name in PORT_SOURCES + ['rt']:
        extra = ['-I' + os.path.join(src, 'syzygy')] if name == 'tbprobe' else []
        o = os.path.join(obj, 'port_' + name + '.o')
        add(cxx + extra + ['-I' + PORT, '-I' + src, '-c', os.path.join(PORT, name + '.cpp'), '-o', o], o)
    o = os.path.join(obj, 'port_sflog.o')
    add([zig, 'cc', '-target', 'x86-linux-musl'] + X87 + ['-O2', '-w', '-c', os.path.join(PORT, 'sflog.c'), '-o', o], o)
    for name in GAME_SOURCES:
        o = os.path.join(obj, 'game_' + name + '.o')
        add(cxx + ['-DEMBER', '-I' + os.path.join(ROOT, 'nano', 'include'),
                   '-I' + os.path.join(ROOT, 'nano', 'gui'), '-I' + GAME, '-I' + gen,
                   '-I' + PORT, '-I' + src, '-c', os.path.join(GAME, name + '.cpp'), '-o', o], o)

    # exp() and pow() from musl's C sources, renamed sf_exp and sf_pow.
    m = os.path.join(zig_lib, 'libc', 'musl')
    minc = ['-I' + os.path.join(m, 'src', 'include'), '-I' + os.path.join(m, 'src', 'internal'),
            '-I' + os.path.join(m, 'arch', 'i386'), '-I' + os.path.join(m, 'arch', 'generic'),
            '-I' + os.path.join(m, 'include')]
    ren = ['-Dexp=sf_exp', '-Dpow=sf_pow', '-D__exp_data=sf__exp_data',
           '-D__pow_log_data=sf__pow_log_data', '-D__math_oflow=sf__math_oflow',
           '-D__math_uflow=sf__math_uflow', '-D__math_xflow=sf__math_xflow',
           '-D__math_invalid=sf__math_invalid', '-D__math_divzero=sf__math_divzero']
    for n in ['exp', 'exp_data', 'pow', 'pow_data', '__math_oflow', '__math_uflow',
              '__math_xflow', '__math_invalid', '__math_divzero']:
        o = os.path.join(obj, 'musl_' + n + '.o')
        add([zig, 'cc', '-target', 'x86-linux-musl', '-O2', '-w', '-std=c99', '-ffreestanding',
             '-fno-builtin', '-nostdinc', '-D_XOPEN_SOURCE=700'] + X87 + minc + ren
            + ['-c', os.path.join(m, 'src', 'math', n + '.c'), '-o', o], o)

    # Ember's runtime, and the desktop's mouse and touchpad drivers.  start.S
    # is copied so the program's header can ask for a bigger stack.
    start = open(os.path.join(ROOT, 'nano', 'start.S')).read()
    start, n = re.subn(r'\.long\s+65536(\s+/\* stack size \*/)', '.long   %d\\1' % STACK_BYTES, start)
    if n != 1:
        sys.exit('start.S: stack size field not found')
    start_s = os.path.join(obj, 'start.S')
    open(start_s, 'w').write(start)
    res = run([zig, 'cc', '-target', 'x86-freestanding', '-print-resource-dir']).strip()
    cinc = ['-I' + os.path.join(ROOT, 'nano', 'include'), '-I' + os.path.join(res, 'include'),
            '-I' + os.path.join(ROOT, 'nano', 'gui')]
    for s in [start_s, os.path.join(ROOT, 'nano', 'sys.c'), os.path.join(ROOT, 'nano', 'libc.c'),
              os.path.join(ROOT, 'nano', 'gui', 'input.c'), os.path.join(ROOT, 'nano', 'gui', 'touch.c')]:
        o = os.path.join(obj, 'rt_' + os.path.splitext(os.path.basename(s))[0] + '.o')
        add([zig, 'cc'] + CFLAGS_EMBER + cinc + ['-c', s, '-o', o], o)

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
    ld_path = os.path.join(obj, 'chess.ld')
    open(ld_path, 'w').write(ld)

    objs.sort(key=lambda p: 0 if p.endswith('rt_start.o') else 1)     # header first
    elf = os.path.join(obj, 'chess.elf')
    # X87 on the link line too: Zig links its own compiler runtime (exp, 64-bit
    # conversions) and builds it for the CPU named here.  Without it that
    # runtime uses SSE, and the first call faults with an invalid opcode.
    r = subprocess.run([zig, 'cc', '-target', 'x86-freestanding'] + X87
                       + ['-nostdlib', '-static', '-Wl,-T,' + ld_path, '-Wl,--gc-sections',
                          '-o', elf] + objs, capture_output=True, text=True)
    if r.returncode:
        sys.exit('link failed:\n' + r.stderr[:8000])
    out = os.path.join(ROOT, 'root', 'CHESS.N32')
    run([zig, 'objcopy', '-O', 'binary', elf, out])
    finish_image(out)

    if args.image:
        subprocess.check_call([sys.executable, 'build.py'], cwd=ROOT)


if __name__ == '__main__':
    main()

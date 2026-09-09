"""Build the native Ember Doom port (root/DOOM/NDOOM.N32).

Uses id Software's GPL linuxdoom-1.10 sources (downloaded to
build/src_dl/DOOM-master) with the platform layer in nano/doom, the runtime
in nano/, and Zig's bundled clang/lld as the freestanding i386 toolchain.
"""
import glob, os, re, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'build', 'src_dl', 'DOOM-master', 'linuxdoom-1.10')
WORK = os.path.join(ROOT, 'build', 'doom')
OBJ = os.path.join(WORK, 'obj')
INC = os.path.join(WORK, 'inc')
OUT = os.path.join(ROOT, 'root', 'DOOM', 'NDOOM.N32')

STD_HEADERS = ['stdio.h', 'stdlib.h', 'string.h', 'strings.h', 'ctype.h', 'errno.h',
               'math.h', 'values.h', 'alloca.h', 'malloc.h', 'fcntl.h', 'unistd.h',
               'signal.h', 'sys/types.h', 'sys/stat.h', 'sys/time.h', 'sys/ioctl.h',
               'sys/socket.h', 'sys/filio.h', 'netinet/in.h', 'netdb.h', 'sys/mman.h',
               'sys/wait.h', 'linux/soundcard.h', 'sys/select.h']


def find_zig():
    for c in [os.environ.get('ZIG', '')] + glob.glob(os.path.join(
            os.environ.get('LOCALAPPDATA', ''), 'Microsoft', 'WinGet', 'Packages',
            'zig.zig_*', 'zig-*', 'zig.exe')):
        if c and os.path.exists(c):
            return c
    return 'zig'


def patch(path, old, new, count=1, regex=False):
    s = open(path, encoding='latin-1').read()
    if regex:
        s2, n = re.subn(old, new, s, count=count, flags=re.S)
    else:
        n = s.count(old)
        s2 = s.replace(old, new)
    if n == 0:
        sys.exit('patch failed: %s: %r' % (os.path.basename(path), old[:50]))
    open(path, 'w', encoding='latin-1').write(s2)


def prepare_sources():
    if os.path.exists(WORK):
        shutil.rmtree(WORK)
    os.makedirs(OBJ)
    for h in STD_HEADERS:
        p = os.path.join(INC, h)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'w').write('#include <nanolibc.h>\n')
    src = os.path.join(WORK, 'src')
    shutil.copytree(SRC, src)
    for f in ['i_main.c', 'i_net.c', 'i_sound.c', 'i_system.c', 'i_video.c']:
        os.remove(os.path.join(src, f))
    # no sound server process: we mix in-process
    patch(os.path.join(src, 'doomdef.h'), '#define SNDSERV  1', '#undef SNDSERV')
    # configuration: plain file in the current directory, WADs likewise
    patch(os.path.join(src, 'd_main.c'),
          r'home = getenv\("HOME"\);\s*if \(!home\)\s*I_Error\("Please set \$HOME to your home directory"\);\s*sprintf\(basedefault, "%s/.doomrc", home\);',
          'strcpy(basedefault, "ndoom.cfg");', regex=True)
    patch(os.path.join(src, 'd_main.c'), 'doomwaddir = ".";', 'doomwaddir = "";')
    # Which game the data is, decided by what is in it rather than by the
    # file's name: "The Ultimate DOOM" as sold today is DOOM.WAD, which the
    # original code took for the older registered game and then asked for a
    # screen (HELP2) that only the older one has.
    patch(os.path.join(src, 'd_main.c'),
          '    W_InitMultipleFiles (wadfiles);\n',
          '    W_InitMultipleFiles (wadfiles);\n'
          '    if (W_CheckNumForName("MAP01") >= 0) gamemode = commercial;\n'
          '    else if (W_CheckNumForName("E4M1") >= 0) gamemode = retail;\n'
          '    else if (W_CheckNumForName("E2M1") >= 0) gamemode = registered;\n'
          '    else gamemode = shareware;\n')
    # and "-iwad FILE" to say which one, when several are in the folder
    patch(os.path.join(src, 'd_main.c'),
          '    strcpy(basedefault, "ndoom.cfg");\n#endif\n',
          '    strcpy(basedefault, "ndoom.cfg");\n#endif\n'
          '    {\n'
          '        int p = M_CheckParm("-iwad");\n'
          '        if (p && p < myargc - 1) {\n'
          '            gamemode = registered;      /* settled by the lumps once loaded */\n'
          '            D_AddFile(myargv[p + 1]);\n'
          '            return;\n'
          '        }\n'
          '    }\n')
    patch(os.path.join(src, 'd_main.c'), '"%s/', '"%s', count=0)
    # file length without fstat
    patch(os.path.join(src, 'w_wad.c'),
          r'int filelength \(int handle\)\s*\{.*?\n\}',
          'int filelength (int handle)\n{\n    long cur = lseek(handle, 0, SEEK_CUR);\n'
          '    long len = lseek(handle, 0, SEEK_END);\n    lseek(handle, cur, SEEK_SET);\n    return len;\n}',
          regex=True)
    # defaults file: a hand parser instead of fscanf
    patch(os.path.join(src, 'm_misc.c'),
          'if (fscanf (f, "%79s %[^\\n]\\n", def, strparm) == 2)',
          'if (M_ReadDefaultLine (f, def, strparm))')
    patch(os.path.join(src, 'm_misc.c'), 'void M_LoadDefaults (void)',
          'static int M_ReadDefaultLine (FILE* f, char* def, char* strparm)\n{\n'
          '    char line[256]; char *p, *q; int n = 0;\n'
          '    if (!fgets(line, sizeof line, f)) return 0;\n'
          '    p = line; while (*p == \' \' || *p == \'\\t\') p++;\n'
          '    while (*p && *p != \' \' && *p != \'\\t\' && *p != \'\\n\' && *p != \'\\r\' && n < 79) def[n++] = *p++;\n'
          '    def[n] = 0; if (!n) return 0;\n'
          '    while (*p == \' \' || *p == \'\\t\') p++;\n'
          '    q = strparm; while (*p && *p != \'\\n\' && *p != \'\\r\') *q++ = *p++; *q = 0;\n'
          '    return q != strparm;\n}\n\nvoid M_LoadDefaults (void)')
    # pitch table for the mixer: 2^((i-128)/64) in 16.16
    with open(os.path.join(INC, 'steptab.h'), 'w') as f:
        f.write('static const unsigned steptable[256] = {\n')
        for i in range(256):
            f.write('%d,' % int(round(2.0 ** ((i - 128) / 64.0) * 65536)))
            if i % 8 == 7:
                f.write('\n')
        f.write('};\n')
    return src


def main():
    zig = find_zig()
    src = prepare_sources()
    cflags = ['-target', 'x86-freestanding', '-O2', '-std=gnu89', '-w', '-ffreestanding',
              '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-fno-pie', '-mno-sse',
              '-mno-sse2', '-mno-mmx', '-fno-strict-aliasing',
              '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-nostdinc',
              '-DNORMALUNIX', '-DLINUX', '-I' + INC, '-I' + os.path.join(ROOT, 'nano', 'include'),
              '-I' + os.path.join(ROOT, 'nano', 'opl'), '-I' + src]
    # clang's own builtin headers (stddef.h, stdint.h, stdarg.h, limits.h)
    res = subprocess.run([zig, 'cc', '-target', 'x86-freestanding', '-print-resource-dir'],
                         capture_output=True, text=True)
    if res.returncode == 0 and res.stdout.strip():
        cflags.append('-I' + os.path.join(res.stdout.strip(), 'include'))
    sources = sorted(glob.glob(os.path.join(src, '*.c')))
    sources += [s for s in sorted(glob.glob(os.path.join(ROOT, 'nano', '*.c')))
                if not s.endswith('hello.c')]
    sources += sorted(glob.glob(os.path.join(ROOT, 'nano', 'doom', '*.c')))
    sources += sorted(glob.glob(os.path.join(ROOT, 'nano', 'opl', '*.c')))
    sources += [os.path.join(ROOT, 'nano', 'start.S')]
    objs = []
    failed = 0
    for s in sources:
        o = os.path.join(OBJ, os.path.splitext(os.path.basename(s))[0] + '.o')
        r = subprocess.run([zig, 'cc'] + cflags + ['-c', s, '-o', o], capture_output=True, text=True)
        if r.returncode:
            failed += 1
            print('---- %s' % os.path.basename(s))
            print(r.stderr[:3000])
        objs.append(o)
    if failed:
        sys.exit('%d files failed to compile' % failed)
    elf = os.path.join(WORK, 'ndoom.elf')
    # start.o first so the header section leads the image
    objs.sort(key=lambda p: 0 if p.endswith('start.o') else 1)
    r = subprocess.run([zig, 'cc', '-target', 'x86-freestanding', '-nostdlib', '-static',
                        '-Wl,-T,' + os.path.join(ROOT, 'nano', 'link.ld'), '-Wl,--gc-sections',
                        '-o', elf] + objs, capture_output=True, text=True)
    if r.returncode:
        sys.exit('link failed:\n' + r.stderr[:4000])
    r = subprocess.run([zig, 'objcopy', '-O', 'binary', elf, OUT], capture_output=True, text=True)
    if r.returncode:
        sys.exit('objcopy failed:\n' + r.stderr[:2000])
    finish_image(OUT)
    print('snd_dbg at %08X' % elf_symbol(elf, 'snd_dbg'))


def elf_symbol(path, name):
    """Address of a symbol in an ELF32 file (0 if absent)."""
    import struct
    e = open(path, 'rb').read()
    shoff = struct.unpack_from('<I', e, 32)[0]
    shentsize, shnum = struct.unpack_from('<HH', e, 46)
    secs = [struct.unpack_from('<IIIIIIIIII', e, shoff + i * shentsize) for i in range(shnum)]
    for s in secs:
        if s[1] != 2:
            continue
        strtab = secs[s[6]]
        for j in range(s[5] // 16):
            nm, val = struct.unpack_from('<II', e, s[4] + j * 16)
            end = e.index(b'\0', strtab[4] + nm)
            if e[strtab[4] + nm:end].decode() == name:
                return val
    return 0


def finish_image(out):
    import struct
    data = open(out, 'rb').read()
    assert data[:4] == b'NX32', 'header missing from the image'
    load, entry, dend, bend, stack = struct.unpack_from('<5I', data, 4)
    if dend - load > len(data):                 # objcopy drops trailing padding
        data += bytes(dend - load - len(data))
        open(out, 'wb').write(data)
    print('%s: %d bytes, load %08X entry %08X data_end %08X bss_end %08X stack %d'
          % (os.path.basename(out), len(data), load, entry, dend, bend, stack))
    assert dend - load == len(data), 'image size mismatch: %d vs %d' % (dend - load, len(data))


def build_app(name, src_files, out_name, extra_inc=()):
    """Build one NX32 program from the runtime plus the given sources."""
    zig = find_zig()
    obj = os.path.join(ROOT, 'build', name)
    if os.path.exists(obj):
        shutil.rmtree(obj)
    os.makedirs(obj)
    cflags = ['-target', 'x86-freestanding', '-O2', '-w', '-ffreestanding',
              '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-fno-pie',
              '-mno-sse', '-mno-sse2', '-mno-mmx', '-fno-strict-aliasing',
              '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-nostdinc',
              '-I' + os.path.join(ROOT, 'nano', 'include')]
    for d in extra_inc:
        cflags.append('-I' + d)
    if os.environ.get('VOX_BUILD'):
        cflags.append('-DVOX_BUILD="' + os.environ['VOX_BUILD'] + '"')
    res = subprocess.run([zig, 'cc', '-target', 'x86-freestanding', '-print-resource-dir'],
                         capture_output=True, text=True)
    if res.returncode == 0 and res.stdout.strip():
        cflags.append('-I' + os.path.join(res.stdout.strip(), 'include'))
    sources = [os.path.join(ROOT, 'nano', 'start.S'),
               os.path.join(ROOT, 'nano', 'sys.c'),
               os.path.join(ROOT, 'nano', 'libc.c')] + list(src_files)
    objs = []
    for s in sources:
        o = os.path.join(obj, os.path.splitext(os.path.basename(s))[0] + '.o')
        r = subprocess.run([zig, 'cc'] + cflags + ['-c', s, '-o', o],
                           capture_output=True, text=True)
        if r.returncode:
            print('---- %s' % os.path.basename(s))
            print(r.stderr[:3000])
            sys.exit('%s failed to compile' % os.path.basename(s))
        objs.append(o)
    objs.sort(key=lambda p: 0 if p.endswith('start.o') else 1)
    elf = os.path.join(obj, name + '.elf')
    out = os.path.join(ROOT, 'root', out_name)
    r = subprocess.run([zig, 'cc', '-target', 'x86-freestanding', '-nostdlib', '-static',
                        '-Wl,-T,' + os.path.join(ROOT, 'nano', 'link.ld'),
                        '-o', elf] + objs, capture_output=True, text=True)
    if r.returncode:
        sys.exit('link failed:\n' + r.stderr[:4000])
    subprocess.check_call([zig, 'objcopy', '-O', 'binary', elf, out])
    finish_image(out)


def build_gui():
    import subprocess as sp
    sp.check_call([sys.executable, os.path.join(ROOT, 'tools', 'check_escapes.py')])
    sp.check_call([sys.executable, os.path.join(ROOT, 'tools', 'stamp.py')])
    sp.check_call([sys.executable, os.path.join(ROOT, 'tools', 'mkfont.py')])
    sp.check_call([sys.executable, os.path.join(ROOT, 'tools', 'mktables.py')])
    sp.check_call([sys.executable, os.path.join(ROOT, 'tools', 'mkicons.py')])
    d = os.path.join(ROOT, 'nano', 'gui')
    build_app('gui',
              [os.path.join(ROOT, 'build', 'src_dl', 'picojpeg.c')] +
              [os.path.join(d, f) for f in ('draw.c', 'gpu.c', 'gpu3d.c', 'input.c', 'crystal.c',
                                            'touch.c', 'wall.c', 'menu.c', 'music.c',
                                            'files.c', 'tools.c', 'apps.c', 'monitor.c', 'mtrr.c', 'osk.c', 'write.c', 'power.c', 'clock.c', 'calendar.c', 'notes.c', 'viewer.c', 'shot.c', 'paint.c', 'png.c', 'scene3d.c', 'icons.c',
                                            'shell.c')]
              + [os.path.join(ROOT, 'nano', 'player', 'paudio.c')],
              'EMBER.N32',
              extra_inc=(d, os.path.join(ROOT, 'build'),
                         os.path.join(ROOT, 'nano', 'player'),
                         os.path.join(ROOT, 'build', 'src_dl'),
                         os.path.join(ROOT, 'build', 'playerinc')))


def build_player():
    d = os.path.join(ROOT, 'nano', 'player')
    build_app('player',
              [os.path.join(d, f) for f in ('pgfx.c', 'paudio.c', 'player.c')],
              'PLAYER.N32',
              extra_inc=(d, os.path.join(ROOT, 'build', 'src_dl'),
                         os.path.join(ROOT, 'build', 'playerinc')))


def build_hello():
    """HELLO32.N32: runtime self-test without Doom."""
    zig = find_zig()
    obj = os.path.join(ROOT, 'build', 'hello32')
    os.makedirs(obj, exist_ok=True)
    cflags = ['-target', 'x86-freestanding', '-O2', '-w', '-ffreestanding', '-fno-builtin',
              '-fno-stack-protector', '-fno-pic', '-fno-pie', '-mno-sse', '-mno-sse2', '-mno-mmx',
              '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-nostdinc',
              '-I' + os.path.join(ROOT, 'nano', 'include')]
    res = subprocess.run([zig, 'cc', '-target', 'x86-freestanding', '-print-resource-dir'],
                         capture_output=True, text=True)
    cflags.append('-I' + os.path.join(res.stdout.strip(), 'include'))
    objs = []
    for s in ['start.S', 'sys.c', 'libc.c', 'hello.c']:
        o = os.path.join(obj, os.path.splitext(s)[0] + '.o')
        r = subprocess.run([zig, 'cc'] + cflags + ['-c', os.path.join(ROOT, 'nano', s), '-o', o],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr)
        objs.append(o)
    elf = os.path.join(obj, 'hello32.elf')
    out = os.path.join(ROOT, 'root', 'HELLO32.N32')
    r = subprocess.run([zig, 'cc', '-target', 'x86-freestanding', '-nostdlib', '-static',
                        '-Wl,-T,' + os.path.join(ROOT, 'nano', 'link.ld'), '-o', elf] + objs,
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('link failed:\n' + r.stderr[:4000])
    subprocess.check_call([zig, 'objcopy', '-O', 'binary', elf, out])
    finish_image(out)


def build_gpu():
    """GPU.N32: a probe of the Intel graphics engine."""
    build_app('gpu', [os.path.join(ROOT, 'nano', 'gpu', 'gpu.c')], 'GPU.N32')


def build_render():
    """RENDER.N32: a textured triangle from the 3D engine."""
    build_app('render', [os.path.join(ROOT, 'nano', 'gpu', 'render.c')], 'RENDER.N32')


def build_fly():
    """FLY.N32: a flight through a tunnel, drawn by the 3D engine."""
    build_app('fly', [os.path.join(ROOT, 'nano', 'gpu', 'fly.c')], 'FLY.N32')


def build_voxel():
    """VOXEL.N32: the block world, drawn by the 3D engine.

    game.c is the author's own, unchanged; everything beside it is the
    platform Ember gives it.  The shim headers in nano/voxel/include point
    the C99 sources at nanolibc, the way Doom's build does.
    """
    d = os.path.join(ROOT, 'nano', 'voxel')
    os.environ['VOX_BUILD'] = time.strftime('%H%M%S')
    build_app('voxel',
              [os.path.join(d, 'game.c'),
               os.path.join(d, 'runner.c'),
               os.path.join(d, 'platform_ember.c'),
               os.path.join(d, 'render_gpu.c'),
               os.path.join(ROOT, 'nano', 'gui', 'input.c')],
              'VOXEL.N32',
              extra_inc=(d, os.path.join(d, 'include'), os.path.join(ROOT, 'nano', 'gui')))


if __name__ == '__main__':
    if '--hello' in sys.argv:
        build_hello()
    elif '--gpu' in sys.argv:
        build_gpu()
    elif '--render' in sys.argv:
        build_render()
    elif '--fly' in sys.argv:
        build_fly()
    elif '--voxel' in sys.argv:
        build_voxel()
    elif '--player' in sys.argv:
        build_player()
    elif '--gui' in sys.argv:
        build_gui()
    else:
        main()

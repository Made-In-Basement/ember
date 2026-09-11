#!/usr/bin/env python3
"""Boot Ember 2.0 under real UEFI firmware in QEMU and read what it says.

    python tools/ember2_test.py --keys "dir{ret}" --wait 30 --png build/e2.png

QEMU's EDK II firmware boots build/ember2 as a FAT volume.  The shim mirrors
its console to the serial port when there is one, so the text of the screen
comes back on stdout without a screenshot - screenshots are still there for
what the framebuffer actually looks like.  Keys go in through the monitor, the
same syntax as qemu_test.py.
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from qemu_test import Monitor, type_keys, ppm_to_png    # noqa: E402

QEMU_DIR = Path(r'C:\Program Files\qemu')
CODE = QEMU_DIR / 'share' / 'edk2-x86_64-code.fd'
VARS = QEMU_DIR / 'share' / 'edk2-i386-vars.fd'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default=str(ROOT / 'build' / 'ember2'))
    ap.add_argument('--keys', default='')
    ap.add_argument('--wait', type=float, default=25, help='seconds to boot')
    ap.add_argument('--after', type=float, default=3)
    ap.add_argument('--png', default='')
    ap.add_argument('--port', type=int, default=4520)
    ap.add_argument('--mem', type=int, default=512)
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--qemulog', default='',
                    help="QEMU's own log of every exception, to this file; "
                         "the last lines before a reset say what faulted")
    ap.add_argument('--regs', action='store_true',
                    help='print the registers at the end - where it is')
    ap.add_argument('--pmemsave', default='',
                    help='addr,len,file: dump guest memory at the end')
    args = ap.parse_args()

    tmp = Path(os.environ.get('TEMP', '.')) / 'ember2_test'
    tmp.mkdir(exist_ok=True)
    vars_copy = tmp / 'vars.fd'
    shutil.copyfile(VARS, vars_copy)
    serial = tmp / 'serial.txt'
    if serial.exists():
        serial.unlink()
    cmd = [str(QEMU_DIR / 'qemu-system-x86_64.exe'), '-m', str(args.mem),
           '-display', 'none', '-serial', 'file:' + str(serial),
           '-drive', f'if=pflash,format=raw,readonly=on,file={CODE}',
           '-drive', f'if=pflash,format=raw,file={vars_copy}',
           '-drive', f'file=fat:rw:{args.dir},format=raw',
           '-monitor', f'tcp:127.0.0.1:{args.port},server,nowait']
    if args.qemulog:
        cmd += ['-accel', 'tcg', '-d', 'int,cpu_reset', '-D',
                str(Path(args.qemulog).resolve())]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    try:
        mon = Monitor(args.port)
        time.sleep(args.wait)
        if args.keys:
            type_keys(mon, args.keys)
        time.sleep(args.after)
        if args.regs:
            print(mon.cmd('info registers'))
        if args.pmemsave:
            addr, length, out = args.pmemsave.split(',')
            out = str(Path(out).resolve()).replace('\\', '/')
            print(mon.cmd(f'pmemsave {addr} {length} "{out}"').strip())
        if args.png:
            ppm = tmp / 'shot.ppm'
            mon.cmd(f'screendump "{str(ppm).replace(chr(92), "/")}"')
            time.sleep(0.5)
            w, h = ppm_to_png(ppm, args.png)
            print(f'[screenshot {w}x{h} -> {args.png}]')
        if not args.keep:
            try:
                mon.cmd('quit')             # QEMU drops the socket mid-reply
            except (ConnectionError, OSError):
                pass
            mon.close()
    finally:
        if not args.keep:
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
    if serial.exists():
        text = serial.read_bytes().decode('latin-1')
        # the firmware's own escape sequences are not worth reading
        out = []
        for ch in text:
            out.append(ch if 32 <= ord(ch) < 127 or ch in '\r\n' else '.')
        text = ''.join(out)
        i = text.find('Ember 2.0')
        print(text[i:] if i >= 0 else text[-3000:])


if __name__ == '__main__':
    main()

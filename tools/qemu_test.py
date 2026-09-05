#!/usr/bin/env python3
"""
qemu_test.py - boot Ember in headless QEMU, type keys, and capture the screen.

    python tools/qemu_test.py --keys "dir{ret}"                # disk boot, print text screen
    python tools/qemu_test.py --floppy --keys "ver{ret}"       # floppy boot (CHS path)
    python tools/qemu_test.py --usb --png build/shot.png       # USB mass-storage boot
    python tools/qemu_test.py --keys "win{ret}" --mouse 300,200,click --png build/gui.png

Key text: plain characters are typed; {ret} {esc} {tab} {up} {down} {left} {right}
{f1}.. {bksp} {alt-tab} etc. are QEMU key names in braces; {wait:2} pauses 2 s.
--mouse takes a comma list: dx,dy[,click|press|release|rclick] (relative moves).
The text screen is read straight out of video memory (0xB8000); --png also
saves a real screenshot (works in graphics modes too).
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
QEMU_CANDIDATES = [r"C:\Program Files\qemu\qemu-system-i386.exe", "qemu-system-i386",
                   "qemu-system-x86_64"]

SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
           '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left',
           '}': 'bracket_right', ':': 'semicolon', '"': 'apostrophe', '~': 'grave_accent',
           '|': 'backslash', '<': 'comma', '>': 'dot', '?': 'slash'}
PLAIN = {' ': 'spc', '\n': 'ret', '\t': 'tab', '\x1b': 'esc', '\b': 'backspace',
         '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right',
         ';': 'semicolon', "'": 'apostrophe', '`': 'grave_accent', '\\': 'backslash',
         ',': 'comma', '.': 'dot', '/': 'slash'}


def keyname(ch):
    if ch in PLAIN:
        return PLAIN[ch]
    if ch in SHIFTED:
        return 'shift-' + SHIFTED[ch]
    if ch.isalpha():
        return ('shift-' + ch.lower()) if ch.isupper() else ch
    if ch.isdigit():
        return ch
    raise ValueError(f"no key mapping for {ch!r}")


def find_qemu():
    for cand in QEMU_CANDIDATES:
        if os.path.isabs(cand):
            if os.path.exists(cand):
                return cand
        else:
            from shutil import which
            if which(cand):
                return which(cand)
    sys.exit("QEMU not found")


class Monitor:
    def __init__(self, port, timeout=15):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.sock.settimeout(10)
        self._read_prompt()

    def _read_prompt(self):
        buf = b""
        while not buf.endswith(b"(qemu) "):
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("monitor closed")
            buf += chunk
        return buf.decode("utf-8", "replace")

    def cmd(self, line):
        self.sock.sendall((line + "\n").encode())
        out = self._read_prompt()
        # strip the echoed command line and the prompt
        return out.replace("\r", "").split("\n", 1)[-1].rsplit("(qemu) ", 1)[0]

    def close(self):
        try:
            self.sock.sendall(b"quit\n")
        except OSError:
            pass
        self.sock.close()


def type_keys(mon, text, delay=0.07):
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == '{':
            j = text.index('}', i)
            token = text[i + 1:j]
            i = j + 1
            if token.startswith("wait:"):
                time.sleep(float(token[5:]))
                continue
            mon.cmd(f"sendkey {token} 40")
        else:
            mon.cmd(f"sendkey {keyname(ch)} 40")
            i += 1
        time.sleep(delay)


def mouse_actions(mon, spec):
    """spec: 'dx,dy[,click|press|release|rclick]' segments separated by ';'"""
    for seg in spec.split(';'):
        parts = [p.strip() for p in seg.split(',') if p.strip()]
        if not parts:
            continue
        dx, dy = int(parts[0]), int(parts[1])
        # move in steps so the guest sees a smooth motion
        steps = max(1, max(abs(dx), abs(dy)) // 20)
        for s in range(steps):
            x = dx * (s + 1) // steps - dx * s // steps
            y = dy * (s + 1) // steps - dy * s // steps
            mon.cmd(f"mouse_move {x} {y}")
            time.sleep(0.02)
        time.sleep(0.15)
        action = parts[2] if len(parts) > 2 else ""
        if action == "click":
            mon.cmd("mouse_button 1"); time.sleep(0.12); mon.cmd("mouse_button 0")
        elif action == "dblclick":
            for _ in range(2):
                mon.cmd("mouse_button 1"); time.sleep(0.06)
                mon.cmd("mouse_button 0"); time.sleep(0.08)
        elif action == "rclick":
            mon.cmd("mouse_button 2"); time.sleep(0.12); mon.cmd("mouse_button 0")
        elif action == "press":
            mon.cmd("mouse_button 1")
        elif action == "release":
            mon.cmd("mouse_button 0")
        time.sleep(0.3)


def screen_text(mon, tmp):
    path = str(tmp).replace("\\", "/")
    mon.cmd(f'pmemsave 0xb8000 4000 "{path}"')
    data = Path(tmp).read_bytes()
    lines = []
    for row in range(25):
        cells = data[row * 160:(row + 1) * 160]
        lines.append(cells[0::2].decode("cp437").rstrip())
    return lines


def ppm_to_png(ppm_path, png_path):
    data = Path(ppm_path).read_bytes()
    parts = data.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    pix = parts[4][:w * h * 3]
    raw = b"".join(b"\x00" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + \
            struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    Path(png_path).write_bytes(png)
    return w, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=str(ROOT / "build" / "ember.img"))
    ap.add_argument("--hd", action="store_true", help="boot as an IDE hard disk (default)")
    ap.add_argument("--floppy", action="store_true", help="boot the image as a floppy")
    ap.add_argument("--usb", action="store_true", help="boot as a USB mass-storage device")
    ap.add_argument("--keys", default="", help="keys to type after boot")
    ap.add_argument("--mouse", default="", help="mouse actions after the keys")
    ap.add_argument("--keys2", default="", help="keys to type after the mouse actions")
    ap.add_argument("--wait", type=float, default=2.5, help="seconds to wait for boot")
    ap.add_argument("--after", type=float, default=1.0, help="seconds to wait before capture")
    ap.add_argument("--png", default="", help="save a screenshot PNG")
    ap.add_argument("--port", type=int, default=4488)
    ap.add_argument("--shift", action="store_true", help="hold Shift during boot")
    ap.add_argument("--shift-delay", type=float, default=0.7,
                    help="seconds after start before pressing Shift")
    ap.add_argument("--keep", action="store_true", help="leave QEMU running")
    ap.add_argument("--audio", default="", help="capture guest audio (HDA + speaker) to this WAV")
    ap.add_argument("--no-pcspk", action="store_true",
                    help="do not wire the PC speaker to the capture (HD Audio only)")
    ap.add_argument("--pmemsave", default="", help="addr,len,file: dump guest memory at the end")
    ap.add_argument("--regs", action="store_true",
                    help="stop on triple fault instead of rebooting and print the CPU registers")
    ap.add_argument("--qemulog", default="", help="write QEMU's interrupt/exception log to this file")
    args = ap.parse_args()

    qemu = find_qemu()
    img = str(Path(args.img).resolve())
    cmd = [qemu, "-m", "64", "-display", "none", "-rtc", "base=localtime",
           "-monitor", f"tcp:127.0.0.1:{args.port},server,nowait"]
    if args.regs:
        cmd += ["-no-reboot", "-no-shutdown"]
    if args.qemulog:
        cmd += ["-accel", "tcg", "-d", "int", "-D", str(Path(args.qemulog).resolve())]
    if args.audio:
        wav = str(Path(args.audio).resolve()).replace("\\", "/")
        cmd += ["-audiodev", f"wav,id=snd0,path={wav}",
                "-device", "intel-hda", "-device", "hda-output,audiodev=snd0"]
        if not args.no_pcspk:
            cmd += ["-machine", "pcspk-audiodev=snd0"]
    if args.usb:
        cmd += ["-drive", f"if=none,id=stick,file={img},format=raw",
                "-usb", "-device", "usb-storage,drive=stick,bootindex=0"]
    elif args.floppy:
        cmd += ["-drive", f"file={img},format=raw,if=floppy", "-boot", "order=a"]
    else:
        cmd += ["-drive", f"file={img},format=raw,if=ide"]

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    tmpdir = Path(os.environ.get("TEMP", ".")) / "ember_test"
    tmpdir.mkdir(exist_ok=True)
    try:
        mon = Monitor(args.port)
        if args.shift:
            # press Shift once the BIOS has initialised the keyboard and hold it
            time.sleep(args.shift_delay)
            mon.cmd("sendkey shift 4000")
        time.sleep(args.wait)
        if args.keys:
            type_keys(mon, args.keys)
        if args.mouse:
            mouse_actions(mon, args.mouse)
        if args.keys2:
            type_keys(mon, args.keys2)
        time.sleep(args.after)
        for line in screen_text(mon, tmpdir / "screen.bin"):
            print(line)
        if args.regs:
            print(mon.cmd("info status").strip())
            print(mon.cmd("info registers"))
        if args.pmemsave:
            addr, length, out = args.pmemsave.split(",")
            out = str(Path(out).resolve()).replace("\\", "/")
            print(mon.cmd(f'pmemsave {addr} {length} "{out}"').strip())
        if args.png:
            ppm = tmpdir / "shot.ppm"
            mon.cmd(f'screendump "{str(ppm).replace(chr(92), "/")}"')
            time.sleep(0.3)
            w, h = ppm_to_png(ppm, args.png)
            print(f"[screenshot {w}x{h} -> {args.png}]")
        if not args.keep:
            mon.close()
    finally:
        if not args.keep:
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
            err = proc.stderr.read().decode(errors="replace").strip()
            if err:
                print("[qemu stderr]", err)


if __name__ == "__main__":
    main()

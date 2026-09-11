#!/usr/bin/env python3
"""The 8x16 font Ember 2.0's stub draws the console with.

There is no BIOS on a UEFI-only machine and so no BIOS font; the shim carries
one.  This renders all 256 code points of code page 437 - the box-drawing
characters included - from a monospaced typeface into one-bit 8x16 glyphs,
which is exactly the shape a VGA text-mode font has and the shape the shim's
drawing loop expects.

    python tools/mkshimfont.py    -> build/shimfont.bin (4096 bytes)

Consolas is the first choice because it is on every Windows machine and has
the box-drawing block; anything monospaced with those glyphs will do.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build')
WIN_FONTS = r'C:\Windows\Fonts'
FACES = ['consola.ttf', 'lucon.ttf', 'cour.ttf', 'DejaVuSansMono.ttf']
SIZE = 14                               # pixels; leaves a row above and below


def find_face():
    for f in FACES:
        for d in (WIN_FONTS, os.path.join(ROOT, 'tools', 'fonts')):
            p = os.path.join(d, f)
            if os.path.exists(p):
                return p
    return None


def build(quiet=False):
    os.makedirs(BUILD, exist_ok=True)
    face = find_face()
    if face:
        font = ImageFont.truetype(face, SIZE)
        asc, desc = font.getmetrics()
        top = max(0, (16 - (asc + desc)) // 2)
    else:
        font = ImageFont.load_default()
        top = 2
    out = bytearray()
    for code in range(256):
        ch = bytes([code]).decode('cp437')
        img = Image.new('L', (8, 16), 0)
        d = ImageDraw.Draw(img)
        if code >= 32:
            # centre glyphs narrower than the cell; clip anything wider
            try:
                w = d.textlength(ch, font=font)
            except AttributeError:
                w = font.getsize(ch)[0]
            x = max(0, int((8 - w) // 2))
            d.text((x, top), ch, font=font, fill=255)
        px = img.load()
        for y in range(16):
            row = 0
            for x in range(8):
                if px[x, y] >= 96:
                    row |= 0x80 >> x
            out.append(row)
    # the cursor and a few things a bitmap font should get exactly right
    out[ord(' ') * 16:(ord(' ') + 1) * 16] = bytes(16)
    out[0xDB * 16:(0xDB + 1) * 16] = bytes([0xFF] * 16)        # full block
    out[0xB0 * 16:(0xB0 + 1) * 16] = bytes([0x88, 0x22] * 8)   # light shade
    out[0xB1 * 16:(0xB1 + 1) * 16] = bytes([0xAA, 0x55] * 8)   # medium
    out[0xB2 * 16:(0xB2 + 1) * 16] = bytes([0xDD, 0x77] * 8)   # dark
    path = os.path.join(BUILD, 'shimfont.bin')
    open(path, 'wb').write(bytes(out))
    if not quiet:
        print('  console font: %s -> build/shimfont.bin'
              % (os.path.basename(face) if face else "PIL's default"))
    return path


if __name__ == '__main__':
    build()

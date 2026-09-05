"""Turn a picture into root/SPLASH.BIN, the boot splash Ember shows.

The picture is fitted inside 800x600 (letterboxed on black), reduced to
256 colours, and stored run-length encoded, which suits the mostly-black
artwork well and keeps the boot-time read short.

File layout:  "NSPL", width, height (16-bit each), 768 bytes of palette
(8-bit R, G, B per entry), then (count, colour) byte pairs until every
pixel is covered.

    python tools/mksplash.py splash_screen.jpeg
"""
import struct
import sys

from PIL import Image

W, H = 800, 600
BLACK_FLOOR = 32


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else 'splash_screen.jpeg'
    out = sys.argv[2] if len(sys.argv) > 2 else 'root/SPLASH.BIN'
    im = Image.open(src).convert('RGB')
    scale = min(W / im.width, H / im.height)
    fitted = im.resize((round(im.width * scale), round(im.height * scale)),
                       Image.LANCZOS)
    canvas = Image.new('RGB', (W, H), (0, 0, 0))
    canvas.paste(fitted, ((W - fitted.width) // 2, (H - fitted.height) // 2))
    # JPEG leaves a faint speckle in the black, which ruins the run lengths:
    # anything nearly black becomes black
    p = canvas.load()
    for y in range(H):
        for x in range(W):
            r, g, b = p[x, y]
            if max(r, g, b) < BLACK_FLOOR:
                p[x, y] = (0, 0, 0)
    pal = canvas.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    palette = pal.getpalette()[:768]
    palette += [0] * (768 - len(palette))
    pixels = pal.tobytes()
    runs = bytearray()
    i, n = 0, len(pixels)
    while i < n:
        c = pixels[i]
        j = i + 1
        while j < n and j - i < 255 and pixels[j] == c:
            j += 1
        runs += bytes((j - i, c))
        i = j
    with open(out, 'wb') as f:
        f.write(b'NSPL' + struct.pack('<HH', W, H) + bytes(palette) + runs)
    print('%s: %dx%d -> %dx%d on %dx%d, %d colours, %d bytes (%d runs)' % (
        out, im.width, im.height, fitted.width, fitted.height, W, H,
        len(set(pixels)), 8 + 768 + len(runs), len(runs) // 2))


if __name__ == '__main__':
    main()

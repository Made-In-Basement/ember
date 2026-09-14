#!/usr/bin/env python3
"""Draw the chess pieces and text fonts into a C++ header.

Called by tools/build_chess.py.  Everything on screen that is not a rectangle or a
circle is drawn here, at build time, from the DejaVu Sans fonts (Bitstream
Vera license, see docs/CHESS.md), and embedded in CHESS.N32 as
antialiased alpha masks.  Ember has no font rasteriser, and does not need
one: the program only ever draws these few sizes.

Pieces use the Unicode chess symbols in DejaVu Sans.  Each piece and size
gets four masks:

  hull    the piece's whole silhouette, filled in
  line    the outline symbol (U+2654..): contour and inner detail lines
  solid   the filled symbol (U+265A..), whose gaps show the detail
  shadow  the hull, blurred and dropped slightly

White pieces are the hull in ivory with the outline in near-black; black
pieces are the hull in grey with the solid symbol in near-black over it, so
the details show as grey lines.
"""
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

PIECE_SIZES = [24, 40, 48, 56, 64, 72, 80, 88, 96, 112, 128]
PIECES = 'PNBRQK'
OUTLINE = {'K': '♔', 'Q': '♕', 'R': '♖', 'B': '♗', 'N': '♘', 'P': '♙'}
SOLID = {'K': '♚', 'Q': '♛', 'R': '♜', 'B': '♝', 'N': '♞', 'P': '♟'}

# name, file, pixel size
FONTS = [
    ('small', 'DejaVuSans.ttf', 12),
    ('body', 'DejaVuSans.ttf', 14),
    ('bold', 'DejaVuSans-Bold.ttf', 14),
    ('coord', 'DejaVuSans-Bold.ttf', 11),
    ('large', 'DejaVuSans-Bold.ttf', 18),
    ('title', 'DejaVuSans-Bold.ttf', 26),
    ('huge', 'DejaVuSans-Bold.ttf', 36),
    ('move', 'DejaVuSans.ttf', 15),
    ('moveb', 'DejaVuSans-Bold.ttf', 15),
]

# Characters 32..126, and a few beyond ASCII given codes 128.. in strings.
EXTRA = ['·', '½', '…', '−', '◀', '▶', '—', '×']

SUPER = 4   # supersampling for the pieces


def c_bytes(data):
    out = []
    for i in range(0, len(data), 24):
        out.append(','.join(str(b) for b in data[i:i + 24]))
    return ',\n'.join(out)


def render_glyph(font, ch, size):
    img = Image.new('L', (size, size), 0)
    ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=255)
    return img


def piece_masks(ttf, piece, size):
    """The four masks for one piece at one square size."""
    big = size * SUPER
    # Scale the font so a king fills about 84% of the square's height.
    probe = ImageFont.truetype(ttf, 400)
    kx0, ky0, kx1, ky1 = probe.getbbox(SOLID['K'])
    fsize = int(400 * big * 0.84 / (ky1 - ky0))
    font = ImageFont.truetype(ttf, fsize)
    # Every piece stands on the same baseline: the king's bottom edge at 91%.
    _, _, _, kb = font.getbbox(SOLID['K'])
    x0, y0, x1, y1 = font.getbbox(OUTLINE[piece])
    dx = (big - (x1 - x0)) // 2 - x0
    dy = int(big * 0.91) - kb

    def draw(ch):
        img = Image.new('L', (big, big), 0)
        ImageDraw.Draw(img).text((dx, dy), ch, font=font, fill=255)
        return img

    line = draw(OUTLINE[piece])
    solid = draw(SOLID[piece])

    if piece == 'P':
        # DejaVu's outline pawn wears a small ring on its head that the
        # filled pawn does not; the pawn's outline is drawn around the
        # filled one instead.
        stroke = int(big * 0.05) | 1
        grown = solid.filter(ImageFilter.MaxFilter(stroke))
        hull = grown
        line = Image.composite(grown, Image.new('L', (big, big), 0),
                               solid.point(lambda v: 255 - v))
    else:
        # hull: everything the outline encloses.  Flood the outside from the
        # corners; what the flood cannot reach is the piece.
        binary = line.point(lambda v: 255 if v > 96 else 0)
        outside = binary.copy()
        ImageDraw.floodfill(outside, (0, 0), 128, thresh=0)
        hull = outside.point(lambda v: 0 if v == 128 else 255)
        hull = hull.filter(ImageFilter.MaxFilter(3))   # cover the outline's antialiased rim

    shadow = hull.filter(ImageFilter.GaussianBlur(big * 0.035))
    shifted = Image.new('L', (big, big), 0)
    shifted.paste(shadow, (int(big * 0.015), int(big * 0.03)))

    def down(img):
        return img.resize((size, size), Image.LANCZOS).tobytes()

    return down(hull), down(line), down(solid), down(shifted)


def font_atlas(ttf, px):
    font = ImageFont.truetype(ttf, px)
    ascent, descent = font.getmetrics()
    chars = [chr(c) for c in range(32, 127)] + EXTRA
    glyphs, pixels = [], bytearray()
    for ch in chars:
        advance = int(round(font.getlength(ch)))
        bbox = font.getbbox(ch)
        if bbox is None or bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            glyphs.append((0, 0, 0, 0, advance, len(pixels)))
            continue
        x0, y0, x1, y1 = bbox
        w, h = x1 - x0, y1 - y0
        img = Image.new('L', (w, h), 0)
        ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255)
        glyphs.append((x0, y0 - ascent, w, h, advance, len(pixels)))
        pixels += img.tobytes()
    return ascent, descent, glyphs, pixels


def generate(font_dir, out_dir):
    sans = os.path.join(font_dir, 'DejaVuSans.ttf')
    lines = ['// Generated by tools/mkchessart.py from the DejaVu fonts. Do not edit.',
             '#pragma once', '#include <cstdint>', '',
             'struct PieceMasks { const uint8_t* hull; const uint8_t* line; const uint8_t* solid; const uint8_t* shadow; };',
             'struct PieceSet { int size; PieceMasks piece[6]; };  // P N B R Q K',
             'struct Glyph { int16_t x, y; uint8_t w, h, advance; uint32_t offset; };',
             'struct Font { int ascent, descent, height; const Glyph* glyph; const uint8_t* pixels; };',
             '']

    sets = []
    for size in PIECE_SIZES:
        entries = []
        for p in PIECES:
            hull, line, solid, shadow = piece_masks(sans, p, size)
            for name, data in (('hull', hull), ('line', line), ('solid', solid), ('shadow', shadow)):
                lines.append('static const uint8_t piece_%s_%d_%s[] = {\n%s };' % (p, size, name, c_bytes(data)))
            entries.append('{ piece_%s_%d_hull, piece_%s_%d_line, piece_%s_%d_solid, piece_%s_%d_shadow }'
                           % ((p, size) * 4))
        sets.append('  { %d, { %s } }' % (size, ', '.join(entries)))
    lines.append('static const PieceSet PieceSets[] = {\n%s\n};' % ',\n'.join(sets))
    lines.append('static const int PieceSetCount = %d;' % len(PIECE_SIZES))
    lines.append('')

    for name, file, px in FONTS:
        ascent, descent, glyphs, pixels = font_atlas(os.path.join(font_dir, file), px)
        lines.append('static const uint8_t font_%s_pixels[] = {\n%s };' % (name, c_bytes(pixels) or '0'))
        lines.append('static const Glyph font_%s_glyphs[] = {\n%s };' % (
            name, ',\n'.join('  { %d, %d, %d, %d, %d, %d }' % g for g in glyphs)))
        lines.append('static const Font font_%s = { %d, %d, %d, font_%s_glyphs, font_%s_pixels };'
                     % (name, ascent, descent, ascent + descent, name, name))
    lines.append('')
    path = os.path.join(out_dir, 'assets.h')
    with open(path + '.tmp', 'w') as f:
        f.write('\n'.join(lines))
    os.replace(path + '.tmp', path)

"""Regenerate the constant tables inside nano/opl/opl3.c.

The OPL core has to link where there is no libm - inside Doom's freestanding
runtime, and later inside a resident driver - so the sine and exponential
tables are written out as C rather than computed at startup.  This script is
where the formulas actually live; run it and paste the result over the block
at the top of opl3.c if either formula ever changes.

The output is verified by rendering: the frozen tables must give audio
identical to a build that computes them with libm.
"""
import math


def wrap(name, ctype, vals, per, fmt):
    out = ['static const %s %s = {' % (ctype, name)]
    for i in range(0, len(vals), per):
        out.append('    ' + ' '.join(fmt % v + ',' for v in vals[i:i + per]))
    out.append('};')
    return '\n'.join(out)


def main():
    # -log2(sin x) over the first quarter cycle, in 1/256ths
    logsin = [int(-math.log(math.sin((i + 0.5) * math.pi / 512)) / math.log(2) * 256 + 0.5)
              for i in range(256)]
    # 2^x - 1 for x in [0,1), scaled by 1024
    expr = [int((2 ** (i / 256.0) - 1) * 1024 + 0.5) for i in range(256)]
    # Envelope steps per sample, 16.16: four rate steps double the speed.
    # The 32 is fitted, not derived.  The doubling law comes from the
    # datasheet; the constant was chosen by ear against Doom's percussion,
    # where 8 left hi-hats and cymbals ringing audibly under later notes.
    # A recording from real hardware would settle it properly.
    eg = [max(1, int(32.0 * 2 ** ((i - 60) / 4.0) * 65536)) for i in range(64)]

    print(wrap('logsinrom[256]', 'uint16_t', logsin, 8, '%5d'))
    print()
    print(wrap('exprom[256]', 'uint16_t', expr, 8, '%5d'))
    print()
    print(wrap('eg_inc[64]', 'uint32_t', eg, 6, '%10u'))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""What the emulated card was doing, read out of a memory dump.

A program that hangs never reaches the report the card prints when it ends,
and the counters are then the only account of what it was up to.  Dump the
module's memory while the machine sits there and read them out:

    python tools/qemu_test.py --img build/x.img --keys "..." \
        --pmemsave 0x109000,0x8000,build/mod.bin
    python tools/sbpeek.py build/mod.bin

The layout is the run of counters after io_probe in modules/dpmi_io.inc; the
order here must match the order there.
"""
import struct
import sys

FIELDS = [
    ('writes to the card',        'I'), ('reads from it',            'I'),
    ('writes to the synthesiser', 'I'), ('the register it points at','B'),
    ('the bank',                  'B'), ('timer flags',              'B'),
    ('resetting',                 'B'), ('has a reply',              'B'),
    ('the reply',                 'B'), ('has a second',             'B'),
    ('the second',                'B'), ('parameters expected',      'B'),
    ('the last command',          'B'), ('parameters so far',        'B'),
]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = open(sys.argv[1], 'rb').read()
    i = d.find(b'SBCARD!!')
    if i < 0:
        sys.exit('no card in that dump: is SB.MOD or DPMI.MOD loaded?')
    print('the card at offset 0x%X in the dump' % i)
    o = i + 8
    for name, fmt in FIELDS:
        v = struct.unpack_from('<' + fmt, d, o)[0]
        o += struct.calcsize(fmt)
        print('  %-26s %d' % (name, v))


if __name__ == '__main__':
    main()

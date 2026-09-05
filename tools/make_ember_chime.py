#!/usr/bin/env python3
"""Generate root/EMBER.WAV: the sound the desktop makes when it opens.

Warm rather than bright, to match the look: a low struck note that blooms
into a fifth and an octave above it, each partial decaying at its own rate
so the sound settles instead of stopping.  16-bit stereo at 44.1 kHz, which
is what the sound chip wants anyway, so nothing has to be resampled.
"""
import math
import struct
import wave
from pathlib import Path

RATE = 44100
LENGTH = 1.9

# a warm stack: root, fifth, octave, and a distant third
PARTIALS = [
    # (ratio, level, decay, delay, detune in cents)
    (1.00, 0.55, 1.6, 0.00, 0),
    (1.50, 0.34, 1.9, 0.06, +2),
    (2.00, 0.30, 2.3, 0.12, -3),
    (3.00, 0.14, 3.0, 0.20, +4),
    (4.00, 0.08, 3.4, 0.28, 0),
    (5.04, 0.05, 3.8, 0.36, 0),
]
BASE = 174.61                                   # F3, low and warm


def render():
    n = int(RATE * LENGTH)
    left = [0.0] * n
    right = [0.0] * n
    for ratio, level, decay, delay, cents in PARTIALS:
        freq = BASE * ratio * (2.0 ** (cents / 1200.0))
        start = int(delay * RATE)
        pan = 0.5 + 0.32 * math.sin(ratio * 2.1)    # spread them a little
        for i in range(start, n):
            t = (i - start) / RATE
            env = (1.0 - math.exp(-t * 260.0)) * math.exp(-t * decay)
            # a touch of vibrato on the upper partials keeps it from sounding dead
            v = math.sin(2 * math.pi * freq * t
                         + (0.008 * ratio) * math.sin(2 * math.pi * 4.5 * t))
            s = level * env * v
            left[i] += s * (1.0 - pan)
            right[i] += s * pan

    # a soft edge at the very end so it does not click
    tail = int(0.05 * RATE)
    for i in range(tail):
        g = i / tail
        left[n - 1 - i] *= g
        right[n - 1 - i] *= g

    peak = max(max(abs(v) for v in left), max(abs(v) for v in right)) or 1.0
    gain = 0.82 / peak
    frames = bytearray()
    for i in range(n):
        frames += struct.pack("<hh",
                              int(max(-32767, min(32767, left[i] * gain * 32767))),
                              int(max(-32767, min(32767, right[i] * gain * 32767))))
    return bytes(frames)


def main():
    out = Path(__file__).resolve().parent.parent / "root" / "EMBER.WAV"
    data = render()
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data)
    print("%s: %.1f s, %d bytes" % (out, LENGTH, out.stat().st_size))


if __name__ == "__main__":
    main()

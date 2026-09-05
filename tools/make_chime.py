#!/usr/bin/env python3
"""Generate root/CHIMES.WAV: a short synthesised start-up chime (22.05 kHz, 8-bit mono)."""
import math
import struct
import wave
from pathlib import Path

RATE = 22050
notes = [(523.25, 0.18), (659.25, 0.18), (783.99, 0.18), (1046.5, 0.55)]  # C5 E5 G5 C6
samples = []
for freq, dur in notes:
    n = int(RATE * dur)
    for i in range(n):
        t = i / RATE
        env = min(1.0, i / (0.01 * RATE)) * math.exp(-3.0 * t / dur)
        v = 0.6 * math.sin(2 * math.pi * freq * t) + 0.25 * math.sin(2 * math.pi * 2 * freq * t)
        samples.append(int(128 + 110 * env * v))
out = Path(__file__).resolve().parent.parent / "root" / "CHIMES.WAV"
with wave.open(str(out), "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(1)
    w.setframerate(RATE)
    w.writeframes(bytes(samples))
print(out, len(samples), "samples,", out.stat().st_size, "bytes")

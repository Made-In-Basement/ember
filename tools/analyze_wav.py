#!/usr/bin/env python3
"""Summarise a captured WAV: loudness over time and the dominant frequency per slice.
Tolerates the unfinalised headers QEMU's wav backend leaves when it is killed."""
import math
import struct
import sys


def goertzel(samples, rate, freq):
    k = 2 * math.cos(2 * math.pi * freq / rate)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + k * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(s1 * s1 + s2 * s2 - k * s1 * s2)


def read_wav(path):
    d = open(path, "rb").read()
    if len(d) < 44 or d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a WAV file ({len(d)} bytes)")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        cid, size = d[pos:pos + 4], struct.unpack_from("<I", d, pos + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", d, pos + 8)
        elif cid == b"data":
            data = d[pos + 8:pos + 8 + size] if size else d[pos + 8:]
            break
        pos += 8 + (size or 0)
        if size == 0:
            break
    if not fmt or data is None:
        raise SystemExit(f"{path}: missing fmt/data chunk")
    _, ch, rate, _, _, bits = fmt
    return rate, ch, bits // 8, data


def main(path, slice_ms=250):
    rate, ch, width, raw = read_wav(path)
    n = len(raw) // (ch * width)
    fmt = {1: "B", 2: "h"}[width]
    data = struct.unpack(f"<{n * ch}{fmt}", raw[:n * ch * width])
    mono = [(data[i] - (128 if width == 1 else 0)) for i in range(0, len(data), ch)]
    print(f"{path}: {rate} Hz, {ch} ch, {width * 8}-bit, {n / rate:.2f} s")
    step = int(rate * slice_ms / 1000)
    cands = [262, 330, 392, 440, 523, 659, 784, 880, 1047, 1319, 1568, 2093]
    loud = 0
    for start in range(0, len(mono), step):
        chunk = mono[start:start + step]
        rms = math.sqrt(sum(x * x for x in chunk) / max(1, len(chunk)))
        if rms < 20:
            continue
        loud += 1
        best = max(cands, key=lambda f: goertzel(chunk, rate, f))
        print(f"  {start / rate:5.2f}s  rms={rms:7.1f}  ~{best} Hz")
    if not loud:
        print("  (silence)")


if __name__ == "__main__":
    main(sys.argv[1])

#!/usr/bin/env python3
"""Checks the sound the app played (host GENPLUS_HOST_AUDIO_DUMP: raw 48 kHz stereo s16) against the test disc's
tones (tests/make_test_cd.py: 1000 Hz left, 1500 Hz right).

  check_tone.py <dump> [left_hz right_hz] [--min-snr dB] [--tolerance %]

From 0.25 s after the sound starts, over 1.5 s, for each channel: the frequency (from the rising zero crossings),
the level and the signal-to-noise ratio (the residue once the fitted sine is taken away: distortion, clicks, the
resampler's images), each measured in blocks of 0.1 s -- the frontend's rate control moves the pitch slowly, and
over a long block that drift would read as noise. The frequency, the level and the SNR are the blocks' medians (a
host under load can drop a buffer now and then; a broken path -- aliasing, a click every buffer, byte-swapped
samples -- spoils every block), the worst block's SNR is printed too.
The pitch must be right within the tolerance (1 %: the frontend's rate control moves it by up to 0.5 %, a wrong
rate -- 44.1 kHz played as 48 kHz -- is 9 % off), the channels not swapped, and the sound clean (30 dB: a click,
aliasing or byte-swapped samples are far below that). Prints one line per channel; exit 1 on a failure.
Pure Python: no numpy needed.
"""
import argparse
import math
import sys
from array import array

RATE = 48000


def measure(x):
    n = len(x)
    ups = []
    for i in range(1, n):
        a, b = x[i - 1], x[i]
        if a < 0 <= b:
            ups.append(i - 1 + (-a) / (b - a))
    if len(ups) < 3:
        return 0.0, 0.0, -99.0
    f = (len(ups) - 1) / (ups[-1] - ups[0]) * RATE
    w = 2 * math.pi * f / RATE
    c = s = 0.0
    for i, v in enumerate(x):
        c += v * math.cos(w * i)
        s += v * math.sin(w * i)
    c, s = 2 * c / n, 2 * s / n
    sig = res = 0.0
    for i, v in enumerate(x):
        fit = c * math.cos(w * i) + s * math.sin(w * i)
        sig += fit * fit
        res += (v - fit) ** 2
    snr = 10 * math.log10(sig / res) if res > 0 else 120.0
    return f, math.hypot(c, s), snr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("tones", nargs="*", type=float)
    ap.add_argument("--min-snr", type=float, default=30.0)
    ap.add_argument("--tolerance", type=float, default=1.0)
    a = ap.parse_args()
    want = a.tones if len(a.tones) == 2 else [1000.0, 1500.0]
    pcm = array("h")
    with open(a.dump, "rb") as f:
        pcm.frombytes(f.read())
    if sys.byteorder != "little":
        pcm.byteswap()
    left, right = pcm[0::2], pcm[1::2]
    start = next((i for i, v in enumerate(left) if abs(v) > 1000), None)
    if start is None:
        print("no sound")
        return 1
    start += RATE // 4
    span = RATE * 3 // 2
    if start + span > len(left):
        print("too little sound: %.2f s" % ((len(left) - start + RATE // 4) / RATE))
        return 1
    ok = True
    block = RATE // 10
    for name, ch, hz in (("left", left, want[0]), ("right", right, want[1])):
        got = [measure([float(v) for v in ch[b:b + block]]) for b in range(start, start + span, block)]
        f = sorted(g[0] for g in got)[len(got) // 2]
        amp = sorted(g[1] for g in got)[len(got) // 2]
        snr = sorted(g[2] for g in got)[len(got) // 2]
        worst = min(g[2] for g in got)
        err = abs(f - hz) / hz * 100
        good = err <= a.tolerance and snr >= a.min_snr and amp > 500
        ok &= good
        print("%s: %.1f Hz (want %.0f, off %.2f %%), level %.0f, SNR %.1f dB (worst 0.1 s: %.1f dB)%s"
              % (name, f, hz, err, amp, snr, worst, "" if good else "  <- FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

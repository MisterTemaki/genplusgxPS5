#!/usr/bin/env python3
"""Checks pixel colours of a host flip dump (PPM).

  check_ppm.py file.ppm x y r g b [tolerance]     the pixel is that colour (default tolerance 8)
  check_ppm.py file.ppm x y red|green|blue|black  the pixel is clearly that colour (a core's colour
                                                  correction moves the exact values)
"""
import sys


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def classify(r, g, b):
    if max(r, g, b) < 40:
        return "black"
    for name, v, o1, o2 in (("red", r, g, b), ("green", g, r, b), ("blue", b, r, g)):
        if v >= 90 and v >= 1.6 * max(o1, o2, 1):
            return name
    return "other"


path, x, y = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
w, h, pix = load(path)
o = (y * w + x) * 3
pr, pg, pb = pix[o], pix[o + 1], pix[o + 2]
if sys.argv[4].isalpha():
    want = sys.argv[4]
    got = classify(pr, pg, pb)
    ok = got == want
    print(f"{path} ({x},{y}) = {pr},{pg},{pb} ({got}) expected {want}: {'ok' if ok else 'FAIL'}")
else:
    r, g, b = map(int, sys.argv[4:7])
    tol = int(sys.argv[7]) if len(sys.argv) > 7 else 8
    ok = abs(pr - r) <= tol and abs(pg - g) <= tol and abs(pb - b) <= tol
    print(f"{path} ({x},{y}) = {pr},{pg},{pb} expected {r},{g},{b}: {'ok' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)

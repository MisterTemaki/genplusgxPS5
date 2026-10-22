#!/usr/bin/env python3
"""check_border.py file.ppm boot.log border-colour picture-colour [fills]
The last "[border] ... window x,y wxh" line of boot.log is the border's window, the last "[video] picture ... -> x,y wxh" line
the picture: around the window (a few pixels out, where the window leaves room) the dump shows the border's colour, inside the
picture the picture's colour. "fills": the picture is the whole window (the window was fitted to the picture's shape).
Colours: red, green, blue, black, or r,g,b (within 12). Scaled to the dump's size."""
import re
import sys

dump, log, border, picture = sys.argv[1:5]
fills = len(sys.argv) > 5 and sys.argv[5] == "fills"
win = rect = None
for line in open(log, errors="replace"):
    m = re.search(r"\[border\] .* window (\d+),(\d+) (\d+)x(\d+)", line)
    if m:
        win = tuple(map(int, m.groups()))
    m = re.search(r"\[video\] picture .* -> (\d+),(\d+) (\d+)x(\d+)", line)
    if m:
        rect = tuple(map(int, m.groups()))
if not win or not rect:
    print("no border window or picture line in", log)
    sys.exit(1)
data = open(dump, "rb").read().split(b"\n", 3)
w, h = map(int, data[1].split())
pix = data[3]
sx, sy = w / 1920, h / 1080


def classify(r, g, b):
    if max(r, g, b) < 40:
        return "black"
    for name, v, o1, o2 in (("red", r, g, b), ("green", g, r, b), ("blue", b, r, g)):
        if v >= 90 and v >= 1.6 * max(o1, o2, 1):
            return name
    return "other"


def at(x, y):
    o = (int(y * sy) * w + int(x * sx)) * 3
    return pix[o], pix[o + 1], pix[o + 2]


def check(x, y, want):
    r, g, b = at(x, y)
    if "," in want:
        good = all(abs(a - int(c)) <= 12 for a, c in zip((r, g, b), want.split(",")))
    else:
        good = classify(r, g, b) == want
    print("%s (%d,%d) = %d,%d,%d, want %s" % ("ok" if good else "FAIL", x, y, r, g, b, want))
    return good


x, y, ww, wh = win
px, py, pw, ph = rect
ok = True
if fills:
    inside = px >= x and py >= y and px + pw <= x + ww and py + ph <= y + wh
    ok &= inside and abs(pw - ww) <= 2 and abs(ph - wh) <= 2
    print("picture", rect, "fills the window" if ok else "does not fill the window", win)
for cx, cy in ((x - 8, y + wh // 2), (x + ww + 7, y + wh // 2), (x + ww // 2, y - 8), (x + ww // 2, y + wh + 7)):
    if 0 <= cx < 1920 and 0 <= cy < 1080:
        ok &= check(cx, cy, border)
for cx, cy in ((px + 8, py + ph // 2), (px + pw - 9, py + ph // 2), (px + pw // 2, py + 8), (px + pw // 2, py + ph - 9)):
    ok &= check(cx, cy, picture)
print("window", win, "picture", rect, "ok" if ok else "FAIL")
sys.exit(0 if ok else 1)

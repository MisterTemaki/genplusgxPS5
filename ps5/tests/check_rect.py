#!/usr/bin/env python3
"""check_rect.py file.ppm boot.log colour  -- the last "[video] picture ... -> x,y wxh" line of boot.log is
where the game picture is: inside it (a few pixels in from each edge) the dump shows `colour` (red, green...),
just outside it black (when there is a border). Scaled to the dump's size (a 720p scan-out)."""
import re
import subprocess
import sys

dump, log, colour = sys.argv[1:4]
rect = None
for line in open(log, errors="replace"):
    m = re.search(r"\[video\] picture .* -> (\d+),(\d+) (\d+)x(\d+)", line)
    if m:
        rect = tuple(map(int, m.groups()))
if not rect:
    print("no picture line in", log)
    sys.exit(1)
with open(dump, "rb") as f:
    w, h = map(int, f.read().split(b"\n", 3)[1].split())
sx, sy = w / 1920, h / 1080
x, y, rw, rh = rect
here = __file__.rsplit("/", 1)[0] + "/check_ppm.py"


def check(px, py, want):
    r = subprocess.run([sys.executable, here, dump, str(int(px * sx)), str(int(py * sy)), want], capture_output=True, text=True)
    print(r.stdout.strip())
    return r.returncode == 0


ok = True
for px, py in ((x + 6, y + rh // 2), (x + rw - 7, y + rh // 2), (x + rw // 2, y + 6), (x + rw // 2, y + rh - 7)):
    ok &= check(px, py, colour)
if x >= 8:
    ok &= check(x - 4, y + rh // 2, "black")
    ok &= check(x + rw + 3, y + rh // 2, "black")
if y >= 8:
    ok &= check(x + rw // 2, y - 4, "black")
print("rect", rect, "ok" if ok else "FAIL")
sys.exit(0 if ok else 1)

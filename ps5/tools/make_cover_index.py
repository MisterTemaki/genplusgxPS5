#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds frontend/data/cover-index.txt: the names of the box art libretro-thumbnails has for each system Genesis Plus GX
runs (Named_Boxarts/<name>.png, git symbolic links included: they are downloaded like the files they point to).

The cover matcher (fe_covers.cpp, fe_artmatch.h) picks a name from this list for a ROM whose own name is not one of them:
a renamed ROM, a fan translation, a hack (Snes9x PS5 2.7, pull request #3 by thatboialex, does the same with its SNES list).
A "#@ <system>" line comes before each system's names; other "#" lines are comments; one name per line, no extension.

  python3 tools/make_cover_index.py                (from ps5/; clones the trees without the pictures)
  python3 tools/make_cover_index.py --repos DIR    (DIR/Sega_-_Mega_Drive_-_Genesis etc.: clones made before)
"""
import argparse
import os
import subprocess
import sys
import tempfile

SOURCES = [
    ("md", "Sega_-_Mega_Drive_-_Genesis"),
    ("scd", "Sega_-_Mega-CD_-_Sega_CD"),
    ("sms", "Sega_-_Master_System_-_Mark_III"),
    ("gg", "Sega_-_Game_Gear"),
    ("sg", "Sega_-_SG-1000"),
]
GITHUB = "https://github.com/libretro-thumbnails/"
FOLDER = "Named_Boxarts/"


def git(cwd, *args):
    return subprocess.run(["git", "-c", "core.quotepath=false", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repos", help="a folder with clones of the libretro-thumbnails repositories (no checkout needed)")
    ap.add_argument("--out", default="frontend/data/cover-index.txt")
    args = ap.parse_args()
    sections = []
    with tempfile.TemporaryDirectory() as tmp:
        for system, name in SOURCES:
            repo = os.path.join(args.repos, name) if args.repos else os.path.join(tmp, name)
            if not args.repos:
                subprocess.run(["git", "clone", "-q", "--depth", "1", "--filter=blob:none", "--no-checkout", GITHUB + name, repo], check=True)
            sha = git(repo, "rev-parse", "HEAD").strip()
            date = git(repo, "log", "-1", "--format=%cs").strip()
            paths = git(repo, "ls-tree", "-r", "--name-only", "-z", "HEAD", "--", FOLDER).split("\0")
            names = sorted({p[len(FOLDER):-4] for p in paths if p.startswith(FOLDER) and p.endswith(".png") and "/" not in p[len(FOLDER):]})
            bad = [n for n in names if not n or "\n" in n or "\t" in n or n.startswith("#")]
            if bad or not names:
                sys.exit("unexpected file names in %s: %r" % (name, bad[:3]))
            sections.append((system, name, sha, date, names))
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# libretro-thumbnails Named_Boxarts filename index: " +
                ", ".join("%s %s (%s)" % (n, sha[:7], d) for _, n, sha, d, _ in sections) + "\n")
        for system, name, _, _, names in sections:
            f.write("#@ %s %s\n" % (system, name))
            for n in names:
                f.write(n + "\n")
    print("%s: %s" % (args.out, ", ".join("%s %d" % (s, len(n)) for s, _, _, _, n in sections)))


if __name__ == "__main__":
    main()

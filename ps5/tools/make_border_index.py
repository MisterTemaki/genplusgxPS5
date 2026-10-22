#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds frontend/data/border-index.txt: the names of the borders The Bezel Project has for each system Genesis Plus GX
runs, so the app knows what it can download without listing the repositories (raw.githubusercontent.com has no directory
listing, and the API is rate limited). Ported from Snes9x PS5's tool (thatboialex's design, pull request #3).

The Bezel Project keeps one repository per system (the ones its own installer, bezelproject.sh, lists):
  md   bezelproject-MegaDrive     retroarch/overlay/GameBezels/MegaDrive/   (Mega Drive and Genesis, every region)
  scd  bezelproject-SegaCD        retroarch/overlay/GameBezels/SegaCD/
  sms  bezelproject-MasterSystem  retroarch/overlay/GameBezels/MasterSystem/
  gg   bezelproject-GameGear      retroarch/overlay/GameBezels/GameGear/
  sg   bezelproject-SG-1000       retroarch/overlay/GameBezels/SG-1000/
The index has a "#@ <system> <repository>/<folder>" line before each system's names: the app matches a game against its
own system's names and downloads the picture from that repository (fe_borderfetch.cpp). Only the names are kept (No-Intro
names, one per line, no extension, "# " comment lines first): the pictures are never bundled, the app downloads the one a
game needs.

  python3 tools/make_border_index.py            (from ps5/; clones the trees without the pictures, a few MB each)
  python3 tools/make_border_index.py --repos DIR   (DIR/bezelproject-MegaDrive etc.: clones made before)
"""
import argparse
import os
import subprocess
import sys
import tempfile

SOURCES = [
    # system, repository, folder of the pictures
    ("md", "bezelproject-MegaDrive", "retroarch/overlay/GameBezels/MegaDrive/"),
    ("scd", "bezelproject-SegaCD", "retroarch/overlay/GameBezels/SegaCD/"),
    ("sms", "bezelproject-MasterSystem", "retroarch/overlay/GameBezels/MasterSystem/"),
    ("gg", "bezelproject-GameGear", "retroarch/overlay/GameBezels/GameGear/"),
    ("sg", "bezelproject-SG-1000", "retroarch/overlay/GameBezels/SG-1000/"),
]
GITHUB = "https://github.com/thebezelproject/"


def git(cwd, *args):
    return subprocess.run(["git", "-c", "core.quotepath=false", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


def names_of(repo, folder):
    """(names, sha, date): the pictures of the folder, from the tree alone (nothing is checked out)."""
    sha = git(repo, "rev-parse", "HEAD").strip()
    date = git(repo, "log", "-1", "--format=%cs").strip()
    paths = git(repo, "ls-tree", "-r", "--name-only", "-z", "HEAD", "--", folder).split("\0")
    names = sorted({p[len(folder):-4] for p in paths if p.startswith(folder) and p.endswith(".png") and "/" not in p[len(folder):]})
    bad = [n for n in names if not n or "\n" in n or "\t" in n or n.startswith("#")]
    if bad or not names:
        sys.exit("unexpected file names in %s: %r" % (repo, bad[:3]))
    return names, sha, date


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repos", help="a folder with clones of the repositories (the pictures need not be checked out)")
    ap.add_argument("--out", default="frontend/data/border-index.txt")
    args = ap.parse_args()

    sections = []
    with tempfile.TemporaryDirectory() as tmp:
        for system, name, folder in SOURCES:
            repo = os.path.join(args.repos, name) if args.repos else os.path.join(tmp, name)
            if not args.repos:
                subprocess.run(["git", "clone", "-q", "--depth", "1", "--filter=blob:none", "--no-checkout", GITHUB + name, repo], check=True)
            sections.append((system, name, folder) + names_of(repo, folder))

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The Bezel Project border filename index: " +
                ", ".join("%s %s (%s)" % (name, sha[:7], date) for _, name, _, _, sha, date in sections) + "\n")
        f.write("# <folder><name>.png on https://raw.githubusercontent.com/thebezelproject/<repository>/master/\n")
        for system, name, folder, names, _, _ in sections:
            f.write("#@ %s %s/%s\n" % (system, name, folder))
            for n in names:
                f.write(n + "\n")
    print("%s: %s" % (args.out, ", ".join("%s %d" % (s, len(n)) for s, _, _, n, _, _ in sections)))


if __name__ == "__main__":
    main()

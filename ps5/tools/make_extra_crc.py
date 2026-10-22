#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds frontend/data/extra-crc.tsv: the CRC32 of Sega dumps that No-Intro (and Redump) do not list -- fan translations,
hacks, homebrew, other dump sets -- with the name each dump goes by. Ported from Snes9x PS5's tool (pull request #3 by
thatboialex), for the systems Genesis Plus GX runs.

Two sources, either or both:
  * libretro-database (https://github.com/libretro/libretro-database), its DATs beyond No-Intro for each system:
    metadat/hacks, metadat/tosec, metadat/homebrew and metadat/lost-level-archive ("Sega - Mega Drive - Genesis.dat"...).
    TOSEC alone names thousands of translated ("[tr en ...]"), hacked ("[h ...]") and other dumps.
  * Hasheous' public dumps (no account, no key), the source Snes9x PS5 uses, one zip per platform:
      https://hasheous.org/api/v1/Dumps/platforms/<platform>.zip
    with <platform> "Sega Mega Drive _ Genesis", "Sega Mega CD _ Sega CD", "Sega Master System", "Sega Game Gear",
    "Sega SG-1000" (--hasheous downloads them, --hasheous-dir reads zips downloaded before, named <system>.zip).

Kept: a ROM with a CRC32 and a size, not an archive (the CRC of a .zip is not the ROM's), and not already in nointro.tsv
for that system. The name is the dump's game name: the app only feeds it to the art matcher (fe_artmatch.h), which strips
tags and translation marks. Rows: system<TAB>CRC32<TAB>name, sorted.

  python3 tools/make_extra_crc.py --libretro-database ../libretro-database --date 2026-10-22   (from ps5/)
  python3 tools/make_extra_crc.py --libretro-database ../libretro-database --hasheous --date 2026-10-22
"""
import argparse
import io
import json
import os
import re
import sys
import urllib.parse
import urllib.request
import zipfile

SYSTEMS = [
    # system, libretro-database DAT name, Hasheous platform
    ("md", "Sega - Mega Drive - Genesis", "Sega Mega Drive _ Genesis"),
    ("scd", "Sega - Mega-CD - Sega CD", "Sega Mega CD _ Sega CD"),
    ("sms", "Sega - Master System - Mark III", "Sega Master System"),
    ("gg", "Sega - Game Gear", "Sega Game Gear"),
    ("sg", "Sega - SG-1000", "Sega SG-1000"),
]
DAT_FOLDERS = ["hacks", "tosec", "homebrew", "lost-level-archive"]
HASHEOUS = "https://hasheous.org/api/v1/Dumps/platforms/"
BAD_DUMP = re.compile(r"\[[abfhopt]\d*[A-Za-z]?\]")  # GoodTools flags: bad dump, overdump, fixed, hack, pirate, trained
ARCHIVE = re.compile(r"\.(zip|7z|rar|gz)$", re.I)
ROM_EXT = re.compile(r"\.(md|gen|smd|bin|sms|gg|sg|iso|cue|chd|img)$", re.I)
GAME = re.compile(r"^game \(\s*$")
NAME = re.compile(r'^\s*name "(.*)"\s*$')
ROM = re.compile(r'rom \( name "(.*?)" size (\d+) crc ([0-9A-Fa-f]{8})')


def dat_roms(path):
    """(game name, rom name, size, crc) of a clrmamepro DAT."""
    name = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if GAME.match(line):
                name = None
                continue
            m = NAME.match(line)
            if m and name is None:
                name = m.group(1)
                continue
            m = ROM.search(line)
            if m and name is not None:
                yield name, m.group(1), int(m.group(2)), m.group(3).lower()


def hasheous_roms(data):
    z = zipfile.ZipFile(io.BytesIO(data))
    for member in z.namelist():
        game = json.loads(z.read(member))
        for attr in game.get("Attributes", []):
            if attr.get("attributeName") != "ROMs":
                continue
            for rom in attr["Value"]:
                yield None, rom.get("Name") or "", rom.get("Size") or 0, (rom.get("Crc") or "").lower()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--libretro-database", help="a checkout of libretro-database (metadat/ is read)")
    ap.add_argument("--hasheous", action="store_true", help="download Hasheous' dumps too")
    ap.add_argument("--hasheous-dir", help="Hasheous' dumps downloaded before: <system>.zip")
    ap.add_argument("--nointro", default="frontend/data/nointro.tsv")
    ap.add_argument("--out", default="frontend/data/extra-crc.tsv")
    ap.add_argument("--date", required=True, help="the day the sources were fetched, for the header")
    args = ap.parse_args()
    if not args.libretro_database and not args.hasheous and not args.hasheous_dir:
        sys.exit("give --libretro-database and/or --hasheous / --hasheous-dir")

    known = set()  # (system, crc)
    with open(args.nointro, encoding="utf-8") as f:
        for line in f:
            if line[0] != "#" and line.count("\t") >= 2:
                system, crc = line.split("\t")[:2]
                known.add((system, crc.lower()))

    best = {}  # (system, crc) -> (rank, name)
    sources = []
    for system, dat, platform in SYSTEMS:
        if system == "scd":
            continue  # a Sega CD disc is named by its file name, never by a CRC (fe_games.cpp RomCrc32): no rows to use
        feeds = []
        if args.libretro_database:
            for folder in DAT_FOLDERS:
                path = os.path.join(args.libretro_database, "metadat", folder, dat + ".dat")
                if os.path.exists(path):
                    feeds.append(dat_roms(path))
                    sources.append("%s/%s" % (folder, dat))
        if args.hasheous_dir and os.path.exists(os.path.join(args.hasheous_dir, system + ".zip")):
            feeds.append(hasheous_roms(open(os.path.join(args.hasheous_dir, system + ".zip"), "rb").read()))
            sources.append("Hasheous " + platform)
        elif args.hasheous:
            url = HASHEOUS + urllib.parse.quote(platform) + ".zip"
            with urllib.request.urlopen(url, timeout=600) as r:
                feeds.append(hasheous_roms(r.read()))
            sources.append("Hasheous " + platform)
        for feed in feeds:
            for game, rom, size, crc in feed:
                if not re.fullmatch(r"[0-9a-f]{8}", crc) or size <= 0 or (system, crc) in known:
                    continue
                if ARCHIVE.search(rom):
                    continue
                name = game if game else ROM_EXT.sub("", rom)
                name = name.replace("\t", " ").replace("\n", " ").strip()
                if not name:
                    continue
                rank = (bool(BAD_DUMP.search(name)), len(name), name)
                if (system, crc) not in best or rank < best[(system, crc)][0]:
                    best[(system, crc)] = (rank, name)

    with open(args.out, "w", encoding="utf-8", newline="\n") as out:
        out.write("# Genesis Plus GX PS5: Sega dumps No-Intro and Redump do not list (fan translations, hacks, homebrew, other dump\n")
        out.write("# sets), by ROM CRC32, fetched %s, made by tools/make_extra_crc.py from: %s.\n" % (args.date, ", ".join(sources)))
        out.write("# system<TAB>CRC32<TAB>the dump's own name: only the art matcher reads it. No-Intro names are in nointro.tsv.\n")
        for key in sorted(best):
            out.write("%s\t%s\t%s\n" % (key[0], key[1].upper(), best[key][1]))
    print("%d CRCs -> %s" % (len(best), args.out), file=sys.stderr)


if __name__ == "__main__":
    main()

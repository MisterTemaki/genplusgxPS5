#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds frontend/data/db-art.tsv: the games libretro-thumbnails has no cover for, but an online database does, with where
to download that cover, and the CRC32 of every dump of the game. Ported from Snes9x PS5's tool (pull request #3 by
thatboialex), for each system Genesis Plus GX runs.

The source is Hasheous' public dumps (no account, no key), one zip per platform, the same files make_extra_crc.py reads:
  https://hasheous.org/api/v1/Dumps/platforms/<platform>.zip
  ("Sega Mega Drive _ Genesis", "Sega Mega CD _ Sega CD", "Sega Master System", "Sega Game Gear", "Sega SG-1000")
One JSON file per game: its name, the ids Hasheous maps it to in other databases, its cover (an IGDB image, which Hasheous
serves at https://hasheous.org/api/v1/images/<id>), and its ROMs with their hashes.

A row, tab-separated: the game's system ("md", "scd", "sms", "gg", "sg"); its name; its cover sources, space-separated
("h<image id>" from Hasheous, "t<id>" for TheGamesDB's front box art,
https://cdn.thegamesdb.net/images/original/boxart/front/<id>-1.jpg); the CRC32 of its dumps, comma-separated, upper case.
The app (fe_dbart.h) finds the row by the ROM's CRC32 or by the words of its name.

Left out: a game with no cover source, and a game libretro-thumbnails already has: one of its dumps is in No-Intro
(nointro.tsv) under a name that is in the cover list (cover-index.txt), so the app finds the cover by CRC.

  python3 tools/make_db_art.py --date 2026-10-22                    (from ps5/; downloads the five dumps)
  python3 tools/make_db_art.py --hasheous-dir DIR --date 2026-10-22 (DIR/<system>.zip downloaded before)
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
    ("md", "Sega Mega Drive _ Genesis"),
    ("scd", "Sega Mega CD _ Sega CD"),
    ("sms", "Sega Master System"),
    ("gg", "Sega Game Gear"),
    ("sg", "Sega SG-1000"),
]
HASHEOUS = "https://hasheous.org/api/v1/Dumps/platforms/"
ARCHIVE = re.compile(r"\.(zip|7z|rar|gz)$", re.I)
CRC = re.compile(r"[0-9a-f]{8}")
IMAGE_ID = re.compile(r"[0-9A-Fa-f]{40}")


def load_sections(path):
    """'#@ <system> ...' sections of cover-index.txt -> {system: set of names}."""
    out, cur = {}, None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#@ "):
                cur = out.setdefault(line.split()[1], set())
            elif line and line[0] != "#" and cur is not None:
                cur.add(line)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hasheous-dir", help="the dumps downloaded before: <system>.zip")
    ap.add_argument("--nointro", default="frontend/data/nointro.tsv")
    ap.add_argument("--covers", default="frontend/data/cover-index.txt")
    ap.add_argument("--out", default="frontend/data/db-art.tsv")
    ap.add_argument("--date", required=True, help="the day the dumps were fetched, for the header")
    args = ap.parse_args()

    nointro = {}  # (system, crc) -> No-Intro name
    with open(args.nointro, encoding="utf-8") as f:
        for line in f:
            if line[0] != "#" and line.count("\t") >= 2:
                system, crc, name = line.rstrip("\n").split("\t")[:3]
                nointro[(system, crc.lower())] = name
    covers = load_sections(args.covers)

    rows = {}  # (system, name) -> [sources, crcs]
    for system, platform in SYSTEMS:
        local = os.path.join(args.hasheous_dir, system + ".zip") if args.hasheous_dir else None
        if local and os.path.exists(local):
            data = open(local, "rb").read()
        else:
            with urllib.request.urlopen(HASHEOUS + urllib.parse.quote(platform) + ".zip", timeout=600) as r:
                data = r.read()
        z = zipfile.ZipFile(io.BytesIO(data))
        for member in sorted(z.namelist()):
            game = json.loads(z.read(member))
            name = (game.get("Name") or "").replace("\t", " ").replace("\n", " ").strip()
            attrs = {a.get("attributeName"): a.get("Value") for a in game.get("Attributes", [])}
            sources = []
            logo = attrs.get("Logo") or ""
            if IMAGE_ID.fullmatch(logo):
                sources.append("h" + logo.upper())
            for m in game.get("Metadata") or []:
                if m.get("Source") == "TheGamesDb" and m.get("Status") == "Mapped" and re.fullmatch(r"\d+", m.get("Id") or ""):
                    sources.append("t" + m["Id"])
            crcs = set()
            for rom in attrs.get("ROMs") or []:
                crc = (rom.get("Crc") or "").lower()
                if not CRC.fullmatch(crc) or (rom.get("Size") or 0) <= 0 or ARCHIVE.search(rom.get("Name") or ""):
                    continue  # never the CRC of an archive
                crcs.add(crc)
            if not name or not sources or not crcs:
                continue
            if any(nointro.get((system, c)) in covers.get(system, ()) for c in crcs):
                continue  # libretro-thumbnails has this game
            row = rows.setdefault((system, name), [[], set()])
            for s in sources:
                if s not in row[0]:
                    row[0].append(s)
            row[1] |= crcs

    with open(args.out, "w", encoding="utf-8", newline="\n") as out:
        out.write("# Genesis Plus GX PS5: Sega games libretro-thumbnails has no cover for, with where an online database has one,\n")
        out.write("# from Hasheous' dumps fetched %s; made by tools/make_db_art.py.\n" % args.date)
        out.write("# system<TAB>name<TAB>cover sources (h = hasheous.org image id, t = TheGamesDB id)<TAB>CRC32 of its dumps\n")
        for key in sorted(rows):
            sources, crcs = rows[key]
            out.write("%s\t%s\t%s\t%s\n" % (key[0], key[1], " ".join(sources), ",".join(c.upper() for c in sorted(crcs))))
    print("%d games -> %s" % (len(rows), args.out), file=sys.stderr)


if __name__ == "__main__":
    main()

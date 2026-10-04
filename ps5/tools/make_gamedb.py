#!/usr/bin/env python3
"""Builds frontend/data/nointro.tsv, the game-name table built into the app, from libretro-database's
No-Intro DATs (metadat/no-intro) and, for Sega CD, its Redump DAT (metadat/redump; names only).

  tools/make_gamedb.py <libretro-database checkout> frontend/data/nointro.tsv

Each line is "system<TAB>CRC32<TAB>No-Intro name" ("-" for the CRC of a disc game, matched by name only).
NES entries come twice in the DAT, with the iNES header (.nes) and without (.unh); both CRCs are kept.
"""
import re
import sys

SYSTEMS = [
    ("md", "no-intro/Sega - Mega Drive - Genesis.dat"),
    ("scd", "redump/Sega - Mega-CD - Sega CD.dat"),
    ("sms", "no-intro/Sega - Master System - Mark III.dat"),
    ("gg", "no-intro/Sega - Game Gear.dat"),
    ("sg", "no-intro/Sega - SG-1000.dat"),
]

GAME = re.compile(r'^game \(\s*$')
NAME = re.compile(r'^\s*name "(.*)"\s*$')
ROM = re.compile(r'rom \( name "(.*?)" size (\d+) crc ([0-9A-Fa-f]{8})')


def parse(path):
    version = ""
    games = []  # (name, [crc...])
    name = None
    crcs = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r'^\s*version "(.*)"', line)
            if m and not version:
                version = m.group(1)
            if GAME.match(line):
                name, crcs = None, []
                continue
            m = NAME.match(line)
            if m and name is None:
                name = m.group(1)
                continue
            m = ROM.search(line)
            if m and name is not None:
                crcs.append(m.group(3).upper())
                continue
            if line.strip() == ")" and name is not None:
                games.append((name, crcs))
                name = None
    return version, games


def main():
    root, out = sys.argv[1], sys.argv[2]
    lines = []
    header = ["# Genesis Plus GX PS5: game names by ROM CRC32, from libretro-database (No-Intro; Redump for Sega CD)"]
    for sysid, rel in SYSTEMS:
        version, games = parse(f"{root}/metadat/{rel}")
        seen = set()
        n = 0
        for name, crcs in games:
            if sysid == "scd":
                key = ("-", name)
                if key not in seen:
                    seen.add(key)
                    lines.append(f"{sysid}\t-\t{name}")
                    n += 1
                continue
            for crc in crcs[:1] if sysid != "nes" else crcs:
                if (crc, name) in seen:
                    continue
                seen.add((crc, name))
                lines.append(f"{sysid}\t{crc}\t{name}")
                n += 1
        header.append(f"#   {sysid}: {rel.split('/', 1)[1]} ({version}), {n} entries")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(header + lines) + "\n")
    print(f"{out}: {len(lines)} entries")


if __name__ == "__main__":
    main()

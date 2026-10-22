#!/usr/bin/env python3
"""Copies Genesis Plus GX's cheat code out of libretro/libretro.c, as it is, for the cheat unit test.

    extract_libretro_cheats.py ../libretro/libretro.c out.inc

The test (tests/test_cheats.cpp, with tests/libretro_cheats_shim.c) then runs the core's own decode_cheat,
apply_cheats, clear_cheats, RAMCheatUpdate, ROMCheatUpdate, retro_cheat_reset and retro_cheat_set against fake ROM
and RAM: the codes the frontend accepts must mean the same to the core, and the core must patch and restore as
expected. The file is libretro.c's (Copyright Eke-Eke, Daniel De Matteis; its licence): nothing is changed in it.
"""
import re
import sys

src = open(sys.argv[1], encoding="utf-8", errors="replace").read().replace("\r\n", "\n")
out = ["/* Extracted from libretro/libretro.c (Genesis Plus GX libretro port, Copyright Eke-Eke (2007-2026),",
       " * Copyright Daniel De Matteis (2012-2016), under that file's licence) by tests/extract_libretro_cheats.py. */"]

# the cheat table and the code alphabets
m = re.search(r"^#define MAX_CHEATS.*?^static char arvalidchars\[\][^\n]*\n", src, re.M | re.S)
if not m:
    sys.exit("libretro.c: the cheat table was not found")
out.append(m.group(0))
out.append("static void RAMCheatUpdate(void);")

for name in ("decode_cheat", "apply_cheats", "clear_cheats", "RAMCheatUpdate", "ROMCheatUpdate", "retro_cheat_reset",
             "retro_cheat_set"):
    # the definition (its signature, then "{" on the next line), not a declaration, up to its closing "}"
    m = re.search(r"^[a-z][^\n(;]*\b%s\([^\n;]*\)[ \t]*\n\{.*?^}\n" % name, src, re.M | re.S)
    if not m:
        sys.exit("libretro.c: %s() was not found" % name)
    out.append(m.group(0))

with open(sys.argv[2], "w") as f:
    f.write("\n".join(out))

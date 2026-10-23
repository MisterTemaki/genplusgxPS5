#!/usr/bin/env python3
"""Builds the .7z archives the 7z tests use (Genesis Plus GX PS5 2.2) into tests/7z/, with py7zr
(pip install py7zr). The archives are committed: the tests need neither py7zr nor the network. Run it again only to
change them:

    python3 tests/make_7z_fixtures.py [out dir, default tests/7z]

For the reader's unit test (tests/test_7z.cpp), with tests/7z/manifest.txt listing what each good archive holds
(archive, entry, size, CRC32 of the data, as computed here):
  lzma.7z lzma2.7z ppmd.7z bcj.7z arm.7z delta.7z   one file (code.bin: 16 KiB of x86-like data, so BCJ has calls to convert)
                                                    with each method or filter
  solid.7z          three files in one solid block: readme.txt, Game B.gg, Game A.md (the first ROM is the .gg)
  empty.7z          no files
  truncated.7z      lzma2.7z cut in half;   corrupt.7z: lzma2.7z with a byte of its packed data changed
  notarchive.7z     a text file
  encrypted.7z      AES-256 data (password "sega"), its list of files readable;   encrypted-header.7z: the list too
  bzip2.7z          BZip2: a method the decoder lacks
  toobig.7z         big.md, 17 MiB of zeros (over the 16 MiB a ROM may have)
  block.7z          pad.dat (65 MiB of zeros) and game.md in one solid block: game.md needs the whole 65 MiB block
  ppmd-mem.7z       PPMd with a 192 MiB model: more memory than the decoder may take
For the app's tests (tests/run-tests.sh, group 44), from tests/make_test_rom.py's ROMs:
  game.7z           game.md, the Mega Drive test ROM
  mystery.7z        mystery.md, the test ROM with its CRC made Streets of Rage 2 (USA)'s (tests/forge_crc.py)
  copier.7z         copier.md, the same ROM body behind a 512-byte copier header (its CRC is read from the data)
  pocket.7z         Pocket.gg, the Game Gear test ROM
  several.7z        notes.txt, Pocket.gg, game.md: the first ROM (Pocket.gg) is the game
  disc.7z           a Sega CD disc: Game (USA).cue and its track
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

import py7zr

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "7z")
os.makedirs(OUT, exist_ok=True)
SOR2_USA = "E01FA526"  # data/nointro.tsv: md, Streets of Rage 2 (USA)

manifest = []


def write(name, files, filters=None, password=None, header_encryption=False, listed=True):
    path = os.path.join(OUT, name)
    if os.path.exists(path):
        os.remove(path)
    kw = {}
    if filters is not None:
        kw["filters"] = filters
    if password is not None:
        kw["password"] = password
        kw["header_encryption"] = header_encryption
    with py7zr.SevenZipFile(path, "w", **kw) as z:
        for arcname, data in files:
            z.writestr(data, arcname)
    if listed:
        for arcname, data in files:
            manifest.append("%s\t%s\t%d\t%08X" % (name, arcname, len(data), zlib.crc32(data)))
    return path


def test_rom(system, region=None):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "rom." + system)
        subprocess.run([sys.executable, os.path.join(HERE, "make_test_rom.py"), p] + ([region] if region else []),
                       check=True, stdout=subprocess.DEVNULL)
        return open(p, "rb").read()


def forged(data, crc):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "rom.bin")
        open(p, "wb").write(data)
        subprocess.run([sys.executable, os.path.join(HERE, "forge_crc.py"), p, crc], check=True, stdout=subprocess.DEVNULL)
        return open(p, "rb").read()


def code_blob():
    # 16 KiB that look a little like x86 code: CALL rel32 (E8) and JMP rel32 (E9) every few bytes among patterned bytes
    out = bytearray()
    i = 0
    while len(out) < 16384:
        out += bytes([(i * 7) & 0xFF, (i >> 3) & 0xFF, 0x8B, 0x45])
        out += bytes([0xE8 if i % 3 else 0xE9]) + ((0x1000 + i * 16) & 0xFFFFFFFF).to_bytes(4, "little")
        i += 1
    return bytes(out[:16384])


# py7zr writes PPMd's properties in 7 bytes; 7-Zip writes 5 (order, memory size), and the SDK's C decoder takes only those:
# the archives here are made the way 7-Zip makes them
from py7zr.compressor import PpmdCompressor  # noqa: E402

_ppmd_props = PpmdCompressor.encode_filter_properties.__func__
PpmdCompressor.encode_filter_properties = classmethod(lambda cls, f: _ppmd_props(cls, f)[:5])
PpmdCompressor._decode_property = lambda self, props: struct.unpack("<BL", bytes(props[:5]))

LZMA2 = {"id": py7zr.FILTER_LZMA2, "preset": 7}
code = code_blob()
write("lzma.7z", [("code.bin", code)], [{"id": py7zr.FILTER_LZMA}])
lzma2 = write("lzma2.7z", [("code.bin", code)], [LZMA2])
write("ppmd.7z", [("code.bin", code)], [{"id": py7zr.FILTER_PPMD, "order": 6, "mem": 24}])
write("bcj.7z", [("code.bin", code)], [{"id": py7zr.FILTER_X86}, LZMA2])
write("arm.7z", [("code.bin", code)], [{"id": py7zr.FILTER_ARM}, LZMA2])
write("delta.7z", [("code.bin", code)], [{"id": py7zr.FILTER_DELTA, "dist": 4}, LZMA2])
md = test_rom("md", "ntsc")
gg = test_rom("gg")
write("solid.7z", [("readme.txt", b"Two games in one archive.\n" * 20), ("Game B.gg", gg), ("Game A.md", md)], [LZMA2])
write("empty.7z", [])

whole = open(lzma2, "rb").read()
open(os.path.join(OUT, "truncated.7z"), "wb").write(whole[: len(whole) // 2])
bad = bytearray(whole)
bad[32 + 40] ^= 0x55  # inside the packed stream (the header, at the end, stays good)
open(os.path.join(OUT, "corrupt.7z"), "wb").write(bytes(bad))
open(os.path.join(OUT, "notarchive.7z"), "wb").write(b"This is not a 7z archive.\n" * 4)

write("encrypted.7z", [("game.md", md)], password="sega", listed=False)
write("encrypted-header.7z", [("game.md", md)], password="sega", header_encryption=True, listed=False)
write("bzip2.7z", [("game.md", md)], [{"id": py7zr.FILTER_BZIP2}], listed=False)
write("toobig.7z", [("big.md", bytes(17 << 20))], [LZMA2], listed=False)
write("block.7z", [("pad.dat", bytes(65 << 20)), ("game.md", md)], [LZMA2], listed=False)
write("ppmd-mem.7z", [("game.md", md)], [{"id": py7zr.FILTER_PPMD, "order": 6, "mem": "192m"}], listed=False)

# the app's
write("game.7z", [("game.md", md)], [LZMA2])
sor2 = forged(md, SOR2_USA)
write("mystery.7z", [("mystery.md", sor2)], [LZMA2])
body = forged(md[:-4], SOR2_USA)  # 128 KiB exactly, so the copier header makes the size 512 past a multiple of 1 KiB
write("copier.7z", [("copier.md", bytes(512) + body)], [LZMA2])
write("pocket.7z", [("Pocket.gg", gg)], [LZMA2])
write("several.7z", [("notes.txt", b"Pocket first, then the Mega Drive game.\n"), ("Pocket.gg", gg), ("game.md", md)], [LZMA2])
cue = b'FILE "Game (USA) (Track 01).bin" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n'
write("disc.7z", [("Game (USA).cue", cue), ("Game (USA) (Track 01).bin", bytes(2352 * 16))], [LZMA2])

open(os.path.join(OUT, "manifest.txt"), "w").write(
    "# archive\tentry\tsize\tCRC32 (tests/make_7z_fixtures.py)\n" + "\n".join(manifest) + "\n")
for f in sorted(os.listdir(OUT)):
    print("%-22s %8d" % (f, os.path.getsize(os.path.join(OUT, f))))

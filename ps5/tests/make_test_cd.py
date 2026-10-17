#!/usr/bin/env python3
"""A stand-in Sega CD disc for the host tests: a data track whose first sector is a Sega CD header
("SEGADISCSYSTEM", the regions), and an audio track of two known tones -- 1000 Hz on the left, 1500 Hz on the
right -- so the tests can check the CD audio that comes out of the app: its pitch (the resampler), its channels,
its byte order and its level.

  make_test_cd.py <out> [--region U|E|J] [--loop]

The output's extension picks the layout:
  .cue   one .bin of raw 2352-byte sectors, both tracks, and the sheet (--loop adds REM LOOP: an MD+ sheet)
  .wcue  the data track as "<name> (Track 1).bin", the audio as "<name> (Track 2).wav", and "<name>.cue"
  .chd   a CHD (version 5, uncompressed: libchdr reads it as it reads chdman's) with the CD metadata (CHT2)
The tracks' lengths are multiples of 4 frames, as a CHD pads them.
"""
import argparse
import math
import os
import struct

DATA_FRAMES = 160   # >= 150: the BIOS wants 2 s of data track
AUDIO_FRAMES = 300  # 4 s of sound
RATE = 44100
TONES = (1000.0, 1500.0)
AMPLITUDE = 12000


def header(region):
    h = bytearray(b"\x00" * 2048)
    h[0x000:0x010] = b"SEGADISCSYSTEM  "
    h[0x010:0x01B] = b"GPGXTEST   "
    h[0x100:0x110] = b"SEGA MEGA DRIVE "
    h[0x110:0x120] = b"(C)TEST 2026.OCT"
    h[0x120:0x150] = b"GENESIS PLUS GX PS5 TEST CD".ljust(48)
    h[0x150:0x180] = b"GENESIS PLUS GX PS5 TEST CD".ljust(48)
    h[0x180:0x18E] = b"GM 00000000-00"
    h[0x190:0x1A0] = b"J".ljust(16)
    h[0x1F0:0x200] = region.encode().ljust(16)
    # the security code's region byte: what Genesis Plus GX picks the BIOS by (loadrom.c, get_region)
    h[0x20B] = {"E": 0x64, "J": 0xA1}.get(region, 0x7A)
    return bytes(h)


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def raw_data(lba, user):
    m, s, f = (lba + 150) // 4500, ((lba + 150) // 75) % 60, (lba + 150) % 75
    return b"\x00" + b"\xff" * 10 + b"\x00" + bytes([bcd(m), bcd(s), bcd(f), 1]) + user + b"\x00" * 288


def data_track(region):
    sectors = [header(region)] + [b"\x00" * 2048] * (DATA_FRAMES - 1)
    return b"".join(raw_data(i, s) for i, s in enumerate(sectors))


def audio_samples():
    """little-endian 16-bit stereo, 588 frames per sector"""
    out = bytearray()
    n = AUDIO_FRAMES * 588
    for i in range(n):
        l = int(round(AMPLITUDE * math.sin(2 * math.pi * TONES[0] * i / RATE)))
        r = int(round(AMPLITUDE * math.sin(2 * math.pi * TONES[1] * i / RATE)))
        out += struct.pack("<hh", l, r)
    return bytes(out)


def wav(pcm):
    return (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE" + b"fmt " +
            struct.pack("<IHHIIHH", 16, 1, 2, RATE, RATE * 4, 4, 16) + b"data" + struct.pack("<I", len(pcm)) + pcm)


def msf(frames):
    return "%02d:%02d:%02d" % (frames // 4500, (frames // 75) % 60, frames % 75)


def chd(path, data, audio):
    unit = 2448
    hunk_frames = 8
    hunkbytes = unit * hunk_frames
    frames = []
    for i in range(DATA_FRAMES):
        frames.append(data[i * 2352:(i + 1) * 2352] + b"\x00" * 96)
    for i in range(AUDIO_FRAMES):
        sec = audio[i * 2352:(i + 1) * 2352]
        # chdman keeps CD audio big-endian: every 16-bit sample swapped
        swapped = bytearray(sec)
        swapped[0::2], swapped[1::2] = sec[1::2], sec[0::2]
        frames.append(bytes(swapped) + b"\x00" * 96)
    total = len(frames)
    hunks = (total + hunk_frames - 1) // hunk_frames
    meta = []
    meta.append("TRACK:1 TYPE:MODE1_RAW SUBTYPE:NONE FRAMES:%d PREGAP:0 PGTYPE:MODE1 PGSUB:RW POSTGAP:0" % DATA_FRAMES)
    meta.append("TRACK:2 TYPE:AUDIO SUBTYPE:NONE FRAMES:%d PREGAP:0 PGTYPE:AUDIO PGSUB:RW POSTGAP:0" % AUDIO_FRAMES)
    hdr_len = 124
    # layout: header | metadata entries | map | (padding to a hunk boundary) | hunks
    meta_blobs = []
    off = hdr_len
    meta_offsets = []
    for text in meta:
        body = text.encode() + b"\x00"
        meta_offsets.append(off)
        off += 16 + len(body)
        meta_blobs.append(body)
    map_off = off
    off += 4 * hunks
    first_hunk = (off + hunkbytes - 1) // hunkbytes
    out = bytearray()
    out += b"MComprHD" + struct.pack(">II", hdr_len, 5) + struct.pack(">IIII", 0, 0, 0, 0)
    out += struct.pack(">QQQ", total * unit, map_off, meta_offsets[0])
    out += struct.pack(">II", hunkbytes, unit)
    out += b"\x00" * 60  # raw SHA-1, SHA-1, parent SHA-1
    assert len(out) == hdr_len
    for i, body in enumerate(meta_blobs):
        nxt = meta_offsets[i + 1] if i + 1 < len(meta_blobs) else 0
        out += b"CHT2" + struct.pack(">I", (0x01 << 24) | len(body)) + struct.pack(">Q", nxt) + body
    assert len(out) == map_off
    for h in range(hunks):
        out += struct.pack(">I", first_hunk + h)
    out += b"\x00" * (first_hunk * hunkbytes - len(out))
    blob = b"".join(frames)
    blob += b"\x00" * (hunks * hunkbytes - len(blob))
    out += blob
    with open(path, "wb") as f:
        f.write(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--region", default="U")
    ap.add_argument("--loop", action="store_true")
    a = ap.parse_args()
    base, ext = os.path.splitext(a.out)
    name = os.path.basename(base)
    folder = os.path.dirname(a.out) or "."
    data = data_track(a.region)
    audio = audio_samples()
    loop = "    REM LOOP\n" if a.loop else ""
    if ext == ".cue":
        with open(base + ".bin", "wb") as f:
            f.write(data + audio)
        with open(a.out, "w") as f:
            f.write(f'FILE "{name}.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'
                    f'  TRACK 02 AUDIO\n{loop}    INDEX 01 {msf(DATA_FRAMES)}\n')
    elif ext == ".wcue":
        with open(os.path.join(folder, name + " (Track 1).bin"), "wb") as f:
            f.write(data)
        with open(os.path.join(folder, name + " (Track 2).wav"), "wb") as f:
            f.write(wav(audio))
        with open(base + ".cue", "w") as f:
            f.write(f'FILE "{name} (Track 1).bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'
                    f'FILE "{name} (Track 2).wav" WAVE\n  TRACK 02 AUDIO\n{loop}    INDEX 01 00:00:00\n')
    elif ext == ".chd":
        chd(a.out, data, audio)
    else:
        raise SystemExit("unknown layout: " + ext)
    print(a.out)


if __name__ == "__main__":
    main()

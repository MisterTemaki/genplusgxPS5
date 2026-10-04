#!/usr/bin/env python3
"""Builds tiny test ROMs for the host tests, one per system (no game data needed).

Each ROM shows a red backdrop; while the pad's Cross button is held -- Mega Drive B, Master System / Game Gear
button 1 with the port's mapping -- the backdrop turns green. So a de-tiled flip shows whether the whole chain
works: the core's CPU and video for that system, the port's scaler and tiler, and the pad -> core input mapping.

    make_test_rom.py out.<md|gen|bin|sms|gg> [ntsc|pal]
"""
import struct
import sys


def assemble(base, items):
    """items: bytes, ("label", name), or (opcode_bytes, "label") for an 8-bit relative branch."""
    labels = {}
    pc = base
    for it in items:
        if isinstance(it, tuple) and it[0] == "label":
            labels[it[1]] = pc
        elif isinstance(it, tuple):
            pc += len(it[0]) + 1
        else:
            pc += len(it)
    out = bytearray()
    pc = base
    for it in items:
        if isinstance(it, tuple) and it[0] == "label":
            continue
        if isinstance(it, tuple):
            op, name = it
            target = labels[name]
            off = target - (pc + len(op) + 1)
            assert -128 <= off <= 127, (name, off)
            out += op + bytes([off & 0xFF])
            pc += len(op) + 1
        else:
            out += it
            pc += len(it)
    return bytes(out), labels


def sms():
    # Master System: all 32 CRAM entries red (green while button 1 is held), display on.
    items = [
        bytes([0xF3, 0xED, 0x56, 0x31, 0xF0, 0xDF]),  # DI; IM 1; LD SP,$DFF0
        bytes([0x3E, 0x40, 0xD3, 0xBF, 0x3E, 0x81, 0xD3, 0xBF]),  # VDP reg 1 = $40 (display on)
        bytes([0x3E, 0x00, 0xD3, 0xBF, 0x3E, 0x87, 0xD3, 0xBF]),  # VDP reg 7 = 0 (backdrop = colour 16)
        ("label", "loop"),
        bytes([0xDB, 0xDC]),                      # IN A,($DC)
        bytes([0xE6, 0x10]),                      # AND $10   button 1 (0 = pressed)
        bytes([0x06, 0x03]),                      # LD B,$03  red
        (bytes([0x20]), "write"),                 # JR NZ,write
        bytes([0x06, 0x0C]),                      # LD B,$0C  green
        ("label", "write"),
        bytes([0x3E, 0x00, 0xD3, 0xBF, 0x3E, 0xC0, 0xD3, 0xBF]),  # CRAM address 0
        bytes([0x0E, 0x20]),                      # LD C,32
        ("label", "cram"),
        bytes([0x78, 0xD3, 0xBE]),                # LD A,B; OUT ($BE),A
        bytes([0x0D]), (bytes([0x20]), "cram"),   # DEC C; JR NZ,cram
        (bytes([0x18]), "loop"),
    ]
    code, _ = assemble(0, items)
    rom = bytearray(0x8000)
    rom[0:len(code)] = code
    rom[0x7FF0:0x7FF8] = b"TMR SEGA"
    rom[0x7FFF] = 0x4C  # SMS export, 32 KiB
    return bytes(rom)


def md(region):
    # Mega Drive (68000, big endian): backdrop = CRAM colour 0, red; green while B is held (pad port A, TH high:
    # bit 4 = B, 0 = pressed). No TMSS: the core boots the cartridge directly.
    code = bytes.fromhex(
        "41F900C00004"      # lea $C00004,a0      VDP control
        "43F900C00000"      # lea $C00000,a1      VDP data
        "30BC8004"          # move.w #$8004,(a0)  mode 1
        "30BC8144"          # move.w #$8144,(a0)  mode 2: display on, mode 5
        "30BC8700"          # move.w #$8700,(a0)  backdrop: palette 0, colour 0
        "13FC004000A10009"  # move.b #$40,$A10009 port A: TH is an output
        # loop:
        "13FC004000A10003"  # move.b #$40,$A10003 TH high
        "4E714E71"          # nop; nop
        "103900A10003"      # move.b $A10003,d0
        "323C000E"          # move.w #$000E,d1    red (0000 BBB0 GGG0 RRR0)
        "08000004"          # btst #4,d0          B
        "6604"              # bne.s write
        "323C00E0"          # move.w #$00E0,d1    green
        # write:
        "20BCC0000000"      # move.l #$C0000000,(a0)  CRAM write, address 0
        "3281"              # move.w d1,(a1)
    )
    loop = 6 + 6 + 4 + 4 + 4 + 8  # offset of "loop" in code
    disp = loop - (len(code) + 2)
    code += bytes([0x60, disp & 0xFF])  # bra.s loop
    rom = bytearray([0xFF] * 0x20000)
    struct.pack_into(">II", rom, 0, 0x00FFFE00, 0x00000200)
    for v in range(2, 64):
        struct.pack_into(">I", rom, v * 4, 0x00000200)
    rom[0x100:0x200] = b" " * 0x100
    rom[0x100:0x110] = b"SEGA MEGA DRIVE "
    rom[0x120:0x150] = b"GENESIS PLUS GX PS5 TEST".ljust(48)
    rom[0x150:0x180] = b"GENESIS PLUS GX PS5 TEST".ljust(48)
    rom[0x180:0x18E] = b"GM 00000000-00"
    struct.pack_into(">II", rom, 0x1A0, 0, len(rom) - 1)
    struct.pack_into(">II", rom, 0x1A8, 0xFF0000, 0xFFFFFF)
    rom[0x1F0:0x1F3] = b"E  " if region == "pal" else b"JUE"
    rom[0x200:0x200 + len(code)] = code
    return bytes(rom)


def gg():
    # Game Gear: 12-bit colours, two bytes each (----BBBB GGGGRRRR): all 32 red, green while button 1 is held.
    items = [
        bytes([0xF3, 0xED, 0x56, 0x31, 0xF0, 0xDF]),  # DI; IM 1; LD SP,$DFF0
        bytes([0x3E, 0x40, 0xD3, 0xBF, 0x3E, 0x81, 0xD3, 0xBF]),  # VDP reg 1 = $40 (display on)
        bytes([0x3E, 0x00, 0xD3, 0xBF, 0x3E, 0x87, 0xD3, 0xBF]),  # VDP reg 7 = 0
        ("label", "loop"),
        bytes([0xDB, 0xDC]),                      # IN A,($DC)
        bytes([0xE6, 0x10]),                      # AND $10   button 1 (0 = pressed)
        bytes([0x06, 0x0F]),                      # LD B,$0F  red (low byte)
        (bytes([0x20]), "write"),                 # JR NZ,write
        bytes([0x06, 0xF0]),                      # LD B,$F0  green
        ("label", "write"),
        bytes([0x3E, 0x00, 0xD3, 0xBF, 0x3E, 0xC0, 0xD3, 0xBF]),  # CRAM address 0
        bytes([0x0E, 0x20]),                      # LD C,32
        ("label", "cram"),
        bytes([0x78, 0xD3, 0xBE, 0x3E, 0x00, 0xD3, 0xBE]),  # LD A,B; OUT ($BE),A; LD A,0; OUT ($BE),A
        bytes([0x0D]), (bytes([0x20]), "cram"),   # DEC C; JR NZ,cram
        (bytes([0x18]), "loop"),
    ]
    code, _ = assemble(0, items)
    rom = bytearray(0x8000)
    rom[0:len(code)] = code
    rom[0x7FF0:0x7FF8] = b"TMR SEGA"
    rom[0x7FFF] = 0x6C  # Game Gear export, 32 KiB
    return bytes(rom)


def main():
    out = sys.argv[1]
    region = sys.argv[2] if len(sys.argv) > 2 else "ntsc"
    ext = out.rsplit(".", 1)[-1].lower()
    data = {
        "md": lambda: md(region),
        "gen": lambda: md(region),
        "bin": lambda: md(region),
        "sms": sms,
        "gg": gg,
    }[ext]()
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {out} ({ext}, {region}, {len(data)} bytes)")


if __name__ == "__main__":
    main()

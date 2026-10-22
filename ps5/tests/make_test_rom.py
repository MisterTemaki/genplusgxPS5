#!/usr/bin/env python3
"""Builds tiny test ROMs for the host tests, one per system (no game data needed).

Each ROM shows a red backdrop; while the pad's Cross button is held -- Mega Drive B, Master System / Game Gear
button 1 with the port's mapping -- the backdrop turns green. So a de-tiled flip shows whether the whole chain
works: the core's CPU and video for that system, the port's scaler and tiler, and the pad -> core input mapping.

    make_test_rom.py out.<md|gen|bin|sms|gg> [ntsc|pal|sram|mdplus|cdbios|cheat|cdbioscheat]

mdplus: a Mega Drive ROM that asks an MD+ (MegaSD) for CD track 2, looped (tests/make_test_cd.py's tones).
cdbios: a stand-in Sega CD BIOS (save it as bios/bios_CD_U.bin): the picture as above, and the RAM cartridge's
first byte $42, or $FF while Cross is held.
cheat (any system), cdbioscheat: the backdrop's colour is read from RAM every frame ($FF0000 on the Mega Drive, $C000 on
the 8-bit systems), where the ROM puts red at boot: a RAM cheat changes the picture's colour.
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


def sms(ramcolor=False):
    # Master System: all 32 CRAM entries red (green while button 1 is held), display on.
    items = [
        bytes([0xF3, 0xED, 0x56, 0x31, 0xF0, 0xDF]),  # DI; IM 1; LD SP,$DFF0
        bytes([0x3E, 0x03, 0x32, 0x00, 0xC0]) if ramcolor else b"",  # LD A,$03; LD ($C000),A
        bytes([0x3E, 0x40, 0xD3, 0xBF, 0x3E, 0x81, 0xD3, 0xBF]),  # VDP reg 1 = $40 (display on)
        bytes([0x3E, 0x00, 0xD3, 0xBF, 0x3E, 0x87, 0xD3, 0xBF]),  # VDP reg 7 = 0 (backdrop = colour 16)
        ("label", "loop"),
        bytes([0xDB, 0xDC]),                      # IN A,($DC)
        bytes([0xE6, 0x10]),                      # AND $10   button 1 (0 = pressed)
        bytes([0x3A, 0x00, 0xC0, 0x47]) if ramcolor else bytes([0x06, 0x03]),  # LD A,($C000); LD B,A / LD B,$03 red
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


def md(region, sram=False, mdplus=False, cdram=False, ramcolor=False):
    # Mega Drive (68000, big endian): backdrop = CRAM colour 0, red; green while B is held (pad port A, TH high:
    # bit 4 = B, 0 = pressed). No TMSS: the core boots the cartridge directly.
    code = bytes.fromhex(
        "41F900C00004"      # lea $C00004,a0      VDP control
        "43F900C00000"      # lea $C00000,a1      VDP data
        "30BC8004"          # move.w #$8004,(a0)  mode 1
        "30BC8144"          # move.w #$8144,(a0)  mode 2: display on, mode 5
        "30BC8700"          # move.w #$8700,(a0)  backdrop: palette 0, colour 0
        "13FC004000A10009"  # move.b #$40,$A10009 port A: TH is an output
    )
    if ramcolor:
        code += bytes.fromhex("33FC000E00FF0000")  # move.w #$000E,$FF0000   red, in RAM
    if mdplus:
        # MD+ (MegaSD): open the overlay, then "play track 2 and loop" (command $12): the test disc's tones
        code += bytes.fromhex(
            "33FCCD540003F7FA"  # move.w #$CD54,$03F7FA
            "33FC12020003F7FE"  # move.w #$1202,$03F7FE
        )
    if cdram:
        # a stand-in Sega CD BIOS: the 512 KiB backup RAM cartridge (Mode 2), write-enabled
        code += bytes.fromhex("13FC0001007FFFFF")  # move.b #1,$7FFFFF
    loop = len(code)  # offset of "loop" in code
    code += bytes.fromhex(
        # loop:
        "13FC004000A10003"  # move.b #$40,$A10003 TH high
        "4E714E71"          # nop; nop
        "103900A10003"      # move.b $A10003,d0
    )
    if sram or cdram:
        # battery RAM: $42 at $200001 every frame, or $FF (the game "erases its save") while B is held; with cdram,
        # the same at $600001: the Sega CD RAM cartridge's first byte
        code += bytes.fromhex(
            "143C0042"          # move.b #$42,d2
            "08000004"          # btst #4,d0          B
            "6604"              # bne.s keep
            "143C00FF"          # move.b #$FF,d2
            # keep:
        )
        code += bytes.fromhex("13C200600001" if cdram else "13C200200001")  # move.b d2,$600001 / $200001
    # move.w $FF0000,d1: the colour in RAM / move.w #$000E,d1: red (0000 BBB0 GGG0 RRR0)
    code += bytes.fromhex("323900FF0000" if ramcolor else "323C000E")
    code += bytes.fromhex(
        "08000004"          # btst #4,d0          B
        "6604"              # bne.s write
        "323C00E0"          # move.w #$00E0,d1    green
        # write:
        "20BCC0000000"      # move.l #$C0000000,(a0)  CRAM write, address 0
        "3281"              # move.w d1,(a1)
    )
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
    if sram:
        rom[0x1B0:0x1B4] = b"RA\xf8\x20"  # backup RAM, odd bytes
        struct.pack_into(">II", rom, 0x1B4, 0x200001, 0x20FFFF)
    rom[0x1F0:0x1F3] = b"E  " if region == "pal" else b"JUE"
    rom[0x200:0x200 + len(code)] = code
    return bytes(rom)


def gg(ramcolor=False):
    # Game Gear: 12-bit colours, two bytes each (----BBBB GGGGRRRR): all 32 red, green while button 1 is held.
    items = [
        bytes([0xF3, 0xED, 0x56, 0x31, 0xF0, 0xDF]),  # DI; IM 1; LD SP,$DFF0
        bytes([0x3E, 0x0F, 0x32, 0x00, 0xC0]) if ramcolor else b"",  # LD A,$0F; LD ($C000),A
        bytes([0x3E, 0x40, 0xD3, 0xBF, 0x3E, 0x81, 0xD3, 0xBF]),  # VDP reg 1 = $40 (display on)
        bytes([0x3E, 0x00, 0xD3, 0xBF, 0x3E, 0x87, 0xD3, 0xBF]),  # VDP reg 7 = 0
        ("label", "loop"),
        bytes([0xDB, 0xDC]),                      # IN A,($DC)
        bytes([0xE6, 0x10]),                      # AND $10   button 1 (0 = pressed)
        bytes([0x3A, 0x00, 0xC0, 0x47]) if ramcolor else bytes([0x06, 0x0F]),  # LD A,($C000); LD B,A / LD B,$0F red
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
    def mega():
        if region == "sram":
            return md("ntsc", sram=True)
        if region == "mdplus":
            return md("ntsc", mdplus=True)
        if region == "cdbios":
            return md("ntsc", cdram=True)
        if region == "cheat":
            return md("ntsc", ramcolor=True)
        if region == "cdbioscheat":
            return md("ntsc", cdram=True, ramcolor=True)
        return md(region)
    cheat = region == "cheat"
    data = {"md": mega, "gen": mega, "bin": mega, "sms": lambda: sms(cheat), "gg": lambda: gg(cheat)}[ext]()
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {out} ({ext}, {region}, {len(data)} bytes)")


if __name__ == "__main__":
    main()

/****************************************************************************
 *  Genesis Plus GX PS5: cheat codes, read the way Genesis Plus GX's libretro port reads them (fe_cheatcode.cpp)
 *
 *  The decoding follows decode_cheat() of libretro/libretro.c:
 *  Copyright Eke-Eke (2007-2026), Copyright Daniel De Matteis (2012-2016).
 *  It is distributed under that file's terms: see fe_cheatcode.cpp for the full notice.
 ****************************************************************************/
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fe
{
namespace cheatcode
{
enum class Kind : uint8_t
{
	None,
	MdGameGenie, // Mega Drive / Sega CD: ABCD-EFGH
	MdActionReplay, // Mega Drive / Sega CD: AAAAAA:DDDD (also AAAAAA:DD on RAM: a byte)
	SmsGameGenie, // Master System / Game Gear / SG-1000: DDA-AAA or DDA-AAA-RXR (with the byte it replaces)
	SmsActionReplay, // 00AA-AADD
	SmsFusionRam, // AAAA:DD
	SmsFusionRom, // RRAAAA:DD (with the byte it replaces)
};

// What the core does with a code: the address it patches (a 24-bit address; RAM at $FFxxxx, ROM below the cartridge's
// size, Sega CD PRG-RAM below $80000 and Word-RAM at $200000), the value (a word when its high byte isn't 0), and for
// the 8-bit systems' ROM patches, the byte that must be there for the patch to apply (0: any).
struct Code
{
	Kind kind = Kind::None;
	uint32_t address = 0;
	uint16_t data = 0;
	uint8_t ref = 0;
	std::string text; // the code as given to the core (retro_cheat_set): upper case, no spaces
};

// One code (no '+'). md: a Mega Drive or Sega CD game (68000 codes); otherwise an 8-bit game (Z80 codes). sega_cd: the
// RAM areas of the Sega CD count as RAM. False when the core would not read the code, or would read it wrongly.
bool Decode(const std::string& code, bool md, bool sega_cd, Code* out);
// A cheat's codes, "A+B+C" as libretro cheat files write them: true when every one of them decodes.
bool DecodeAll(const std::string& codes, bool md, bool sega_cd, std::vector<Code>* out);
} // namespace cheatcode
} // namespace fe

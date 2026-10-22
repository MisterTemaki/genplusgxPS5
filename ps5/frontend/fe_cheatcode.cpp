/****************************************************************************
 *  fe_cheatcode.cpp
 *
 *  Genesis Plus GX PS5: cheat codes read the way Genesis Plus GX's libretro port reads them. The decoding below
 *  is decode_cheat() of libretro/libretro.c (Genesis Plus GX libretro port), written again in C++ to check a
 *  code before the core gets it; the core (retro_cheat_set) still decodes and applies every code itself.
 *
 *  Copyright Eke-Eke (2007-2026)
 *
 *  Copyright Daniel De Matteis (2012-2016)
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include the
 *     complete source code, including the source code for all components used by a
 *     binary built from the modified sources. However, as a special exception, the
 *     source code distributed need not include anything that is normally distributed
 *     (in either source or binary form) with the major components (compiler, kernel,
 *     and so on) of the operating system on which the executable runs, unless that
 *     component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************
 *
 *  What differs from decode_cheat(), on purpose:
 *   - a code must have all its characters: decode_cheat() reads a short code past its end (a code cut short in a
 *     file would patch with whatever follows), so a short or malformed code is refused here instead;
 *   - letters may be lower case and the code may be followed by a comment after a space ("FF0000:0001 (P1)");
 *   - a Mega Drive Action Replay code with a 2-digit value, AAAAAA:DD (about 2,800 of libretro-database's Mega
 *     Drive codes), is read as what it means, the byte DD at AAAAAA, when AAAAAA is RAM: the core gets
 *     AAAAAA:00DD. decode_cheat() would take the missing digits as zeros and write the word DD00 at the even
 *     address below. On ROM such a code is ambiguous and refused.
 */
#include "fe_cheatcode.h"

#include <cctype>
#include <cstring>

namespace fe
{
namespace cheatcode
{
namespace
{
const char kGgChars[] = "ABCDEFGHJKLMNPRSTVWXYZ0123456789"; // libretro.c's ggvalidchars
const char kHexChars[] = "0123456789ABCDEF"; // arvalidchars

int GgDigit(char c)
{
	const char* p = c ? strchr(kGgChars, c) : nullptr;
	return p ? int(p - kGgChars) : -1;
}

int HexDigit(char c)
{
	const char* p = c ? strchr(kHexChars, c) : nullptr;
	return p ? int(p - kHexChars) : -1;
}

// s matches pattern: 'g' a Game Genie letter, 'h' a hex digit, anything else itself
bool Shape(const std::string& s, const char* pattern)
{
	if (s.size() != strlen(pattern))
		return false;
	for (size_t i = 0; i < s.size(); i++)
	{
		const char p = pattern[i];
		if (p == 'g' ? GgDigit(s[i]) < 0 : p == 'h' ? HexDigit(s[i]) < 0 : s[i] != p)
			return false;
	}
	return true;
}

uint32_t Hex(const std::string& s, size_t pos, size_t n)
{
	uint32_t v = 0;
	for (size_t i = 0; i < n; i++)
		v = (v << 4) | uint32_t(HexDigit(s[pos + i]));
	return v;
}

// The RAM a 68000 cheat may write a byte to: Work-RAM, and with a Sega CD game its PRG-RAM and 2M Word-RAM
// (libretro.c's apply_cheats and RAMCheatUpdate).
bool IsRam(uint32_t address, bool sega_cd)
{
	if (address >= 0xFF0000)
		return true;
	return sega_cd && (address < 0x80000 || (address >= 0x200000 && address < 0x240000));
}

bool DecodeMd(const std::string& s, bool sega_cd, Code* c)
{
	if (Shape(s, "gggg-gggg"))
	{
		// Game Genie: decode_cheat's bit layout
		uint32_t address = 0;
		uint16_t data = 0;
		int k = 0;
		for (int i = 0; i < 8; i++)
		{
			if (i == 4)
				k++;
			const uint32_t n = uint32_t(GgDigit(s[size_t(k++)]));
			switch (i)
			{
				case 0: data |= uint16_t(n << 3); break;
				case 1:
					data |= uint16_t(n >> 2);
					address |= (n & 3) << 14;
					break;
				case 2: address |= n << 9; break;
				case 3: address |= (n & 0xF) << 20 | (n >> 4) << 8; break;
				case 4:
					data |= uint16_t((n & 1) << 12);
					address |= (n >> 1) << 16;
					break;
				case 5: data |= uint16_t((n & 1) << 15 | (n >> 1) << 8); break;
				case 6:
					data |= uint16_t((n >> 3) << 13);
					address |= (n & 7) << 5;
					break;
				case 7: address |= n; break;
			}
		}
		c->kind = Kind::MdGameGenie;
		c->address = address;
		c->data = data;
		c->text = s;
		return true;
	}
	if (Shape(s, "hhhhhh:hhhh"))
	{
		c->kind = Kind::MdActionReplay;
		c->address = Hex(s, 0, 6);
		c->data = uint16_t(Hex(s, 7, 4));
		c->text = s;
		return true;
	}
	if (Shape(s, "hhhhhh:hh"))
	{
		const uint32_t address = Hex(s, 0, 6);
		if (!IsRam(address, sega_cd))
			return false;
		c->kind = Kind::MdActionReplay;
		c->address = address;
		c->data = uint16_t(Hex(s, 7, 2));
		c->text = s.substr(0, 7) + "00" + s.substr(7);
		return true;
	}
	return false;
}

bool DecodeSms(const std::string& s, Code* c)
{
	uint32_t address = 0;
	uint16_t data = 0;
	uint8_t ref = 0;
	if (Shape(s, "hhh-hhh") || Shape(s, "hhh-hhh-hhh"))
	{
		// Game Genie: DD A-AA A' (A' = the address's top 4 bits, inverted), then the reference byte, rotated and
		// XORed; decode_cheat skips the reference's middle character
		data = uint16_t(Hex(s, 0, 2));
		address = Hex(s, 2, 1) << 8 | Hex(s, 4, 2);
		address |= (Hex(s, 6, 1) ^ 0xF) << 12;
		if (s.size() == 11)
		{
			ref = uint8_t(HexDigit(s[8]) << 4 | HexDigit(s[10]));
			ref = uint8_t((ref >> 2) | ((ref & 0x03) << 6));
			ref ^= 0xBA;
		}
		c->kind = Kind::SmsGameGenie;
	}
	else if (Shape(s, "hhhh-hhhh"))
	{
		// Action Replay: 00AA-AADD
		address = Hex(s, 2, 2) << 8 | Hex(s, 5, 2);
		data = uint16_t(Hex(s, 7, 2));
		c->kind = Kind::SmsActionReplay;
	}
	else if (Shape(s, "hhhh:hh"))
	{
		address = Hex(s, 0, 4);
		data = uint16_t(Hex(s, 5, 2));
		c->kind = Kind::SmsFusionRam;
	}
	else if (Shape(s, "hhhhhh:hh"))
	{
		ref = uint8_t(Hex(s, 0, 2));
		address = Hex(s, 2, 4);
		data = uint16_t(Hex(s, 7, 2));
		c->kind = Kind::SmsFusionRom;
	}
	else
		return false;
	// the Z80's RAM ($C000-$FFFF, mirrored every 8 KB) as a 24-bit Work-RAM address
	if (address >= 0xC000)
		address = 0xFF0000 | (address & 0x1FFF);
	c->address = address;
	c->data = data;
	c->ref = ref;
	c->text = s;
	return true;
}
} // namespace

bool Decode(const std::string& code, bool md, bool sega_cd, Code* out)
{
	// the code: the first word, upper case
	size_t b = 0;
	while (b < code.size() && isspace(uint8_t(code[b])))
		b++;
	size_t e = b;
	while (e < code.size() && !isspace(uint8_t(code[e])))
		e++;
	std::string s = code.substr(b, e - b);
	for (char& ch : s)
		ch = char(toupper(uint8_t(ch)));
	Code c;
	const bool ok = md ? DecodeMd(s, sega_cd, &c) : DecodeSms(s, &c);
	if (ok && out)
		*out = c;
	return ok;
}

bool DecodeAll(const std::string& codes, bool md, bool sega_cd, std::vector<Code>* out)
{
	std::vector<Code> list;
	size_t pos = 0;
	for (;;)
	{
		const size_t plus = codes.find('+', pos);
		const std::string part = codes.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
		Code c;
		if (!Decode(part, md, sega_cd, &c))
			return false;
		list.push_back(c);
		if (plus == std::string::npos)
			break;
		pos = plus + 1;
	}
	if (out)
		*out = std::move(list);
	return true;
}
} // namespace cheatcode
} // namespace fe

// Genesis Plus GX PS5 tests: the cheats -- the frontend's code check (fe_cheatcode.cpp) against the core's own
// decode_cheat, the core's patching (libretro.c's cheat code on fake ROM and RAM, libretro_cheats_shim.c), the .cht
// files (fe_chtfile.cpp) and the lookup of a game's file.
//
//     cheats-test              every check; exit code 0 when all pass
//     cheats-test decode md|sms CODE   what the core reads in CODE (address, data, reference), for run-tests.sh
//
// SPDX-License-Identifier: MIT

#include "fe_cheatcode.h"
#include "fe_chtfile.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" {
void up_setup(uint8_t hw, uint32_t romsize);
uint8_t* up_rom(void);
uint8_t* up_work_ram(void);
uint8_t* up_prg_ram(void);
uint8_t* up_word_ram(void);
void up_map(int page, uint32_t rom_offset);
uint32_t up_decode(const char* code, uint32_t* address, uint16_t* data, uint16_t* ref);
void up_cheat_reset(void);
void up_cheat_set(unsigned index, bool enabled, const char* code);
void up_frame(void);
void up_bank_switch(void);
int up_count(void);
}

namespace
{
using fe::cheatcode::Code;
using fe::cheatcode::Decode;
using fe::cheatcode::Kind;

constexpr uint8_t kMd = 0x80, kMcd = 0x84, kSms = 0x20, kGgHw = 0x40, kSg = 0x01;

int g_fail = 0, g_pass = 0;
void Check(bool ok, const std::string& what)
{
	if (ok)
		g_pass++;
	else
	{
		g_fail++;
		printf("FAIL: %s\n", what.c_str());
	}
}

uint16_t Word(const uint8_t* base, uint32_t offset)
{
	uint16_t v;
	memcpy(&v, base + offset, 2);
	return v;
}

void SetWord(uint8_t* base, uint32_t offset, uint16_t v)
{
	memcpy(base + offset, &v, 2);
}

// The byte the 68000 sees at an address of its RAM: the core keeps 16-bit words in the host's order (LSB_FIRST), so
// a byte is at offset ^ 1 (core/macros.h, READ_BYTE).
uint8_t Byte68k(const uint8_t* base, uint32_t offset)
{
	return base[offset ^ 1];
}

// ---- the codes' encoders (the inverse of decode_cheat), to build codes for any address
const char kGg[] = "ABCDEFGHJKLMNPRSTVWXYZ0123456789";
std::string MdGameGenie(uint32_t a, uint16_t d)
{
	const uint32_t n[8] = {
		uint32_t(d >> 3) & 31,
		(uint32_t(d & 7) << 2) | ((a >> 14) & 3),
		(a >> 9) & 31,
		((a >> 20) & 15) | (((a >> 8) & 1) << 4),
		(uint32_t(d >> 12) & 1) | (((a >> 16) & 15) << 1),
		(uint32_t(d >> 15) & 1) | ((uint32_t(d >> 8) & 15) << 1),
		((uint32_t(d >> 13) & 3) << 3) | ((a >> 5) & 7),
		a & 31,
	};
	std::string s;
	for (int i = 0; i < 8; i++)
	{
		if (i == 4)
			s += '-';
		s += kGg[n[i]];
	}
	return s;
}

std::string SmsGameGenie(uint32_t a, uint8_t d, int ref = -1)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%02X%X-%02X%X", d, (a >> 8) & 15, a & 0xFF, ((a >> 12) & 15) ^ 15);
	std::string s = buf;
	if (ref >= 0)
	{
		uint8_t x = uint8_t(ref) ^ 0xBA;
		x = uint8_t((x << 2) | (x >> 6)); // decode_cheat rotates right by 2
		snprintf(buf, sizeof(buf), "-%X%X%X", x >> 4, 7, x & 15); // the middle character is skipped
		s += buf;
	}
	return s;
}

// The frontend accepts the code exactly when the core reads it, and reads it the same.
void Agree(const std::string& code, bool md, bool sega_cd, bool want_ok)
{
	Code c;
	const bool ok = Decode(code, md, sega_cd, &c);
	Check(ok == want_ok, "\"" + code + "\" " + (want_ok ? "accepted" : "refused") + (md ? " (68000)" : " (Z80)"));
	if (!ok)
		return;
	up_setup(md ? (sega_cd ? kMcd : kMd) : kSms, 0x40000);
	uint32_t a;
	uint16_t d, r;
	const uint32_t len = up_decode(c.text.c_str(), &a, &d, &r);
	char buf[200];
	snprintf(buf, sizeof(buf), "\"%s\" -> \"%s\": the core reads %06X:%04X ref %02X (len %u), the frontend %06X:%04X ref %02X",
		code.c_str(), c.text.c_str(), a, d, r, len, c.address, c.data, c.ref);
	Check(len == c.text.size() && a == c.address && d == c.data && uint8_t(r) == c.ref, buf);
}

void DecodeTests()
{
	// Mega Drive: Game Genie, Action Replay
	Agree("RFAA-A6VR", true, false, true);
	Agree("AJ4T-AA3C", true, false, true);
	Agree("ajbt-aa3c", true, false, true); // lower case
	Agree("FFA3BF:0003", true, false, true);
	Agree("FF0000:0E00", true, false, true);
	Agree("FFB1DC:0070 (P1)", true, false, true); // a comment after the code (libretro-database's Streets of Rage CD)
	Agree("  FFB1DC:0070  ", true, false, true);
	Agree("ABCD-EFGI", true, false, false); // I is not a Game Genie letter
	Agree("ABCD-EFG", true, false, false); // short: decode_cheat would read past its end
	Agree("FFA3BF:??", true, false, false); // a placeholder
	Agree("FFA3BF:00X1", true, false, false);
	Agree("FFA3BF", true, false, false);
	Agree("", true, false, false);
	Agree("00C0-0B09", true, false, true); // also a Mega Drive Game Genie code (0, C, B, 9 are Game Genie letters)
	// Action Replay with a 2-digit value: the byte, on RAM only
	{
		Code c;
		Check(Decode("FF92A7:78", true, false, &c) && c.text == "FF92A7:0078" && c.address == 0xFF92A7 && c.data == 0x78,
			"FF92A7:78 -> the byte $78 at $FF92A7 (FF92A7:0078)");
		Check(!Decode("001000:78", true, false, nullptr), "a 2-digit value on ROM is refused (byte or word?)");
		Check(!Decode("01A000:12", true, false, nullptr) && Decode("01A000:12", true, true, nullptr),
			"$01A000 is RAM for a Sega CD game (PRG-RAM), not for a cartridge");
		Check(Decode("210000:12", true, true, nullptr) && !Decode("240000:12", true, true, nullptr),
			"Sega CD Word-RAM ($200000-$23FFFF) is RAM");
		// what the core itself would make of it: the missing digits as zeros, a word $7800 at the even address
		uint32_t a;
		uint16_t d, r;
		up_setup(kMd, 0x40000);
		up_decode("FF92A7:78", &a, &d, &r);
		Check(d == 0x7800, "(decode_cheat alone reads FF92A7:78 as the word $7800: why the frontend writes it out)");
	}
	Agree("FF92A7:78", true, false, true);
	Agree("01A000:12", true, true, true);
	// a Sega CD game: RAM codes only (the core patches no ROM for a disc: a ROM code would show On and do nothing)
	Agree("100000:1234", true, false, true);
	Agree("100000:1234", true, true, false);
	Agree("240000:1234", true, true, false);
	Agree("01A000:1234", true, true, true);
	Agree("210000:1234", true, true, true);
	Agree("FF1234:1234", true, true, true);
	Agree(MdGameGenie(0x100000, 0x4E71), true, true, false);
	Agree(MdGameGenie(0x100000, 0x4E71), true, false, true);
	Agree(MdGameGenie(0x01A000, 0x4E71), true, true, true);
	Agree(MdGameGenie(0xFF1234, 0x0001), true, true, true);
	// Master System / Game Gear / SG-1000: Game Genie (with and without the reference), Action Replay, Fusion
	Agree("00C0-0B09", false, false, true);
	Agree("00C4-2299", false, false, true);
	Agree("3A7-1BF", false, false, true);
	Agree("3A7-1BF-2A9", false, false, true);
	Agree("C000:30", false, false, true);
	Agree("C9C123:00", false, false, true);
	Agree("00C4-22XX", false, false, false); // libretro-database's "fill in the number" codes
	Agree("00C4-22", false, false, false);
	Agree("3A7-1BF-2A", false, false, false);
	Agree("RFAA-A6VR", false, false, false);
	Agree("FFA3BF:0003", false, false, false);
	// the decoders on random codes of every shape
	std::mt19937 rng(1234);
	auto hex = [&](int n) {
		std::string s;
		for (int i = 0; i < n; i++)
			s += "0123456789ABCDEF"[rng() % 16];
		return s;
	};
	auto gg = [&](int n) {
		std::string s;
		for (int i = 0; i < n; i++)
			s += kGg[rng() % 32];
		return s;
	};
	const int before = g_fail;
	for (int i = 0; i < 3000 && g_fail == before; i++)
	{
		Agree(gg(4) + "-" + gg(4), true, false, true);
		Agree(hex(6) + ":" + hex(4), true, false, true);
		Agree("FF" + hex(4) + ":" + hex(2), true, false, true);
		Agree(hex(3) + "-" + hex(3), false, false, true);
		Agree(hex(3) + "-" + hex(3) + "-" + hex(3), false, false, true);
		Agree(hex(4) + "-" + hex(4), false, false, true);
		Agree(hex(4) + ":" + hex(2), false, false, true);
		Agree(hex(6) + ":" + hex(2), false, false, true);
	}
	// the encoders: any address and value
	for (int i = 0; i < 2000; i++)
	{
		const uint32_t a = rng() & 0xFFFFFF;
		const uint16_t d = uint16_t(rng());
		Code c;
		const std::string s = MdGameGenie(a, d);
		if (!(Decode(s, true, false, &c) && c.address == a && c.data == d))
		{
			Check(false, "Mega Drive Game Genie round trip " + s);
			break;
		}
		const uint32_t sa = rng() & 0xBFFF; // ROM (RAM addresses are moved to $FFxxxx)
		const uint8_t sd = uint8_t(rng()), sr = uint8_t(rng());
		const std::string t = SmsGameGenie(sa, sd, sr);
		if (!(Decode(t, false, false, &c) && c.address == sa && c.data == sd && c.ref == sr))
		{
			Check(false, "Master System Game Genie round trip " + t);
			break;
		}
	}
	Check(true, "encoders");
	// a cheat's codes
	std::vector<Code> list;
	Check(fe::cheatcode::DecodeAll("FFB1DC:0070+FFB1DE:0070", true, false, &list) && list.size() == 2 &&
			list[1].address == 0xFFB1DE,
		"A+B: two codes");
	Check(!fe::cheatcode::DecodeAll("FFB1DC:0070+FFB1DE:??", true, false, &list), "a cheat with one bad code is unusable");
	Check(!fe::cheatcode::DecodeAll("", true, false, &list) && !fe::cheatcode::DecodeAll("FFB1DC:0070+", true, false, &list),
		"empty codes");
}

// ---- the core's cheat code, as RetroArch and the PS5 frontend drive it: reset, then the codes that are on
void ApplyTests()
{
	// Mega Drive ROM: a word patched, put back by retro_cheat_reset; two patches of one word undone in order
	up_setup(kMd, 0x20000);
	SetWord(up_rom(), 0x1000, 0x1234);
	up_cheat_set(0, true, "001000:ABCD");
	Check(Word(up_rom(), 0x1000) == 0xABCD, "Mega Drive: an Action Replay code patches the ROM");
	up_cheat_set(1, true, "001000:5555");
	Check(Word(up_rom(), 0x1000) == 0x5555, "... a second code on the same word");
	up_cheat_reset();
	Check(Word(up_rom(), 0x1000) == 0x1234, "... switched off: the ROM's word is back");
	up_cheat_set(0, true, MdGameGenie(0x1000, 0xBEEF).c_str());
	Check(Word(up_rom(), 0x1000) == 0xBEEF, "Mega Drive: a Game Genie code (" + MdGameGenie(0x1000, 0xBEEF) + ") patches the ROM");
	up_cheat_reset();
	Check(Word(up_rom(), 0x1000) == 0x1234, "... and is taken back");
	up_cheat_set(0, true, "030000:1111"); // beyond the ROM: nothing
	Check(Word(up_rom(), 0x1000) == 0x1234 && up_count() == 1, "a code beyond the ROM changes nothing");

	// Mega Drive RAM: written every frame, a word or a byte, where the 68000 reads it
	up_setup(kMd, 0x20000);
	up_cheat_set(0, true, "FF1234:ABCD");
	Code byte;
	Decode("FF1237:42", true, false, &byte);
	up_cheat_set(1, true, byte.text.c_str());
	up_cheat_set(2, true, "FF2001:0099");
	up_frame();
	Check(Word(up_work_ram(), 0x1234) == 0xABCD, "Mega Drive RAM: a word value");
	Check(Byte68k(up_work_ram(), 0x1237) == 0x42 && Byte68k(up_work_ram(), 0x1236) == 0,
		"Mega Drive RAM: FF1237:42 writes the byte the 68000 reads at $FF1237, not its neighbour");
	Check(Byte68k(up_work_ram(), 0x2001) == 0x99 && Byte68k(up_work_ram(), 0x2000) == 0, "Mega Drive RAM: FF2001:0099, a byte");
	SetWord(up_work_ram(), 0x1234, 0x0001); // the game writes its own value...
	up_frame(); // ...the next frame the cheat's is back
	Check(Word(up_work_ram(), 0x1234) == 0xABCD, "the value is written again every frame");
	up_cheat_reset();
	SetWord(up_work_ram(), 0x1234, 0x0001);
	up_frame();
	Check(Word(up_work_ram(), 0x1234) == 0x0001, "switched off: the RAM is the game's again");

	// Sega CD: PRG-RAM (Sub-CPU) and Word-RAM, and Work-RAM; no cartridge ROM to patch
	up_setup(kMcd, 0);
	up_cheat_set(0, true, "01A000:1234");
	up_cheat_set(1, true, "200100:00AA"); // a byte in Word-RAM
	up_cheat_set(2, true, "FF9516:0099");
	up_frame();
	Check(Word(up_prg_ram(), 0x1A000) == 0x1234, "Sega CD: a PRG-RAM word");
	Check(Byte68k(up_word_ram(), 0x100) == 0xAA, "Sega CD: a Word-RAM byte");
	Check(Byte68k(up_work_ram(), 0x9516) == 0x99, "Sega CD: a Work-RAM byte (Final Fight CD's \"Inf Time\")");

	// Master System ROM: patched where the Z80 reads it, only when the byte there is the code's reference
	for (uint8_t hw : {kSms, kGgHw, kSg})
	{
		const std::string sys = hw == kSms ? "Master System" : hw == kGgHw ? "Game Gear" : "SG-1000";
		up_setup(hw, 0x40000);
		up_rom()[0x0123] = 0x03;
		up_rom()[0x0456] = 0x77;
		up_cheat_set(0, true, SmsGameGenie(0x0123, 0x30, 0x03).c_str()); // reference matches
		up_cheat_set(1, true, SmsGameGenie(0x0456, 0x11, 0x55).c_str()); // reference doesn't
		Check(up_rom()[0x0123] == 0x30 && up_rom()[0x0456] == 0x77,
			sys + ": a Game Genie code patches the ROM when the byte there is its reference, not otherwise");
		up_cheat_reset();
		Check(up_rom()[0x0123] == 0x03, sys + ": switched off, the ROM's byte is back");
		// RAM ($C000-$DFFF): Action Replay, Fusion
		up_cheat_set(0, true, "00C0-0030");
		up_cheat_set(1, true, "C010:55");
		up_frame();
		Check(up_work_ram()[0x0000] == 0x30 && up_work_ram()[0x0010] == 0x55, sys + ": RAM values ($C000, $C010)");
	}
	// ROM codes with no reference byte (6-character Game Genie, Action Replay and Fusion AAAA:DD below $C000): applied
	// at once, also on a game that never switches banks, and undone with the ROM's own byte, never 0
	for (uint8_t hw : {kSms, kGgHw, kSg})
	{
		const std::string sys = hw == kSms ? "Master System" : hw == kGgHw ? "Game Gear" : "SG-1000";
		up_setup(hw, 0x40000);
		up_rom()[0x0123] = 0x03;
		up_rom()[0x0456] = 0x77;
		up_rom()[0x0789] = 0x42;
		const std::string gg = SmsGameGenie(0x0123, 0x30);
		up_cheat_set(0, true, gg.c_str());
		up_cheat_set(1, true, "0004-5611");
		up_cheat_set(2, true, "0789:22");
		Check(up_rom()[0x0123] == 0x30 && up_rom()[0x0456] == 0x11 && up_rom()[0x0789] == 0x22,
			sys + ": codes with no reference (" + gg + ", 0004-5611, 0789:22) patch the ROM with no bank switch");
		up_bank_switch();
		Check(up_rom()[0x0123] == 0x30 && up_rom()[0x0456] == 0x11 && up_rom()[0x0789] == 0x22, sys + ": ... still after a bank switch");
		up_cheat_set(3, true, "C000:01"); // another cheat switched on: the core takes all its patches back and applies them again
		up_cheat_set(1, false, "0004-5611"); // one switched off
		Check(up_rom()[0x0123] == 0x30 && up_rom()[0x0456] == 0x77 && up_rom()[0x0789] == 0x22,
			sys + ": ... switching other cheats keeps them; the one switched off gives the ROM its byte back");
		up_bank_switch();
		up_cheat_reset();
		Check(up_rom()[0x0123] == 0x03 && up_rom()[0x0456] == 0x77 && up_rom()[0x0789] == 0x42,
			sys + ": ... all off: the ROM's own bytes are back (not 0)");
	}
	// a bank switch: the patch follows the page the Z80 reads; the page left gets its byte back
	up_setup(kSms, 0x40000);
	up_rom()[0x8123] = 0x03; // bank 2, page 0x20 of the map
	up_rom()[0xC123] = 0x03; // bank 3
	up_map(0x20, 0x8000);
	up_cheat_set(0, true, SmsGameGenie(0x8123, 0x30, 0x03).c_str());
	Check(up_rom()[0x8123] == 0x30, "Master System: a patch in the banked area");
	up_map(0x20, 0xC000);
	up_bank_switch();
	Check(up_rom()[0x8123] == 0x03 && up_rom()[0xC123] == 0x30, "... after a bank switch it is on the new bank, the old one restored");
	up_cheat_reset();
	Check(up_rom()[0xC123] == 0x03, "... and taken back from there");
	// the same with no reference: both banks keep their own bytes
	up_setup(kSms, 0x40000);
	up_rom()[0x8123] = 0x05;
	up_rom()[0xC123] = 0x06;
	up_map(0x20, 0x8000);
	up_cheat_set(0, true, SmsGameGenie(0x8123, 0x30).c_str());
	Check(up_rom()[0x8123] == 0x30, "Master System: a patch with no reference in the banked area");
	up_map(0x20, 0xC000);
	up_bank_switch();
	Check(up_rom()[0x8123] == 0x05 && up_rom()[0xC123] == 0x30, "... after a bank switch: on the new bank, the old one's byte back");
	up_cheat_reset();
	Check(up_rom()[0xC123] == 0x06 && up_rom()[0x8123] == 0x05, "... and taken back from there");
}

void FileTests()
{
	const std::string text =
		"cheats = 3\r\n\r\n"
		"cheat0_desc = \"Infinite Lives Player 1\"\r\n"
		"cheat0_code = \"FFA3BF:0003\"\r\n"
		"cheat0_enable = false \r\n\r\n"
		"cheat1_desc = \"P1 Inf HP\"\n"
		"cheat1_code = \"FFB1DC:0070+FFB1DE:0070\"\n"
		"cheat1_enable = true\n"
		"cheat1_handler = \"1\"\n"
		"cheat2_code = \"FFA3BF:??\"\n";
	std::vector<fe::cht::Entry> e;
	Check(fe::cht::Parse(text, &e) && e.size() == 3, "a libretro-database .cht (CRLF): 3 cheats");
	Check(e[0].desc == "Infinite Lives Player 1" && e[0].code == "FFA3BF:0003" && !e[0].enable, "cheat 0");
	Check(e[1].code == "FFB1DC:0070+FFB1DE:0070" && e[1].enable, "cheat 1: two codes, on");
	Check(e[2].desc == "Cheat 3" && e[2].code == "FFA3BF:??", "cheat 2: no description, a placeholder kept as it is");
	std::vector<fe::cht::Entry> back;
	Check(fe::cht::Parse(fe::cht::Write(e), &back) && back.size() == 3 && back[1].code == e[1].code && back[1].enable &&
			back[2].code == "FFA3BF:??" && back[0].desc == e[0].desc,
		"written and read back the same");
	e[0].desc = "Say \"hi\"\nnow";
	Check(fe::cht::Parse(fe::cht::Write(e), &back) && back[0].desc == "Say 'hi' now", "a description stays on one quoted line");
	Check(!fe::cht::Parse("this is not a cheat file\n", &back), "not a cheat file");
	Check(!fe::cht::Parse("cheat\n  name: x\n  code: 7E0000=01\n", &back), "a Snes9x (BML) file is not a libretro one");
}

void LookupTests()
{
	using fe::cht::Best;
	const std::string d = "/data/genplus/cheats/Sega - Mega Drive - Genesis/";
	const std::vector<std::string> md = {
		d + "Sonic The Hedgehog (USA, Europe).cht",
		d + "Sonic The Hedgehog (USA, Europe) (Game Genie).cht",
		d + "Sonic The Hedgehog 2 (World).cht",
		d + "Streets of Rage (USA, Europe, Korea).cht",
		d + "Streets of Rage 2 (USA).cht",
		d + "Phantasy Star II (USA, Europe) (Rev A).cht",
		d + "notes.txt",
	};
	Check(Best(md, "Sonic The Hedgehog (USA, Europe)", "") == md[0], "the exact name; the full set before its Game Genie subset");
	Check(Best(md, "sonic", "Sonic The Hedgehog (USA, Europe)") == md[0], "\"sonic.md\" by its No-Intro name");
	Check(Best(md, "Sonic The Hedgehog 2 (World)", "") == md[2], "Sonic 2 is not Sonic 1");
	Check(Best(md, "Streets of Rage 2", "") == md[4], "Streets of Rage 2 by its title alone");
	Check(Best(md, "Streets of Rage", "") == md[3], "Streets of Rage, not its sequel");
	Check(Best(md, "Phantasy Star II (USA, Europe)", "") == md[5], "a revision's file for the game");
	Check(Best(md, "Sonic & Knuckles", "").empty(), "a game with no file: nothing");
	Check(Best(md, "Sonic", "").empty(), "a short, different name: nothing (never another game's cheats)");
	const std::vector<std::string> regions = {d + "Columns (Japan).cht", d + "Columns (USA, Europe).cht"};
	Check(Best(regions, "Columns (Japan)", "") == regions[0], "the game's region first");
	Check(Best(regions, "Columns", "") == regions[1], "no region: the USA one");
	Check(fe::cht::Key("Dr. Robotnik's Mean Bean Machine (USA).md") == "drrobotniksmeanbeanmachine", "a title's key");
	Check(fe::cht::Key("Final Fight CD (USA).cue") == "finalfightcd", "a disc's key");
}
} // namespace

int main(int argc, char** argv)
{
	if (argc == 4 && strcmp(argv[1], "decode") == 0)
	{
		const bool md = strcmp(argv[2], "md") == 0;
		Code c;
		if (!Decode(argv[3], md, false, &c))
		{
			printf("refused\n");
			return 1;
		}
		uint32_t a;
		uint16_t d, r;
		up_setup(md ? kMd : kSms, 0x40000);
		up_decode(c.text.c_str(), &a, &d, &r);
		printf("%06X:%04X:%02X\n", a, d, r & 0xFF);
		return 0;
	}
	DecodeTests();
	ApplyTests();
	FileTests();
	LookupTests();
	printf("cheats-test: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

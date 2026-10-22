// Genesis Plus GX PS5 frontend: libretro (RetroArch) cheat files, .cht -- reading, writing, and finding a game's.
//
// The format is libretro-database's (cht/Sega - Mega Drive - Genesis/<game>.cht and the other Sega folders):
//
//     cheats = 2
//     cheat0_desc = "Infinite Lives"
//     cheat0_code = "FFA3BF:0003"
//     cheat0_enable = false
//     cheat1_desc = "Invincible"
//     cheat1_code = "AJ4T-AA3C+AJ4T-AA4G"
//     cheat1_enable = true
//
// The lookup is Snes9x PS5's (fe_cheatlookup.h, by thatboialex): the exact name first, then a complete collection
// before its "(Game Genie)" / "(Action Replay)" subsets, and only a small, unambiguous spelling difference.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace fe
{
namespace cht
{
struct Entry
{
	std::string desc;
	std::string code;
	bool enable = false;
};

// Reads a .cht file's text. False when it isn't a libretro cheat file (no cheatN_code line in it).
bool Parse(const std::string& text, std::vector<Entry>* out);
// The text of a .cht file holding these cheats, in the same format.
std::string Write(const std::vector<Entry>& cheats);

bool EndsWithCht(const std::string& name);
// The best of `files` (paths of .cht files) for a game: rom = the ROM's file name without extension, official = its
// No-Intro / Redump name ("" if unknown). "" when nothing is close enough, or two different titles are as close.
std::string Best(const std::vector<std::string>& files, const std::string& rom, const std::string& official);

// Title matching (Snes9x PS5's fe_titlematch.h rules): the name without its (...) / [...] tags and extension,
// letters and digits only, lower case; the region tag; a bounded edit distance.
std::string Key(const std::string& name);
std::string Region(const std::string& name);
int Distance(const std::string& a, const std::string& b, int limit);
} // namespace cht
} // namespace fe

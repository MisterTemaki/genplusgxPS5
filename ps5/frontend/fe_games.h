// Genesis Plus GX PS5 frontend: the game library -- every ROM under the ROM folders, with its system and official name.
//
// The system comes from the file extension (inside a .zip, from the ROM in it). A game is then named the way
// libretro names it:
//   1. its file name, when it is already a No-Intro name ("Super Mario Bros. (World).nes");
//   2. otherwise the CRC32 of the ROM (the whole file, then without a 512-byte copier header), looked up in the
//      table built into the app (data/nointro.tsv: No-Intro for the cartridge systems, Redump names for Sega CD;
//      tools/make_gamedb.py makes it);
//   3. otherwise the file name, loosely: "super mario world.smc" -> the best entry with that title (USA first,
//      then World, Europe, Japan).
// The Master System and the SG-1000 are searched together (and a .sms file may be a Game Gear dump); the table
// that knows the game decides its system, and so where its cover comes from.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fe
{
enum class System : uint8_t
{
	Md, // Mega Drive / Genesis
	SegaCd, // Mega-CD / Sega CD
	Sms, // Master System / Mark III
	Gg, // Game Gear
	Sg, // SG-1000
	Count
};

// The shelf's tabs: systems grouped the way players think of them.
enum class Family : uint8_t
{
	All,
	Md,
	SegaCd,
	Sms, // Master System, SG-1000
	Gg,
	Count
};

struct SystemInfo
{
	const char* id; // "md": the table's and the cover cache's tag
	const char* name; // "Mega Drive"
	const char* thumbs; // the libretro-thumbnails repository
	Family family;
};
const SystemInfo& Info(System s);
const char* FamilyName(Family f); // "All games", "Mega Drive", "Sega CD"...

// ".md" -> System::Md; false for anything that isn't a game we list.
bool SystemForExt(const std::string& ext, System* out);

struct GameInfo
{
	std::string path; // full path
	std::string file_base; // file name without extension
	std::string ext; // ".md", ".zip"... lower case
	System system = System::Md;
	std::string nointro; // official name, "" until known (or unknown)
	std::string title; // what the shelf shows
	std::string region; // "USA", "Europe"... from the name's tags
	bool on_usb = false;
	bool name_by_crc = false; // the name came from the CRC (not the file name)
};

// The table built into the app.
namespace gamedb
{
size_t Count();
// "" when unknown. *found_in: the system whose table knew it (one of `s`'s family).
std::string ByCrc(System s, uint32_t crc, System* found_in);
// Exact No-Intro name ("Sonic The Hedgehog (USA, Europe)") -> itself, or "".
std::string Exact(System s, const std::string& name, System* found_in);
// Loose: title without tags, any case ("sonic the hedgehog") -> best regional entry, or "".
std::string Loose(System s, const std::string& file_base, System* found_in);
// "Sonic The Hedgehog (USA, Europe)" -> "Sonic The Hedgehog"; "Ooze, The (...)" -> "The Ooze"
std::string Title(const std::string& nointro);
std::string Region(const std::string& nointro);
} // namespace gamedb

// The ROM's CRC32s: of the whole file, and without its header (= whole when it has none). For a .zip, of the ROM
// inside; *inner_ext gets that ROM's extension. False if it can't be read (a disc image: named by file name).
bool RomCrc32(const std::string& path, uint32_t* whole, uint32_t* body, std::string* inner_ext = nullptr);

// The system of a .zip: the extension of the first ROM inside. False if none.
bool ArchiveSystem(const std::string& path, System* out);
// The first ROM inside a .zip (its name in the archive) and its bytes. False if none can be read.
bool ReadFromZip(const std::string& path, std::string* inner, std::vector<uint8_t>* data);

// Every ROM under the ROM folders (sub-folders included, 4 levels), sorted by title. Names that need a
// CRC are resolved here too, with a cache (/data/genplus/covers/crc-cache.txt) so each ROM is read once.
std::vector<GameInfo> ScanGames();
} // namespace fe

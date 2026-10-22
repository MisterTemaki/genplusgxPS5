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

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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
	const char* folder; // "MegaDrive": its folder in roms/ and in covers/
};
const SystemInfo& Info(System s);
const char* FamilyName(Family f); // "All games", "Mega Drive", "Sega CD"...

// Creates roms/<folder> and covers/<folder> for every system (MegaDrive, SegaCD, MasterSystem, GameGear, SG1000)
// and moves covers cached by an older build ("md - <name>.png" in covers/) into their system's folder.
void PrepareFolders();

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
	bool on_network = false; // on a share (/smb/..., /nfs/...): named without reading the ROM (netfs)
	bool name_by_crc = false; // the name came from the CRC (not the file name)
	bool crc_pending = false; // on a share, named from its file name only so far: NetNaming reads its CRC
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
// cancel (when given): stops the read when it turns true (false is returned).
bool RomCrc32(const std::string& path, uint32_t* whole, uint32_t* body, std::string* inner_ext = nullptr,
	const std::atomic<bool>* cancel = nullptr);

// The folders to look for games in: OrbisRomRoots() plus the network shares that are connected (in memory, no I/O).
std::vector<std::string> RomRoots();

// Names the games on shares by their ROM's CRC, in the background (Snes9x PS5's design, by thatboialex). A scan does
// not read network ROMs (it would make a big library crawl); this does, one at a time, only for the games with
// crc_pending (a zip: its directory, which also gives its system), and remembers each CRC in crc-cache.txt, so the
// next ScanGames names them. Add() starts or extends the pass; it stops by itself when a game starts
// (netfs_realtime) and when Stop() or the destructor is called (leaving the shelf).
class NetNaming
{
public:
	NetNaming();
	~NetNaming();
	void Add(const std::vector<GameInfo>& games);
	void Stop();
	int Resolved() const; // ROMs read so far (named or not)
	bool Busy() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_i;
};

// The system of a .zip: the extension of the first ROM inside. False if none.
bool ArchiveSystem(const std::string& path, System* out);
// The first ROM inside a .zip (its name in the archive) and its bytes. False if none can be read.
bool ReadFromZip(const std::string& path, std::string* inner, std::vector<uint8_t>* data);

// Every ROM under the ROM folders (sub-folders included, 4 levels), sorted by title. Names that need a
// CRC are resolved here too, with a cache (/data/genplus/covers/crc-cache.txt) so each ROM is read once.
// progress (when given) is told how far the scan is: stage 0 while the folders are listed (done = games found
// so far, total = 0), stage 1 while each game is identified (done of total). It is called often: it decides itself
// how often to draw.
using ScanProgress = std::function<void(int stage, size_t done, size_t total)>;
std::vector<GameInfo> ScanGames(const ScanProgress& progress = nullptr);
} // namespace fe

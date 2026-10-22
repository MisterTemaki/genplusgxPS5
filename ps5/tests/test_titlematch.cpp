// The cover and border matcher over Genesis Plus GX PS5's data (no PS5 SDK needed): fe_titlematch.h, fe_artmatch.h,
// fe_dbart.h, fe_romheader.h and the tables built into the app (frontend/data). Ported from Snes9x PS5 2.7's test (pull
// request #3 by thatboialex), with the Sega systems' lists. Run from ps5/.
// SPDX-License-Identifier: MIT
#include "../frontend/fe_titlematch.h"
#include "../frontend/fe_artmatch.h"
#include "../frontend/fe_dbart.h"
#include "../frontend/fe_romheader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

static int g_failed = 0, g_checked = 0;
#define CHECK(cond, what)                                                       \
	do                                                                          \
	{                                                                           \
		g_checked++;                                                            \
		if (!(cond))                                                            \
		{                                                                       \
			g_failed++;                                                         \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what);               \
		}                                                                       \
	} while (0)

// A "#@ <system>" sectioned list (cover-index.txt, border-index.txt): system -> names.
static std::map<std::string, std::vector<std::string>> Sections(const char* path)
{
	std::map<std::string, std::vector<std::string>> out;
	std::ifstream f(path);
	if (!f.good())
		printf("FAIL cannot read %s (run from ps5/)\n", path), g_failed++;
	std::string line, cur;
	while (std::getline(f, line))
	{
		if (line.compare(0, 3, "#@ ") == 0)
		{
			cur = line.substr(3, line.find(' ', 3) - 3);
			out[cur];
			continue;
		}
		if (!line.empty() && line[0] != '#')
			out[cur].push_back(line);
	}
	return out;
}

// The data lines of a table file.
static std::vector<std::string> Lines(const char* path)
{
	std::ifstream f(path);
	std::vector<std::string> out;
	std::string line;
	while (std::getline(f, line))
		if (!line.empty() && line[0] != '#')
			out.push_back(line);
	return out;
}

static bool Has(const std::vector<std::string>& list, const std::string& name)
{
	return std::find(list.begin(), list.end(), name) != list.end();
}

int main()
{
	namespace am = fe::artmatch;
	using fe::titles::Best;
	const auto covers = Sections("frontend/data/cover-index.txt");
	const auto borders = Sections("frontend/data/border-index.txt");
	const char* const systems[] = {"md", "scd", "sms", "gg", "sg"};
	for (const char* s : systems)
	{
		CHECK(covers.count(s) && covers.at(s).size() > 100, "cover-index.txt has a section for every system");
		CHECK(borders.count(s) && borders.at(s).size() > 100, "border-index.txt has a section for every system");
	}
	const std::vector<std::string>& md = covers.at("md");
	const std::vector<std::string>& gg = covers.at("gg");

	// ---- spelling (fe_titlematch.h) ----
	CHECK(Best(md, "Sonic the Hedgehog 2.md").find("Sonic The Hedgehog 2 (") == 0, "a ROM named loosely finds its game");
	CHECK(Best(md, "Streets-of-Rage 2.gen").find("Streets of Rage 2 (") == 0, "hyphens and the extension do not matter");
	CHECK(Best(md, "Phantasy Star IV (USA)") == "Phantasy Star IV (USA)", "an exact name is itself");
	CHECK(Best(md, "Completely Different Game (USA)").empty(), "a game that is not there finds nothing");
	CHECK(am::NumbersConflict("Sonic The Hedgehog 3", "Sonic The Hedgehog 2 (World)") && !am::NumbersConflict("Sonic The Hedgehog 2", "Sonic The Hedgehog 2 (World)"),
		"a different number is another game");
	// the index (first four characters) gives the plain scan's answers, for every name of every list and its variants
	{
		size_t checked = 0;
		bool same = true;
		for (const auto* lists : {&covers, &borders})
			for (const auto& sec : *lists)
			{
				const fe::titles::Index& ix = fe::titles::IndexOf(sec.second);
				for (const std::string& n : sec.second)
				{
					std::string typo = n;
					if (typo.size() > 6)
						typo[typo.size() / 2] = 'x';
					for (const std::string& q : {n, n.substr(0, n.find(" (")), typo})
					{
						same = same && Best(ix, q) == Best(sec.second, q);
						checked++;
					}
				}
			}
		CHECK(same, "index lookups equal the plain scan");
		std::cout << "Index lookups equal to the plain scan: " << checked << "\n";
	}

	// ---- renamed and fan-translated ROMs (fe_artmatch.h) ----
	{
		const am::Index& ix = am::IndexOf(md);
		CHECK(am::Parse("Langrisser II (English).md").title == "Langrisser II" && am::Parse("Langrisser II (English).md").translated,
			"a translation's name: the title, marked translated");
		CHECK(am::Parse("Phantasy Star IV (ENG) # Genesis").title == "Phantasy Star IV", "a frontend label goes");
		CHECK(am::Parse("Sonic_The_Hedgehog_3_English_Patched_v2.0").title == "Sonic The Hedgehog 3", "underscores, language and version words go");
		CHECK(am::Parse("Langrisser II Rev 01 (Japan)[tr en Hiryuu Honyaku]").title == "Langrisser II",
			"a revision written out (TOSEC's \"Rev 01\") is not part of the title");
		CHECK(am::Parse("Sonic The Hedgehog (U) [!]").region == "USA" && am::Parse("Bare Knuckle (J)").region == "Japan",
			"GoodTools region codes");
		CHECK(am::Resolve(ix, "Langrisser II [T-Eng]") == "Langrisser II (Japan)", "a fan translation finds the Japanese game's art");
		CHECK(am::Resolve(ix, "Langrisser II Rev 01 (Japan)[tr en Hiryuu Honyaku - NoPrgress]") == "Langrisser II (Japan)",
			"so does its TOSEC name");
		CHECK(am::Resolve(ix, "Advanced Busterhawk Gley Lancer (Japan)[tr en M.I.J.E.T.]") == "Advanced Busterhawk Gleylancer (Japan)",
			"Gley Lancer and Gleylancer are one spelling slip apart");
		CHECK(am::Resolve(am::IndexOf(gg), "Phantasy Star Gaiden (Japan)[tr en Magic_Trans][v4.0][Smallfont]") == "Phantasy Star Gaiden (Japan)",
			"a Game Gear translation finds the Game Gear game");
		CHECK(am::Resolve(ix, "Phantasy Star Gaiden (English)").empty(), "and not a Mega Drive one: the systems are apart");
		CHECK(am::Resolve(ix, "Streets of Rage 2 (English)") == "Streets of Rage 2 (USA)", "no region: the USA release");
		CHECK(am::Resolve(ix, "Completely Different Game (English)").empty(), "a game that is not in the list: nothing");
		CHECK(am::Resolve(ix, "Sonic (English)").empty(), "a single common word: nothing");
		CHECK(am::TitleKey(am::Resolve(ix, "Sonic The Hedgehog 3 (English)")) == am::TitleKey("Sonic The Hedgehog 3"), "numbers match");
		CHECK(am::TitleKey(am::Resolve(ix, "Phantasy Star 4 (English)")) == am::TitleKey("Phantasy Star IV"), "a roman numeral is its number");
		// every name of each list, written the ways a translator or a library tool writes it, finds its own game (and never another)
		size_t variants = 0;
		bool own = true;
		for (const auto* lists : {&covers, &borders})
			for (const auto& sec : *lists)
			{
				const am::Index& lx = am::IndexOf(sec.second);
				for (const std::string& name : sec.second)
				{
					const std::string base = am::Parse(name).title;
					std::string underscored = base;
					std::replace(underscored.begin(), underscored.end(), ' ', '_');
					for (const std::string& variant : {base + " (English)", am::Lower(base) + " [T+Eng v1.1]", underscored + "_English", name + ".md"})
					{
						++variants;
						const std::string found = am::Resolve(lx, variant);
						if (am::TitleKey(found) != am::TitleKey(name))
						{
							own = false;
							std::cout << sec.first << ": Resolve(\"" << variant << "\") = \"" << found << "\"\n";
						}
					}
				}
			}
		CHECK(own, "every name, renamed four ways, finds its own game");
		std::cout << "Word matcher: " << variants << " renamed variants\n";
	}

	// ---- the extra CRC table (tools/make_extra_crc.py): dumps No-Intro does not list ----
	{
		std::set<std::string> known; // system + CRC
		for (const std::string& line : Lines("frontend/data/nointro.tsv"))
		{
			const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
			known.insert(line.substr(0, t1) + "\t" + line.substr(t1 + 1, t2 - t1 - 1));
		}
		std::map<std::string, std::string> by_crc;
		bool shape = true, unknown = true, once = true;
		size_t translations = 0, found = 0;
		for (const std::string& line : Lines("frontend/data/extra-crc.tsv"))
		{
			const size_t t1 = line.find('\t');
			shape = shape && t1 != std::string::npos && line.size() > t1 + 10 && line[t1 + 9] == '\t' && covers.count(line.substr(0, t1));
			if (!shape)
				break;
			const std::string key = line.substr(0, t1 + 9), name = line.substr(t1 + 10);
			unknown = unknown && !known.count(key);
			once = once && by_crc.emplace(key, name).second;
			const std::string l = am::Lower(name);
			if (l.find("[tr en") == std::string::npos && l.find("t-en") == std::string::npos)
				continue;
			++translations;
			found += !am::Resolve(am::IndexOf(covers.at(line.substr(0, t1))), name).empty();
		}
		CHECK(shape && by_crc.size() > 3000, "extra-crc.tsv: system, CRC32, name on every line");
		CHECK(unknown, "it lists only what No-Intro lacks for that system");
		CHECK(once, "one name per CRC and system");
		CHECK(by_crc.count("gg\t1667748B") && am::Resolve(am::IndexOf(gg), by_crc.at("gg\t1667748B")) == "Phantasy Star Gaiden (Japan)",
			"a translation's CRC names its game, and the name finds its art");
		std::cout << "Extra CRC table: " << by_crc.size() << " dumps, " << found << " of " << translations << " English translations find art\n";
		CHECK(translations > 100 && found * 100 >= translations * 90, "nine in ten English translations find art");
	}

	// ---- the border index (tools/make_border_index.py): one section per system ----
	{
		const std::vector<std::string>& bmd = borders.at("md");
		CHECK(bmd.size() > 3000 && borders.at("gg").size() > 800 && borders.at("sms").size() > 1000 && borders.at("sg").size() > 200 &&
				borders.at("scd").size() > 400,
			"The Bezel Project's five Sega sets");
		for (const auto& sec : borders)
			CHECK(std::set<std::string>(sec.second.begin(), sec.second.end()).size() == sec.second.size(), "no name twice in a system");
		CHECK(Has(bmd, "Sonic The Hedgehog 2 (World)") && Has(borders.at("gg"), "Sonic The Hedgehog (Japan, USA) (En)"), "names as the repositories spell them");
		std::ifstream f("frontend/data/border-index.txt");
		std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		for (const char* where : {"#@ md bezelproject-MegaDrive/retroarch/overlay/GameBezels/MegaDrive/\n",
				 "#@ scd bezelproject-SegaCD/retroarch/overlay/GameBezels/SegaCD/\n",
				 "#@ sms bezelproject-MasterSystem/retroarch/overlay/GameBezels/MasterSystem/\n",
				 "#@ gg bezelproject-GameGear/retroarch/overlay/GameBezels/GameGear/\n",
				 "#@ sg bezelproject-SG-1000/retroarch/overlay/GameBezels/SG-1000/\n"})
			CHECK(text.find(where) != std::string::npos, "each section names its repository and folder");
		// a retranslation that fits a game's USA and Japanese entries alike: the cover matcher declines, the border matcher
		// (usa_on_tie) takes the USA one; a name that is not a tie is unchanged
		const am::Index& ix = am::IndexOf(bmd);
		CHECK(am::Resolve(ix, "Shining Force II - Retranslation").empty(), "a toss-up between two games: no cover");
		CHECK(am::Resolve(ix, "Shining Force II - Retranslation", "", true) == "Shining Force II (USA)", "the border takes the USA one");
		for (const char* name : {"Langrisser II (English)", "Phantasy Star IV - Retranslation", "Streets of Rage 2 (Europe)", "Gunstar Heroes (Japan)"})
			CHECK(am::Resolve(ix, name, "", true) == am::Resolve(ix, name), "the tie-break changes nothing else");
		CHECK(am::Resolve(ix, "Gunstar Heroes (Japan)") == "Gunstar Heroes (Japan)" && am::Resolve(ix, "Gunstar Heroes (Europe)") == "Gunstar Heroes (Europe)",
			"a game takes its own region's border");
		// most No-Intro releases (not betas, prototypes, unlicensed) have a border by name
		size_t releases = 0, bordered = 0;
		std::map<std::string, std::set<std::string>> bset;
		for (const auto& sec : borders)
			bset[sec.first].insert(sec.second.begin(), sec.second.end());
		for (const std::string& line : Lines("frontend/data/nointro.tsv"))
		{
			const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
			const std::string sys = line.substr(0, t1), name = line.substr(t2 + 1);
			bool odd = false;
			for (const char* tag : {"(Beta", "(Proto", "(Sample", "(Unl", "(Aftermarket", "(Pirate", "(Demo", "(Program", "[b]"})
				odd = odd || name.find(tag) != std::string::npos;
			if (odd || sys == "scd")
				continue;
			++releases;
			bordered += bset[sys].count(name);
		}
		std::cout << "Border index: " << bordered << " of " << releases << " cartridge releases have a border by name\n";
		CHECK(releases > 3500 && bordered * 100 >= releases * 60, "three in five cartridge releases have a border by name (the matcher finds more)");
	}

	// ---- the database covers (fe_dbart.h): rows per system ----
	{
		namespace da = fe::dbart;
		const std::string fixture = "# test\n"
									"md\tSome Homebrew\thAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA t123\tDEADBEEF,0BADF00D\n"
									"gg\tSome Homebrew\thBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\tDEADBEEF\n"
									"md\tKing of Fighters 2000\tt27252\t12345678\n"
									"md\tbroken line\n"
									"md\tNo Sources\t\t11111111\n";
		da::Table t, g;
		da::Parse(fixture.data(), fixture.data() + fixture.size(), "md", t);
		da::Parse(fixture.data(), fixture.data() + fixture.size(), "gg", g);
		CHECK(t.rows.size() == 2 && g.rows.size() == 1, "each system reads its own rows; malformed rows and rows without a source are skipped");
		const da::Row* hb = da::Find(t, 0x0BADF00D, "whatever");
		CHECK(hb && hb->name == "Some Homebrew" && da::Urls(*hb, "https://h/", "https://t/") == "https://h/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\thttps://t/123-1.jpg",
			"found by CRC, with the Hasheous image first and TheGamesDB's box art after it");
		CHECK(da::Find(g, 0xDEADBEEF, "") && da::Urls(*da::Find(g, 0xDEADBEEF, ""), "https://h/", "https://t/") == "https://h/BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",
			"the same CRC on another system is that system's row");
		CHECK(da::Find(t, 0, "King of Fighters 2000 (Unl) [T-Eng]") && !da::Find(t, 0, "King of Fighters 2001"), "by the words of the name, and a different number never");
		CHECK(!da::Find(t, 0xCAFEBABE, "Completely Different Game"), "nothing for a game it does not list");
		std::ifstream f("frontend/data/db-art.tsv", std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		size_t rows = 0;
		for (const char* s : systems)
		{
			da::Table table;
			da::Parse(text.data(), text.data() + text.size(), s, table);
			rows += table.rows.size();
		}
		std::cout << "Database covers: " << rows << " games in db-art.tsv\n";
	}

	// ---- the Sega header (fe_romheader.h) ----
	{
		namespace rh = fe::romheader;
		auto header = [](const char* sys, const char* domestic, const char* overseas, const char* region) {
			std::vector<uint8_t> h(0x200, ' ');
			memcpy(&h[0x100], sys, strlen(sys));
			memcpy(&h[0x120], domestic, strlen(domestic));
			memcpy(&h[0x150], overseas, strlen(overseas));
			memcpy(&h[0x1F0], region, strlen(region));
			return h;
		};
		auto reader = [](const std::vector<uint8_t>& img) {
			return [&img](uint64_t at, uint8_t* out, size_t n) {
				if (at + n > img.size())
					return false;
				memcpy(out, img.data() + at, n);
				return true;
			};
		};
		rh::Info info;
		std::vector<uint8_t> rom = header("SEGA MEGA DRIVE ", "LANGRISSER II", "LANGRISSER II", "J  ");
		rom.resize(0x80000, 0xff);
		CHECK(rh::ReadCart(reader(rom), rom.size(), &info) && info.title == "LANGRISSER II" && info.japan() && info.other.empty(),
			"a Japanese cartridge: its domestic title, Japan");
		rom = header("SEGA GENESIS    ", "SONIC THE               HEDGEHOG 2", "SONIC THE HEDGEHOG 2", "JUE");
		rom.resize(0x80000, 0xff);
		CHECK(rh::ReadCart(reader(rom), rom.size(), &info) && info.title == "SONIC THE HEDGEHOG 2" && !info.japan() && info.other.empty(),
			"a world release: the overseas title, spaces made one (the same domestic title is not kept twice)");
		rom = header("SEGA MEGA DRIVE ", "\x83\x5c\x83\x6c\x83\x62\x83\x4e", "SONIC THE HEDGEHOG", "4");
		rom.resize(0x80000, 0xff);
		CHECK(rh::ReadCart(reader(rom), rom.size(), &info) && info.title == "SONIC THE HEDGEHOG" && !info.japan(),
			"a Shift-JIS domestic title is no text: the overseas one; region 4 (USA) is not Japan");
		rom = header("SEGA MEGA DRIVE ", "BARE KNUCKLE", "STREETS OF RAGE", "1");
		rom.resize(0x80000, 0xff);
		CHECK(rh::ReadCart(reader(rom), rom.size(), &info) && info.title == "BARE KNUCKLE" && info.japan() && info.other == "STREETS OF RAGE",
			"region 1 is Japan: the domestic title, the overseas one kept as the other");
		// behind a 512-byte copier header, and interleaved (.smd)
		std::vector<uint8_t> copier(512, 0);
		copier.insert(copier.end(), rom.begin(), rom.end());
		CHECK(rh::ReadCart(reader(copier), copier.size(), &info) && info.title == "BARE KNUCKLE", "behind a copier header");
		std::vector<uint8_t> smd(512, 0);
		for (size_t block = 0; block < rom.size(); block += 16384)
		{
			std::vector<uint8_t> b(16384);
			for (size_t i = 0; i < 8192; i++)
			{
				b[i] = rom[block + 2 * i + 1];
				b[8192 + i] = rom[block + 2 * i];
			}
			smd.insert(smd.end(), b.begin(), b.end());
		}
		CHECK(rh::ReadCart(reader(smd), smd.size(), &info) && info.title == "BARE KNUCKLE", "an interleaved .smd dump");
		std::vector<uint8_t> none(0x80000, 0x41);
		CHECK(!rh::ReadCart(reader(none), none.size(), &info), "no SEGA at $100: no header");
		std::vector<uint8_t> tiny(100, 0);
		CHECK(!rh::ReadCart(reader(tiny), tiny.size(), &info), "a file too small for a header");
		// a disc: an .iso (2048-byte sectors) and a raw track (2352: 16 bytes of sync and header first)
		std::vector<uint8_t> iso = header("SEGA MEGA DRIVE ", "LUNAR THE SILVER STAR", "LUNAR THE SILVER STAR", "U  ");
		memcpy(iso.data(), "SEGADISCSYSTEM  ", 16);
		iso.resize(0x10000, 0);
		CHECK(rh::ReadDisc(reader(iso), &info) && info.title == "LUNAR THE SILVER STAR" && !info.japan(), "a Sega CD .iso");
		std::vector<uint8_t> raw(16, 0);
		raw.insert(raw.end(), iso.begin(), iso.end());
		CHECK(rh::ReadDisc(reader(raw), &info) && info.title == "LUNAR THE SILVER STAR", "a raw 2352-byte track");
		CHECK(!rh::ReadDisc(reader(rom), &info), "a cartridge is not a disc");
		// the header's title finds the art (ResolveHeader)
		const am::Index& ix = am::IndexOf(md);
		CHECK(am::ResolveHeader(ix, "LANGRISSER II", true) == "Langrisser II (Japan)", "a Japanese header finds the Japanese game");
		CHECK(am::ResolveHeader(ix, "PHANTASY STAR 4", false).find("Phantasy Star IV (") == 0, "a header's digits are its numerals");
		CHECK(am::ResolveHeader(ix, "BARE KNUCKLE II", true).find("Bare Knuckle II - Shitou e no Requiem") == 0,
			"a numbered header keeps its number: Bare Knuckle II, not III (their letters are alike once doubled vowels collapse)");
		CHECK(am::ResolveHeader(ix, "SEGA GENESIS", false).empty() && am::ResolveHeader(ix, "SEGA MEGA DRIVE", false).empty(),
			"a header that names only the console finds nothing");
	}

	printf("%d checks, %d failed\n", g_checked, g_failed);
	return g_failed ? 1 : 0;
}

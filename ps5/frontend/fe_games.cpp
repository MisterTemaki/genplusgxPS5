// Genesis Plus GX PS5 frontend: the game library (fe_games.h).
// SPDX-License-Identifier: MIT

#include "fe_games.h"
#include "fe_dbart.h"

#include "OrbisPaths.h"
#include "ProsperoThread.h"
#include "netfs.h"

#include "third_party/unzip.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <unordered_map>

#define GENPLUS_INCBIN(sym, path)                                                                                \
	__asm__(".section .rodata\n"                                                                               \
			".balign 16\n"                                                                                     \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                        \
			".incbin \"" path "\"\n"                                                                           \
			".global " #sym "_end\n" #sym "_end:\n"                                                            \
			".previous\n");                                                                                    \
	extern "C" const char sym##_begin[];                                                                       \
	extern "C" const char sym##_end[];

GENPLUS_INCBIN(genplus_gamedb, GAMEDB_TSV)
// the dumps No-Intro does not list, and the games an online database has a cover of (tools/make_extra_crc.py, make_db_art.py)
GENPLUS_INCBIN(genplus_extracrc, EXTRACRC_TSV)
GENPLUS_INCBIN(genplus_dbart, DBART_TSV)

namespace fe
{
namespace
{
const SystemInfo kSystems[size_t(System::Count)] = {
	{"md", "Mega Drive", "Sega_-_Mega_Drive_-_Genesis", Family::Md, "MegaDrive"},
	{"scd", "Sega CD", "Sega_-_Mega-CD_-_Sega_CD", Family::SegaCd, "SegaCD"},
	{"sms", "Master System", "Sega_-_Master_System_-_Mark_III", Family::Sms, "MasterSystem"},
	{"gg", "Game Gear", "Sega_-_Game_Gear", Family::Gg, "GameGear"},
	{"sg", "SG-1000", "Sega_-_SG-1000", Family::Sms, "SG1000"},
};

struct ExtSys
{
	const char* ext;
	System sys;
};
// What the shelf lists (Genesis Plus GX's libretro extensions; .bin is a Mega Drive ROM, discs are .cue/.chd/.iso).
const ExtSys kExts[] = {
	{".md", System::Md}, {".gen", System::Md}, {".smd", System::Md}, {".bin", System::Md}, {".mdx", System::Md},
	{".cue", System::SegaCd}, {".chd", System::SegaCd}, {".iso", System::SegaCd},
	{".sms", System::Sms}, {".gg", System::Gg}, {".sg", System::Sg},
};

uint32_t Crc(const uint8_t* p, size_t n)
{
	return uint32_t(crc32(0L, p, uInt(n)));
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = char(tolower(uint8_t(c)));
	return s;
}

std::string ExtOf(const std::string& name)
{
	const size_t dot = name.find_last_of('.');
	const size_t slash = name.find_last_of("/\\");
	if (dot == std::string::npos || dot == 0 || (slash != std::string::npos && dot < slash))
		return "";
	return Lower(name.substr(dot));
}

// The systems searched together for a game of system s.
std::vector<System> Group(System s)
{
	switch (s)
	{
		case System::Sms: return {System::Sms, System::Sg, System::Gg};
		case System::Sg: return {System::Sg, System::Sms};
		default: return {s};
	}
}

// "Super Mario World (USA) (Rev 1)" -> "super mario world"; also drops [tags] and trims.
std::string Key(const std::string& name)
{
	std::string out;
	int depth = 0;
	for (char c : name)
	{
		if (c == '(' || c == '[')
			depth++;
		else if ((c == ')' || c == ']') && depth > 0)
			depth--;
		else if (depth == 0)
			out += char(tolower(uint8_t(c)));
	}
	// collapse spaces, drop punctuation that file names often lose
	std::string k;
	bool space = false;
	for (char c : out)
	{
		if (c == '_' || c == ' ' || c == '.')
			space = !k.empty();
		else if (isalnum(uint8_t(c)) || c == '&' || c == '\'' || c == '-' || c == ',' || c == '!')
		{
			if (space)
				k += ' ';
			space = false;
			k += c;
		}
	}
	return k;
}

int RegionRank(const std::string& name)
{
	// prefer clean dumps: no Beta/Proto/Pirate/Virtual Console/etc.
	int penalty = 0;
	static const char* const bad[] = {"(Beta", "(Proto", "(Pirate", "(Virtual Console", "(Sample", "(Demo", "(Alt",
		"(Unl", "(Aftermarket", "(Arcade", "(Kiosk", "(Program", "(Hack", "(Switch Online", "(Collection"};
	for (const char* b : bad)
		if (name.find(b) != std::string::npos)
			penalty += 100;
	if (name.find("(Rev") != std::string::npos)
		penalty += 1;
	if (name.find("USA") != std::string::npos)
		return penalty + 0;
	if (name.find("World") != std::string::npos)
		return penalty + 2;
	if (name.find("Europe") != std::string::npos)
		return penalty + 4;
	if (name.find("Japan") != std::string::npos)
		return penalty + 6;
	return penalty + 8;
}

uint64_t CrcKey(System s, uint32_t crc)
{
	return (uint64_t(s) << 32) | crc;
}

std::string NameKey(System s, const std::string& k)
{
	return std::string(1, char('A' + int(s))) + k;
}

struct Db
{
	size_t entries = 0;
	std::unordered_map<uint64_t, std::string> by_crc;
	std::unordered_map<std::string, std::string> exact; // sys + lower-case name -> name
	std::unordered_map<std::string, std::string> loose; // sys + Key -> best name
	std::unordered_map<uint64_t, std::string> extra_by_crc; // sys + CRC -> the name of a dump No-Intro does not list
	dbart::Table covers[size_t(System::Count)]; // per system: games an online database has a cover for
	Db()
	{
		const char* dbart_begin = genplus_dbart_begin;
		const char* dbart_end = genplus_dbart_end;
#ifndef __PROSPERO__
		// the host tests give a table of their own (the built-in one may be empty until tools/make_db_art.py runs)
		std::string host_dbart;
		if (const char* path = getenv("GENPLUS_HOST_DBART"))
			if (FILE* f = fopen(path, "rb"))
			{
				char buf[4096];
				size_t n;
				while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
					host_dbart.append(buf, n);
				fclose(f);
				dbart_begin = host_dbart.data();
				dbart_end = host_dbart.data() + host_dbart.size();
			}
#endif
		for (int i = 0; i < int(System::Count); i++)
			dbart::Parse(dbart_begin, dbart_end, kSystems[i].id, covers[i]);
		// system<TAB>CRC32<TAB>name
		for (const char* q = genplus_extracrc_begin; q < genplus_extracrc_end;)
		{
			const char* nl = static_cast<const char*>(memchr(q, '\n', size_t(genplus_extracrc_end - q)));
			if (!nl)
				nl = genplus_extracrc_end;
			const std::string line(q, size_t(nl - q));
			q = nl + 1;
			const size_t t1 = line.find('\t');
			if (line.empty() || line[0] == '#' || t1 == std::string::npos || line.size() < t1 + 11 || line[t1 + 9] != '\t')
				continue;
			for (int i = 0; i < int(System::Count); i++)
				if (line.compare(0, t1, kSystems[i].id) == 0 && strlen(kSystems[i].id) == t1)
					extra_by_crc.emplace(CrcKey(System(i), uint32_t(strtoul(line.substr(t1 + 1, 8).c_str(), nullptr, 16))),
						line.substr(t1 + 10));
		}
		const char* p = genplus_gamedb_begin;
		const char* end = genplus_gamedb_end;
		while (p < end)
		{
			const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
			if (!nl)
				nl = end;
			Line(std::string(p, size_t(nl - p)));
			p = nl + 1;
		}
	}
	void Line(const std::string& line)
	{
		if (line.empty() || line[0] == '#')
			return;
		const size_t t1 = line.find('\t');
		const size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
		if (t2 == std::string::npos)
			return;
		const std::string id = line.substr(0, t1);
		const std::string crc = line.substr(t1 + 1, t2 - t1 - 1);
		const std::string name = line.substr(t2 + 1);
		System sys = System::Count;
		for (int i = 0; i < int(System::Count); i++)
			if (id == kSystems[i].id)
				sys = System(i);
		if (sys == System::Count || name.empty())
			return;
		entries++;
		if (crc != "-")
			by_crc.emplace(CrcKey(sys, uint32_t(strtoul(crc.c_str(), nullptr, 16))), name);
		exact.emplace(NameKey(sys, Lower(name)), name);
		const std::string k = NameKey(sys, Key(name));
		auto it = loose.find(k);
		if (it == loose.end() || RegionRank(name) < RegionRank(it->second))
			loose[k] = name;
	}
};

const Db& D()
{
	static const Db db;
	return db;
}

// ---- CRC cache: path \t size \t mtime \t whole \t body ----
// One per process: the scan and the pass that names network ROMs (NetNaming) share it, each under the lock.
struct CrcCache
{
	std::mutex lock;
	std::unordered_map<std::string, std::string> lines; // path -> "size\tmtime\twhole\tbody"
	std::unordered_map<std::string, bool> seen; // the paths this scan looked up
	bool dirty = false;
	bool loaded = false;
	std::string file;
	// A new scan: reads the file the first time, forgets which paths the last scan saw.
	void Begin()
	{
		std::lock_guard<std::mutex> g(lock);
		seen.clear();
		LoadLocked();
	}
	// Reads the file the first time, keeps the scan's list (DescribeGame, a game being loaded).
	void Ensure()
	{
		std::lock_guard<std::mutex> g(lock);
		LoadLocked();
	}
	void LoadLocked()
	{
		if (loaded)
			return;
		loaded = true;
		file = OrbisDir("covers") + "/crc-cache.txt";
		FILE* f = fopen(file.c_str(), "r");
		if (!f)
			return;
		char line[2048];
		while (fgets(line, sizeof(line), f))
		{
			line[strcspn(line, "\r\n")] = 0;
			char* tab = strchr(line, '\t');
			if (tab)
			{
				*tab = 0;
				lines[line] = tab + 1;
			}
		}
		fclose(f);
	}
	// prune: the scan's end drops the entries of files that are gone; the naming pass (mid-scan, maybe) keeps all.
	void Save(bool prune)
	{
		std::lock_guard<std::mutex> g(lock);
		// entries of files that are gone are dropped (a USB drive that isn't plugged in, or a share that is not
		// connected, keeps its own)
		for (auto it = lines.begin(); prune && it != lines.end();)
		{
			const std::string& key = it->first;
			const bool network = netfs_is_network_path(key.c_str()) || (key.rfind("sys:", 0) == 0 && netfs_is_network_path(key.c_str() + 4));
			if (!seen.count(key) && key.rfind("/mnt/", 0) != 0 && !network)
			{
				it = lines.erase(it);
				dirty = true;
			}
			else
				++it;
		}
		if (!dirty)
			return;
		const std::string tmp = file + ".part";
		FILE* f = fopen(tmp.c_str(), "w");
		if (!f)
			return;
		for (const auto& kv : lines)
			fprintf(f, "%s\t%s\n", kv.first.c_str(), kv.second.c_str());
		bool ok = ferror(f) == 0;
		ok = fclose(f) == 0 && ok;
		if (!ok || rename(tmp.c_str(), file.c_str()) != 0)
		{
			remove(tmp.c_str());
			return;
		}
		dirty = false;
	}
	void Put(const std::string& path, const std::string& val)
	{
		std::lock_guard<std::mutex> g(lock);
		lines[path] = val;
		dirty = true;
	}
};

CrcCache& SharedCrcCache()
{
	static CrcCache* c = new CrcCache; // never freed: the naming pass may still run at exit
	return *c;
}

// read: false for a ROM on a share, where only a CRC already in the cache is used (reading every ROM over the
// network is what makes a scan slow). known (if given): the cache holds an answer for this file as it is now,
// "could not be read" included.
bool CachedCrc(CrcCache& cache, const std::string& path, uint32_t* whole, uint32_t* body, bool read = true, bool* known = nullptr)
{
	if (known)
		*known = false;
	{
		std::lock_guard<std::mutex> g(cache.lock);
		cache.seen[path] = true;
	}
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0)
		return false;
	char key[64];
	snprintf(key, sizeof(key), "%lld\t%lld\t", (long long)st.st_size, (long long)st.st_mtime);
	{
		std::lock_guard<std::mutex> g(cache.lock);
		auto it = cache.lines.find(path);
		if (it != cache.lines.end() && it->second.compare(0, strlen(key), key) == 0)
		{
			if (known)
				*known = true;
			if (it->second.compare(strlen(key), std::string::npos, "fail") == 0)
				return false; // couldn't be read last time, and hasn't changed since
			unsigned a = 0, b = 0;
			if (sscanf(it->second.c_str() + strlen(key), "%x\t%x", &a, &b) == 2)
			{
				*whole = a;
				*body = b;
				return true;
			}
		}
	}
	if (!read)
		return false;
	char val[96];
	if (!RomCrc32(path, whole, body))
	{
		// unreadable (or an empty zip): remembered, so the next scans don't read it again until it changes
		snprintf(val, sizeof(val), "%sfail", key);
		cache.Put(path, val);
		return false;
	}
	snprintf(val, sizeof(val), "%s%08X\t%08X", key, *whole, *body);
	cache.Put(path, val);
	return true;
}

// The offset of the ROM data after a header we know, or 0.
size_t HeaderSize(const std::vector<uint8_t>& d, const std::string& ext)
{
	if ((ext == ".md" || ext == ".gen" || ext == ".bin" || ext == ".sms" || ext == ".gg" || ext == ".sg") &&
		(d.size() & 0x3ff) == 0x200)
		return 512; // copier header
	return 0;
}

// The track files .cue sheets of this folder name (FILE "Game (Track 01).bin" BINARY): they belong to a disc,
// not to the shelf as Mega Drive .bin ROMs.
std::vector<std::string> CueTracks(const std::string& dir)
{
	std::vector<std::string> tracks;
	DIR* d = opendir(dir.c_str());
	if (!d)
		return tracks;
	while (dirent* e = readdir(d))
	{
		if (ExtOf(e->d_name) != ".cue")
			continue;
		FILE* f = fopen((dir + "/" + e->d_name).c_str(), "r");
		if (!f)
			continue;
		char line[1024];
		while (fgets(line, sizeof(line), f))
		{
			const char* p = line;
			while (*p == ' ' || *p == '\t')
				p++;
			if (strncasecmp(p, "FILE", 4) != 0)
				continue;
			// FILE "Game (Track 01).bin" BINARY, or FILE track01.bin BINARY; a path keeps only its file name
			p += 4;
			while (*p == ' ' || *p == '\t')
				p++;
			std::string file;
			if (*p == '"')
			{
				const char* q2 = strchr(p + 1, '"');
				if (q2)
					file.assign(p + 1, q2);
			}
			else
			{
				const char* end = p;
				while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')
					end++;
				file.assign(p, end);
			}
			const size_t slash = file.find_last_of("/\\");
			if (slash != std::string::npos)
				file = file.substr(slash + 1);
			if (!file.empty())
				tracks.push_back(Lower(file));
		}
		fclose(f);
	}
	closedir(d);
	return tracks;
}

// A zip's system (the ROM inside it), from the cache while the zip hasn't changed: opening every zip at every
// start is slow on a USB drive with a big library. "sys:<path>" -> "size\tmtime\t<system>".
// read: false for a zip on a share (*known then says whether the cache had it; NetNaming opens it later).
bool CachedArchiveSystem(CrcCache& cache, const std::string& path, const struct stat& st, System* out, bool read = true,
	bool* known = nullptr)
{
	const std::string key = "sys:" + path;
	char stamp[64];
	snprintf(stamp, sizeof(stamp), "%lld\t%lld\t", (long long)st.st_size, (long long)st.st_mtime);
	if (known)
		*known = true;
	{
		std::lock_guard<std::mutex> g(cache.lock);
		cache.seen[key] = true;
		auto it = cache.lines.find(key);
		if (it != cache.lines.end() && it->second.compare(0, strlen(stamp), stamp) == 0)
		{
			const std::string v = it->second.substr(strlen(stamp));
			if (v == "none")
				return false;
			const int n = atoi(v.c_str());
			if (n >= 0 && n < int(System::Count) && System(n) != System::SegaCd)
			{
				*out = System(n);
				return true;
			}
		}
	}
	if (!read)
	{
		if (known)
			*known = false;
		return false;
	}
	const bool ok = ArchiveSystem(path, out);
	cache.Put(key, std::string(stamp) + (ok ? std::to_string(int(*out)) : std::string("none")));
	return ok;
}

// The system a zip on a share most likely holds before NetNaming has opened it: its folder's name (roms/GameGear/,
// "Game Gear", "Master System"...), else the Mega Drive.
System GuessSystemFromFolder(const std::string& path)
{
	std::string dir = path.substr(0, path.find_last_of('/'));
	for (int depth = 0; depth < 3 && !dir.empty(); depth++)
	{
		std::string name = dir.substr(dir.find_last_of('/') + 1), key;
		for (char c : name)
			if (isalnum(uint8_t(c)))
				key += char(tolower(uint8_t(c)));
		if (key.find("gamegear") != std::string::npos || key == "gg")
			return System::Gg;
		if (key.find("mastersystem") != std::string::npos || key.find("markiii") != std::string::npos || key == "sms")
			return System::Sms;
		if (key.find("sg1000") != std::string::npos)
			return System::Sg;
		if (key.find("megadrive") != std::string::npos || key.find("genesis") != std::string::npos || key == "md")
			return System::Md;
		dir = dir.substr(0, dir.find_last_of('/'));
	}
	return System::Md;
}

void Walk(const std::string& dir, int depth, bool usb, bool net, std::vector<GameInfo>& out, CrcCache& cache,
	const ScanProgress& progress)
{
	const std::vector<std::string> tracks = CueTracks(dir);
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	while (dirent* e = readdir(d))
	{
		if (e->d_name[0] == '.')
			continue;
		const std::string path = dir + "/" + e->d_name;
		struct stat st = {};
		if (stat(path.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
		{
			if (depth < 4)
				Walk(path, depth + 1, usb, net, out, cache, progress);
			continue;
		}
		if (!S_ISREG(st.st_mode))
			continue; // a FIFO or a device would block the CRC read (and the shelf) forever
		const std::string name = e->d_name;
		const std::string ext = ExtOf(name);
		if (ext.empty())
			continue;
		if (std::find(tracks.begin(), tracks.end(), Lower(name)) != tracks.end())
			continue; // a disc's track
		GameInfo g;
		if (ext == ".zip")
		{
			// a zip on a share is not opened by the scan: the cache knows its system, or its folder's name says it
			// until NetNaming has looked inside
			bool known = true;
			if (!CachedArchiveSystem(cache, path, st, &g.system, !net, &known))
			{
				if (known)
					continue;
				g.system = GuessSystemFromFolder(path);
				g.crc_pending = true;
			}
		}
		else if (!SystemForExt(ext, &g.system))
			continue;
		g.path = path;
		g.file_base = name.substr(0, name.size() - ext.size());
		g.ext = ext;
		g.on_usb = usb;
		g.on_network = net;
		out.push_back(std::move(g));
		if (progress)
			progress(0, out.size(), 0);
	}
	closedir(d);
}

// A ROM no name or CRC identified keeps its header's title for the cover and border lookup (Snes9x PS5 2.7's HeaderHint).
// Cached like the CRC (under the path plus "|hdr", with the size and mtime), so each such ROM is opened once:
// "size\tmtime\tTITLE\tJAPAN" or "...\tnone".
void LoadHeaderHint(CrcCache& cache, GameInfo& g)
{
	struct stat st = {};
	if (stat(g.path.c_str(), &st) != 0)
		return;
	const std::string key = g.path + "|hdr";
	char stamp[64];
	snprintf(stamp, sizeof(stamp), "%lld\t%lld\t", (long long)st.st_size, (long long)st.st_mtime);
	std::string val;
	bool cached = false;
	{
		std::lock_guard<std::mutex> lk(cache.lock); // the network naming pass uses the cache too
		cache.seen[key] = true;
		auto it = cache.lines.find(key);
		if (it != cache.lines.end() && it->second.compare(0, strlen(stamp), stamp) == 0)
		{
			val = it->second.substr(strlen(stamp));
			cached = true;
		}
	}
	if (!cached)
	{
		// a ROM on a share is not opened during the scan (that is what makes a network scan slow): no header hint
		if (g.on_network)
			return;
		romheader::Info info;
		val = ReadRomHeader(g.path, g.system, &info) ? info.title + "\t" + (info.japan() ? "1" : "0") : "none";
		cache.Put(key, stamp + val);
	}
	const size_t tab = val.find('\t');
	if (tab == std::string::npos)
		return;
	g.header_title = val.substr(0, tab);
	g.header_japan = val.compare(tab + 1, std::string::npos, "1") == 0;
}

// Names one game, in the order fe_games.h describes: its file name, its CRC, the file name loosely; a ROM none of that
// names gets what its CRC is called in the extra table and its header's title. ScanGames and DescribeGame (a game being
// loaded) both come through here, so the shelf, the cover and the border name a game alike.
void NameGame(CrcCache& cache, GameInfo& g)
{
	System found = g.system;
	g.nointro = gamedb::Exact(g.system, g.file_base, &found);
	uint32_t whole = 0, body = 0;
	bool have_crc = false;
	if (g.nointro.empty())
	{
		bool known = false;
		const bool have = CachedCrc(cache, g.path, &whole, &body, !g.on_network, &known);
		// a cartridge on a share is named from its file name for now: NetNaming reads its CRC (a disc has none)
		if (g.on_network && !known && g.system != System::SegaCd)
			g.crc_pending = true;
		if (have)
		{
			have_crc = true;
			g.crc = body;
			g.nointro = gamedb::ByCrc(g.system, whole, &found);
			if (g.nointro.empty() && body != whole)
				g.nointro = gamedb::ByCrc(g.system, body, &found);
			g.name_by_crc = !g.nointro.empty();
		}
	}
	if (g.nointro.empty())
		g.nointro = gamedb::Loose(g.system, g.file_base, &found);
	if (!g.nointro.empty())
		g.system = found;
	else
	{
		if (have_crc)
		{
			g.crc_name = gamedb::ByExtraCrc(g.system, whole);
			if (g.crc_name.empty() && body != whole)
				g.crc_name = gamedb::ByExtraCrc(g.system, body);
		}
		LoadHeaderHint(cache, g);
	}
	g.title = g.nointro.empty() ? g.file_base : gamedb::Title(g.nointro);
	g.region = gamedb::Region(g.nointro);
}

template <typename F>
std::string Search(System s, System* found_in, F&& find)
{
	for (System g : Group(s))
	{
		std::string n = find(g);
		if (!n.empty())
		{
			if (found_in)
				*found_in = g;
			return n;
		}
	}
	return "";
}
} // namespace

const SystemInfo& Info(System s)
{
	return kSystems[size_t(s) < size_t(System::Count) ? size_t(s) : 0];
}

const char* FamilyName(Family f)
{
	switch (f)
	{
		case Family::All: return "All games";
		case Family::Md: return "Mega Drive";
		case Family::SegaCd: return "Sega CD";
		case Family::Sms: return "Master System";
		case Family::Gg: return "Game Gear";
		default: return "?";
	}
}

bool SystemForExt(const std::string& ext, System* out)
{
	const std::string e = Lower(ext);
	for (const ExtSys& x : kExts)
		if (e == x.ext)
		{
			*out = x.sys;
			return true;
		}
	return false;
}

namespace
{
// Calls fn(name) for each file of the zip until it returns true; the zip is then positioned on that file.
template <typename F>
bool FindInZip(unzFile z, F&& fn)
{
	for (int r = unzGoToFirstFile(z); r == UNZ_OK; r = unzGoToNextFile(z))
	{
		unz_file_info info;
		char name[512];
		// minizip ends the name with '\0' only when it fits: keep the last byte for it, skip names that don't fit
		name[sizeof(name) - 1] = '\0';
		if (unzGetCurrentFileInfo(z, &info, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK ||
			info.size_filename >= sizeof(name) - 1)
			continue;
		if (fn(std::string(name)))
			return true;
	}
	return false;
}
} // namespace

bool ArchiveSystem(const std::string& path, System* out)
{
	unzFile z = unzOpen(path.c_str());
	if (!z)
		return false;
	const bool ok = FindInZip(z, [&](const std::string& name) {
		System s;
		if (!SystemForExt(ExtOf(name), &s) || s == System::SegaCd)
			return false;
		*out = s;
		return true;
	});
	unzClose(z);
	return ok;
}

// The CRC a zip stores for its ROM, when the ROM has no copier header to skip (its size isn't 512 bytes past a
// multiple of 1 KiB): true and the name inside; false when the data has to be read.
bool StoredZipCrc(const std::string& path, std::string* inner, uint32_t* crc)
{
	unzFile z = unzOpen(path.c_str());
	if (!z)
		return false;
	bool ok = FindInZip(z, [&](const std::string& name) {
		System s;
		if (!SystemForExt(ExtOf(name), &s) || s == System::SegaCd)
			return false;
		*inner = name;
		return true;
	});
	if (ok)
	{
		unz_file_info info;
		ok = unzGetCurrentFileInfo(z, &info, nullptr, 0, nullptr, 0, nullptr, 0) == UNZ_OK &&
			 info.uncompressed_size > 0 && info.uncompressed_size <= 16u * 1024 * 1024 &&
			 (info.uncompressed_size & 0x3ff) != 0x200;
		if (ok)
			*crc = uint32_t(info.crc);
	}
	unzClose(z);
	return ok;
}

bool ReadFromZip(const std::string& path, std::string* inner, std::vector<uint8_t>* data)
{
	unzFile z = unzOpen(path.c_str());
	if (!z)
		return false;
	bool ok = FindInZip(z, [&](const std::string& name) {
		System s;
		if (!SystemForExt(ExtOf(name), &s) || s == System::SegaCd)
			return false;
		*inner = name;
		return true;
	});
	if (ok)
	{
		unz_file_info info;
		ok = unzGetCurrentFileInfo(z, &info, nullptr, 0, nullptr, 0, nullptr, 0) == UNZ_OK &&
			 info.uncompressed_size > 0 && info.uncompressed_size <= 16u * 1024 * 1024 /* the core takes 10 MiB at most */ && unzOpenCurrentFile(z) == UNZ_OK;
		if (ok)
		{
			data->resize(info.uncompressed_size);
			ok = unzReadCurrentFile(z, data->data(), unsigned(data->size())) == int(data->size());
			unzCloseCurrentFile(z);
		}
	}
	unzClose(z);
	return ok;
}

namespace gamedb
{
size_t Count()
{
	return D().entries;
}

std::string ByCrc(System s, uint32_t crc, System* found_in)
{
	return Search(s, found_in, [&](System g) {
		auto it = D().by_crc.find(CrcKey(g, crc));
		return it == D().by_crc.end() ? std::string() : it->second;
	});
}

std::string Exact(System s, const std::string& name, System* found_in)
{
	return Search(s, found_in, [&](System g) {
		auto it = D().exact.find(NameKey(g, Lower(name)));
		return it == D().exact.end() ? std::string() : it->second;
	});
}

std::string Loose(System s, const std::string& file_base, System* found_in)
{
	return Search(s, found_in, [&](System g) {
		auto it = D().loose.find(NameKey(g, Key(file_base)));
		return it == D().loose.end() ? std::string() : it->second;
	});
}

std::string Title(const std::string& nointro)
{
	std::string t = nointro.substr(0, nointro.find(" ("));
	// "Legend of Zelda, The - A Link to the Past" -> "The Legend of Zelda - A Link to the Past"
	static const char* const arts[] = {", The", ", A", ", An"};
	for (const char* a : arts)
	{
		const size_t pos = t.find(a);
		if (pos != std::string::npos)
		{
			const size_t end = pos + strlen(a);
			if (end == t.size() || t.compare(end, 3, " - ") == 0 || t[end] == ':')
			{
				const std::string art = std::string(a + 2);
				t = art + " " + t.substr(0, pos) + t.substr(end);
				break;
			}
		}
	}
	return t;
}

std::string ByExtraCrc(System s, uint32_t crc)
{
	return Search(s, nullptr, [&](System g) {
		auto it = D().extra_by_crc.find(CrcKey(g, crc));
		return it == D().extra_by_crc.end() ? std::string() : it->second;
	});
}

std::string DatabaseCoverUrls(const GameInfo& game)
{
	// the host tests point both at a local server
	const char* hasheous = getenv("GENPLUS_DBART_HASHEOUS");
	const char* thegamesdb = getenv("GENPLUS_DBART_THEGAMESDB");
	const dbart::Table& t = D().covers[size_t(game.system) < size_t(System::Count) ? size_t(game.system) : 0];
	const dbart::Row* row = dbart::Find(t, game.crc, game.nointro.empty() ? game.file_base : game.nointro);
	if (!row && game.nointro != game.file_base)
		row = dbart::Find(t, 0, game.file_base);
	if (!row && !game.crc_name.empty())
		row = dbart::Find(t, 0, game.crc_name);
	if (!row)
		return std::string();
	return dbart::Urls(*row, hasheous && *hasheous ? hasheous : "https://hasheous.org/api/v1/images/",
		thegamesdb && *thegamesdb ? thegamesdb : "https://cdn.thegamesdb.net/images/original/boxart/front/");
}

std::string Region(const std::string& nointro)
{
	const size_t a = nointro.find(" (");
	if (a == std::string::npos)
		return "";
	const size_t b = nointro.find(')', a);
	return b == std::string::npos ? "" : nointro.substr(a + 2, b - a - 2);
}
} // namespace gamedb

bool RomCrc32(const std::string& path, uint32_t* whole, uint32_t* body, std::string* inner_ext, const std::atomic<bool>* cancel)
{
	auto stopped = [cancel] { return cancel && cancel->load(); };
	std::string ext = ExtOf(path);
	std::vector<uint8_t> data;
	System sys;
	if (ext == ".zip")
	{
		// the zip keeps its ROM's CRC: read only when a 512-byte copier header has to come off for the body's
		std::string inner;
		uint32_t stored = 0;
		if (StoredZipCrc(path, &inner, &stored))
		{
			if (inner_ext)
				*inner_ext = ExtOf(inner);
			*whole = *body = stored;
			return true;
		}
		if (!ReadFromZip(path, &inner, &data))
			return false;
		ext = ExtOf(inner);
	}
	else if (!SystemForExt(ext, &sys) || sys == System::SegaCd)
		return false; // a disc: named by its file name only
	else
	{
		FILE* f = fopen(path.c_str(), "rb");
		if (!f)
			return false;
		fseek(f, 0, SEEK_END);
		const long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (size <= 0 || size > 64L * 1024 * 1024)
		{
			fclose(f);
			return false;
		}
		data.resize(size_t(size));
		// in pieces, so a cancel (NetNaming::Stop) does not wait for a whole ROM to come over the network
		size_t n = 0;
		while (n < data.size() && !stopped())
		{
			const size_t got = fread(data.data() + n, 1, std::min<size_t>(1 << 20, data.size() - n), f);
			if (got == 0)
				break;
			n += got;
		}
		fclose(f);
		if (n != data.size())
			return false;
	}
	if (inner_ext)
		*inner_ext = ext;
	*whole = Crc(data.data(), data.size());
	const size_t skip = HeaderSize(data, ext);
	*body = skip > 0 && skip < data.size() ? Crc(data.data() + skip, data.size() - skip) : *whole;
	return true;
}

void PrepareFolders()
{
	const std::string roms = OrbisDir("roms"), covers = OrbisDir("covers"), borders = OrbisDir("borders");
	for (const SystemInfo& s : kSystems)
	{
		OrbisMkdirs(roms + "/" + s.folder);
		OrbisMkdirs(covers + "/" + s.folder);
		OrbisMkdirs(borders + "/" + s.folder); // the border pack (fe_border.h)
	}
	// covers cached by 1.0's first builds: covers/"md - Sonic The Hedgehog (USA, Europe).png" (and .missing)
	DIR* d = opendir(covers.c_str());
	if (!d)
		return;
	std::vector<std::pair<std::string, std::string>> moves;
	while (dirent* e = readdir(d))
	{
		const std::string name = e->d_name;
		for (const SystemInfo& s : kSystems)
		{
			const std::string tag = std::string(s.id) + " - ";
			if (name.size() > tag.size() && name.compare(0, tag.size(), tag) == 0)
			{
				moves.push_back({covers + "/" + name, covers + "/" + s.folder + "/" + name.substr(tag.size())});
				break;
			}
		}
	}
	closedir(d);
	for (const auto& m : moves)
	{
		struct stat st = {};
		if (stat(m.second.c_str(), &st) == 0)
			OrbisLog("[games] %s kept: %s is there already", m.first.c_str(), m.second.c_str()); // never overwritten
		else if (rename(m.first.c_str(), m.second.c_str()) != 0)
			OrbisLog("[games] can't move %s to %s", m.first.c_str(), m.second.c_str());
	}
	if (!moves.empty())
		OrbisLog("[games] moved %zu cached cover file(s) into covers/<system>/", moves.size());
}

// ---- the naming pass for ROMs on shares ----

struct NetNaming::Impl
{
	std::mutex lock;
	std::deque<std::string> todo;
	std::set<std::string> tried; // every path ever queued: a ROM is read once per run
	std::atomic<bool> stop{false};
	std::atomic<int> resolved{0};
	std::atomic<bool> busy{false};
	ps5::BigThread thread;

	void Run()
	{
		CrcCache& cache = SharedCrcCache();
		int named = 0, since_save = 0;
		for (;;)
		{
			std::string path;
			{
				std::unique_lock<std::mutex> g(lock);
				if (stop.load() || todo.empty() || netfs_realtime())
				{
					busy = false;
					break;
				}
				path = todo.front();
				todo.pop_front();
			}
			struct stat st = {};
			if (stat(path.c_str(), &st) != 0)
				continue; // the share went away; the next scan will list it again
			if (ExtOf(path) == ".zip")
			{
				System sys;
				CachedArchiveSystem(cache, path, st, &sys, true); // the system of the ROM inside, for the next scan
			}
			char key[64];
			snprintf(key, sizeof(key), "%lld\t%lld\t", (long long)st.st_size, (long long)st.st_mtime);
			uint32_t whole = 0, body = 0;
			const bool ok = RomCrc32(path, &whole, &body, nullptr, &stop);
			if (stop.load())
				break; // cancelled half way: nothing recorded
			char val[96];
			if (ok)
				snprintf(val, sizeof(val), "%s%08X\t%08X", key, whole, body);
			else
				snprintf(val, sizeof(val), "%sfail", key);
			cache.Put(path, val);
			named++;
			resolved++;
			if (++since_save >= 20)
			{
				cache.Save(false);
				since_save = 0;
			}
		}
		cache.Save(false);
		busy = false;
		OrbisLog("[games] network CRC pass: %d ROM(s) read%s", named, stop.load() ? " (stopped)" : "");
	}
};

NetNaming::NetNaming() : m_i(new Impl) {}

NetNaming::~NetNaming()
{
	Stop();
}

void NetNaming::Add(const std::vector<GameInfo>& games)
{
	size_t added = 0;
	{
		std::lock_guard<std::mutex> g(m_i->lock);
		for (const GameInfo& game : games)
			if (game.crc_pending && m_i->tried.insert(game.path).second)
			{
				m_i->todo.push_back(game.path);
				added++;
			}
	}
	if (!added)
		return;
	OrbisLog("[games] naming %zu network ROM(s) by CRC in the background", added);
	if (m_i->busy.load())
		return; // the running pass takes the new ones too
	m_i->thread.join();
	m_i->stop = false;
	m_i->busy = true;
	m_i->thread = ps5::BigThread([this] { m_i->Run(); }, 256 * 1024);
}

void NetNaming::Stop()
{
	m_i->stop = true;
	m_i->thread.join();
	m_i->busy = false;
}

int NetNaming::Resolved() const
{
	return m_i->resolved.load();
}

bool NetNaming::Busy() const
{
	return m_i->busy.load();
}

std::vector<std::string> RomRoots()
{
	std::vector<std::string> roots = OrbisRomRoots();
	for (const std::string& r : netfs_mount_points())
		roots.push_back(r);
	return roots;
}

std::vector<GameInfo> ScanGames(const ScanProgress& progress)
{
	PrepareFolders();
	std::vector<GameInfo> games;
	CrcCache& cache = SharedCrcCache();
	cache.Begin();
	for (const std::string& root : RomRoots())
		Walk(root, 0, root.rfind("/mnt/", 0) == 0, netfs_is_network_path(root.c_str()), games, cache, progress);

	size_t identified = 0;
	for (GameInfo& g : games)
	{
		if (progress)
			progress(1, identified++, games.size());
		NameGame(cache, g);
	}
	cache.Save(true);

	std::sort(games.begin(), games.end(), [](const GameInfo& a, const GameInfo& b) {
		const int c = strcasecmp(a.title.c_str(), b.title.c_str());
		if (c != 0)
			return c < 0;
		if (a.system != b.system)
			return a.system < b.system;
		return a.path < b.path;
	});
	OrbisLog("[games] %zu ROM(s); table of %zu names", games.size(), gamedb::Count());
	// each game once per scan is a lot of boot.log for a big library: the first 200
	constexpr size_t kLogged = 200;
	for (size_t i = 0; i < games.size() && i < kLogged; i++)
	{
		const GameInfo& g = games[i];
		OrbisLog("[games]   %s [%s] -> \"%s\"%s%s%s%s%s", g.path.c_str(), Info(g.system).id, g.nointro.c_str(),
			g.name_by_crc ? " (by CRC)" : "", g.crc_name.empty() ? "" : " (dump: ", g.crc_name.c_str(), g.crc_name.empty() ? "" : ")",
			g.header_title.empty() ? "" : (" (header: " + g.header_title + ")").c_str());
	}
	if (games.size() > kLogged)
		OrbisLog("[games]   ... and %zu more", games.size() - kLogged);
	return games;
}
GameInfo DescribeGame(const std::string& path)
{
	GameInfo g;
	g.path = path;
	const size_t slash = path.find_last_of('/');
	const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	const std::string ext = ExtOf(name);
	g.file_base = name.substr(0, name.size() - ext.size());
	g.ext = ext;
	g.on_usb = path.rfind("/mnt/", 0) == 0;
	g.on_network = netfs_is_network_path(path.c_str());
	if (ext == ".zip")
	{
		if (!ArchiveSystem(path, &g.system))
			g.system = System::Md;
	}
	else if (!SystemForExt(ext, &g.system))
		g.system = System::Md;
	CrcCache& cache = SharedCrcCache();
	cache.Ensure();
	NameGame(cache, g);
	cache.Save(false); // only what this game added: the scan drops the entries of files that are gone
	return g;
}

namespace
{
// The first FILE of a .cue sheet, as a path beside it.
std::string CueFirstTrack(const std::string& cue)
{
	FILE* f = fopen(cue.c_str(), "r");
	if (!f)
		return "";
	char line[1024];
	std::string file;
	while (file.empty() && fgets(line, sizeof(line), f))
	{
		const char* p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (strncasecmp(p, "FILE", 4) != 0)
			continue;
		p += 4;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '"')
		{
			const char* q = strchr(p + 1, '"');
			if (q)
				file.assign(p + 1, q);
		}
		else
		{
			const char* e = p;
			while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n')
				e++;
			file.assign(p, e);
		}
	}
	fclose(f);
	const size_t s = file.find_last_of("/\\");
	if (s != std::string::npos)
		file = file.substr(s + 1);
	return file.empty() ? "" : cue.substr(0, cue.find_last_of('/') + 1) + file;
}
} // namespace

bool ReadRomHeader(const std::string& path, System system, romheader::Info* info)
{
	const std::string ext = ExtOf(path);
	if (system == System::SegaCd)
	{
		// a disc: its first data track (a .chd is compressed: not read here)
		const std::string track = ext == ".cue" ? CueFirstTrack(path) : ext == ".iso" ? path : std::string();
		FILE* f = track.empty() ? nullptr : fopen(track.c_str(), "rb");
		if (!f)
			return false;
		const bool ok = romheader::ReadDisc(
			[f](uint64_t at, uint8_t* out, size_t n) { return fseek(f, long(at), SEEK_SET) == 0 && fread(out, 1, n, f) == n; }, info);
		fclose(f);
		return ok;
	}
	if (system != System::Md)
		return false; // the Master System's, the Game Gear's and the SG-1000's headers have no title
	if (ext == ".zip")
	{
		std::string inner;
		std::vector<uint8_t> data;
		if (!ReadFromZip(path, &inner, &data))
			return false;
		return romheader::ReadCart(
			[&data](uint64_t at, uint8_t* out, size_t n) {
				if (at + n > data.size())
					return false;
				memcpy(out, data.data() + at, n);
				return true;
			},
			data.size(), info);
	}
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	const uint64_t total = uint64_t(ftell(f));
	const bool ok = romheader::ReadCart(
		[f](uint64_t at, uint8_t* out, size_t n) { return fseek(f, long(at), SEEK_SET) == 0 && fread(out, 1, n, f) == n; }, total,
		info);
	fclose(f);
	return ok;
}
} // namespace fe

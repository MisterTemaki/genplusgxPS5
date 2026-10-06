// Genesis Plus GX PS5 frontend: the game library (fe_games.h).
// SPDX-License-Identifier: MIT

#include "fe_games.h"

#include "OrbisPaths.h"

#include "third_party/unzip.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
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
	Db()
	{
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
struct CrcCache
{
	std::unordered_map<std::string, std::string> lines; // path -> "size\tmtime\twhole\tbody"
	bool dirty = false;
	std::string file;
	void Load()
	{
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
	void Save()
	{
		if (!dirty)
			return;
		const std::string tmp = file + ".part";
		FILE* f = fopen(tmp.c_str(), "w");
		if (!f)
			return;
		for (const auto& kv : lines)
			fprintf(f, "%s\t%s\n", kv.first.c_str(), kv.second.c_str());
		fclose(f);
		rename(tmp.c_str(), file.c_str());
		dirty = false;
	}
};

bool CachedCrc(CrcCache& cache, const std::string& path, uint32_t* whole, uint32_t* body)
{
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0)
		return false;
	char key[64];
	snprintf(key, sizeof(key), "%lld\t%lld\t", (long long)st.st_size, (long long)st.st_mtime);
	auto it = cache.lines.find(path);
	if (it != cache.lines.end() && it->second.compare(0, strlen(key), key) == 0)
	{
		unsigned a = 0, b = 0;
		if (sscanf(it->second.c_str() + strlen(key), "%x\t%x", &a, &b) == 2)
		{
			*whole = a;
			*body = b;
			return true;
		}
	}
	if (!RomCrc32(path, whole, body))
		return false;
	char val[96];
	snprintf(val, sizeof(val), "%s%08X\t%08X", key, *whole, *body);
	cache.lines[path] = val;
	cache.dirty = true;
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
			const char* q1 = strchr(p, '"');
			const char* q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
			if (q1 && q2)
				tracks.push_back(Lower(std::string(q1 + 1, q2)));
		}
		fclose(f);
	}
	closedir(d);
	return tracks;
}

void Walk(const std::string& dir, int depth, bool usb, std::vector<GameInfo>& out)
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
		if (OrbisIsDir(path))
		{
			if (depth < 4)
				Walk(path, depth + 1, usb, out);
			continue;
		}
		const std::string name = e->d_name;
		const std::string ext = ExtOf(name);
		if (ext.empty())
			continue;
		if (std::find(tracks.begin(), tracks.end(), Lower(name)) != tracks.end())
			continue; // a disc's track
		GameInfo g;
		if (ext == ".zip")
		{
			if (!ArchiveSystem(path, &g.system))
				continue;
		}
		else if (!SystemForExt(ext, &g.system))
			continue;
		g.path = path;
		g.file_base = name.substr(0, name.size() - ext.size());
		g.ext = ext;
		g.on_usb = usb;
		out.push_back(std::move(g));
	}
	closedir(d);
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
		if (unzGetCurrentFileInfo(z, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
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
			 info.uncompressed_size > 0 && info.uncompressed_size <= 64u * 1024 * 1024 && unzOpenCurrentFile(z) == UNZ_OK;
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

std::string Region(const std::string& nointro)
{
	const size_t a = nointro.find(" (");
	if (a == std::string::npos)
		return "";
	const size_t b = nointro.find(')', a);
	return b == std::string::npos ? "" : nointro.substr(a + 2, b - a - 2);
}
} // namespace gamedb

bool RomCrc32(const std::string& path, uint32_t* whole, uint32_t* body, std::string* inner_ext)
{
	std::string ext = ExtOf(path);
	std::vector<uint8_t> data;
	System sys;
	if (ext == ".zip")
	{
		std::string inner;
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
		const size_t n = fread(data.data(), 1, data.size(), f);
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
	const std::string roms = OrbisDir("roms"), covers = OrbisDir("covers");
	for (const SystemInfo& s : kSystems)
	{
		OrbisMkdirs(roms + "/" + s.folder);
		OrbisMkdirs(covers + "/" + s.folder);
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
		if (rename(m.first.c_str(), m.second.c_str()) != 0)
			OrbisLog("[games] can't move %s to %s", m.first.c_str(), m.second.c_str());
	if (!moves.empty())
		OrbisLog("[games] moved %zu cached cover file(s) into covers/<system>/", moves.size());
}

std::vector<GameInfo> ScanGames()
{
	PrepareFolders();
	std::vector<GameInfo> games;
	for (const std::string& root : OrbisRomRoots())
		Walk(root, 0, root.rfind("/mnt/", 0) == 0, games);

	CrcCache cache;
	cache.Load();
	for (GameInfo& g : games)
	{
		System found = g.system;
		g.nointro = gamedb::Exact(g.system, g.file_base, &found);
		if (g.nointro.empty())
		{
			uint32_t whole = 0, body = 0;
			if (CachedCrc(cache, g.path, &whole, &body))
			{
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
		g.title = g.nointro.empty() ? g.file_base : gamedb::Title(g.nointro);
		g.region = gamedb::Region(g.nointro);
	}
	cache.Save();

	std::sort(games.begin(), games.end(), [](const GameInfo& a, const GameInfo& b) {
		const int c = strcasecmp(a.title.c_str(), b.title.c_str());
		if (c != 0)
			return c < 0;
		if (a.system != b.system)
			return a.system < b.system;
		return a.path < b.path;
	});
	OrbisLog("[games] %zu ROM(s); table of %zu names", games.size(), gamedb::Count());
	for (const GameInfo& g : games)
		OrbisLog("[games]   %s [%s] -> \"%s\"%s", g.path.c_str(), Info(g.system).id, g.nointro.c_str(),
			g.name_by_crc ? " (by CRC)" : "");
	return games;
}
} // namespace fe

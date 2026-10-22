// Genesis Plus GX PS5 frontend: the borders downloaded from The Bezel Project (fe_borderfetch.h), ported from Snes9x PS5 2.7.
// SPDX-License-Identifier: MIT

#include "fe_borderfetch.h"

#include "fe_covers.h"

#include "OrbisPaths.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <set>

#ifndef BORDER_INDEX_TXT
#error BORDER_INDEX_TXT must name the built-in index of the borders The Bezel Project has
#endif
#define GENPLUS_BORDERS_INCBIN(sym, path)                                                                        \
	__asm__(".section .rodata\n"                                                                               \
			".balign 16\n"                                                                                     \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                        \
			".incbin \"" path "\"\n"                                                                           \
			".global " #sym "_end\n" #sym "_end:\n"                                                            \
			".previous\n");                                                                                    \
	extern "C" const char sym##_begin[];                                                                       \
	extern "C" const char sym##_end[];

GENPLUS_BORDERS_INCBIN(genplus_border_index, BORDER_INDEX_TXT)

namespace fe::border
{
namespace
{
constexpr long kMissingRetrySeconds = 30L * 24 * 3600; // as the covers: a 404 isn't asked again for 30 days
constexpr size_t kSystems = size_t(System::Count);

// "Sonic & Knuckles (World)" -> "Sonic%20%26%20Knuckles%20%28World%29": everything but letters, digits and -_.~
std::string Encode(const std::string& s)
{
	static const char* const hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s)
	{
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c))
			out += char(c);
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

bool NonEmptyFile(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

// <name>.missing holds the address that gave a 404 (fe_coverfetch.cpp: SaveCoverResult).
bool RecentlyMissing(const std::string& marker, const std::string& url)
{
	struct stat st = {};
	if (stat(marker.c_str(), &st) != 0 || time(nullptr) - st.st_mtime >= kMissingRetrySeconds)
		return false;
	FILE* f = fopen(marker.c_str(), "r");
	if (!f)
		return false;
	char line[8192] = {};
	const bool read = fgets(line, sizeof(line), f) != nullptr;
	fclose(f);
	if (!read)
		return false;
	std::string old(line);
	while (!old.empty() && (old.back() == '\n' || old.back() == '\r'))
		old.pop_back();
	return old == url;
}

// The index text: "#@ <system> <repository>/<folder>" before each system's names; other "#" lines are comments.
struct Index
{
	std::vector<std::string> names[kSystems];
	std::string base[kSystems]; // https://raw.githubusercontent.com/thebezelproject/<repository>/master/<folder>
};

Index ParseIndex(const char* p, const char* end)
{
	Index index;
	int cur = -1;
	while (p < end)
	{
		const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
		if (!nl)
			break;
		const std::string line(p, size_t(nl - p));
		p = nl + 1;
		if (line.compare(0, 3, "#@ ") == 0)
		{
			const size_t sp = line.find(' ', 3);
			const std::string id = line.substr(3, sp == std::string::npos ? std::string::npos : sp - 3);
			cur = -1;
			for (size_t i = 0; i < kSystems; i++)
				if (id == Info(System(i)).id)
					cur = int(i);
			if (cur >= 0 && sp != std::string::npos)
			{
				const std::string where = line.substr(sp + 1); // "bezelproject-MegaDrive/retroarch/overlay/GameBezels/MegaDrive/"
				const size_t slash = where.find('/');
				if (slash != std::string::npos)
					index.base[cur] = "https://raw.githubusercontent.com/thebezelproject/" + where.substr(0, slash) + "/master" +
									  where.substr(slash);
			}
			continue;
		}
		if (line.empty() || line[0] == '#' || cur < 0)
			continue;
		index.names[cur].push_back(line);
	}
	return index;
}

const Index& TheIndex()
{
	static const Index index = ParseIndex(genplus_border_index_begin, genplus_border_index_end);
	return index;
}

size_t Slot(System s)
{
	return size_t(s) < kSystems ? size_t(s) : 0;
}

// One matcher per system, made the first time a game of that system asks (the index never changes); a toss-up takes the
// USA border.
const PackMatcher& Matcher(System s)
{
	static std::mutex lock;
	static std::unique_ptr<PackMatcher> made[kSystems];
	std::lock_guard<std::mutex> hold(lock);
	std::unique_ptr<PackMatcher>& m = made[Slot(s)];
	if (!m)
		m.reset(new PackMatcher(IndexNames(s), true));
	return *m;
}

bool BezelNeeded(const std::string& pack, System s, const std::string& name, WantedCover* out)
{
	const std::string file = std::string(Info(s).folder) + "/" + name + ".png";
	const std::string path = pack + "/" + file;
	const std::string url = BezelUrl(s, name);
	if (NonEmptyFile(path) || RecentlyMissing(path.substr(0, path.size() - 4) + ".missing", url))
		return false;
	out->file = file;
	out->url = url;
	return true;
}

void WriteList(const std::string& path, const std::vector<WantedCover>& list)
{
	std::string text;
	for (const WantedCover& w : list)
		text += w.file + "\t" + w.url + "\n";
	OrbisMkdirs(path.substr(0, path.find_last_of('/')));
	WriteFileAtomicTo(path, std::vector<uint8_t>(text.begin(), text.end()));
}
} // namespace

const std::vector<std::string>& IndexNames(System s)
{
	return TheIndex().names[Slot(s)];
}

std::string BezelNameFor(const GameInfo& game)
{
	if (IndexNames(game.system).empty())
		return "";
	return Matcher(game.system).Match(game);
}

std::string BezelUrl(System s, const std::string& name)
{
	const char* base = getenv("GENPLUS_BORDER_URL");
	return std::string(base && *base ? base : TheIndex().base[Slot(s)]) + Encode(name) + ".png";
}

bool BorderWanted(const std::string& pack, const GameInfo& game, WantedCover* out)
{
	const std::string name = BezelNameFor(game);
	return !name.empty() && BezelNeeded(pack, game.system, name, out);
}

std::vector<WantedCover> MissingBorders(const std::string& pack, const std::vector<GameInfo>& games)
{
	std::vector<WantedCover> out;
	std::set<std::string> seen;
	for (const GameInfo& g : games)
	{
		const std::string folder = Info(g.system).folder;
		if (NonEmptyFile(pack + "/" + folder + "/" + g.file_base + ".png"))
			continue; // the player's own border for this ROM
		const std::string name = BezelNameFor(g);
		WantedCover w;
		if (!name.empty() && seen.insert(folder + "/" + name).second && BezelNeeded(pack, g.system, name, &w))
			out.push_back(w);
	}
	return out;
}

void WriteWantedBorders(const std::string& pack, const std::vector<WantedCover>& wanted)
{
	WriteList(pack + "/wanted.txt", wanted);
}

void WritePriorityBorders(const std::string& pack, const std::vector<WantedCover>& wanted)
{
	WriteList(pack + "/priority.txt", wanted);
}
} // namespace fe::border

// Genesis Plus GX PS5 frontend: the running game's cheats (fe_cheats.h). Ported from Snes9x PS5's fe_emu.cpp
// (thatboialex's cheat manager, pull request #1; the reload when the file changes, 2.7), for Genesis Plus GX's
// libretro cheat interface.
// SPDX-License-Identifier: MIT

#include "fe_cheats.h"

#include "fe_cheatcode.h"
#include "fe_chtfile.h"
#include "fe_emu.h"
#include "fe_i18n.h"

#include "OrbisPaths.h"

#include "libretro.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

namespace fe
{
namespace cheats
{
namespace
{
// libretro.c keeps 150 codes (MAX_CHEATS); one more would be written past its table.
constexpr int kMaxCodes = 150;

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string Basename(const std::string& p)
{
	const size_t slash = p.find_last_of('/');
	return slash == std::string::npos ? p : p.substr(slash + 1);
}

// A file as it was when looked at: path, size and time, to tell whether someone has changed it since. size -1: not there.
struct FileStamp
{
	std::string path;
	long long size = -1;
	long long mtime = 0;
	long long mtime_ns = 0;
	bool operator==(const FileStamp& o) const
	{
		return path == o.path && size == o.size && mtime == o.mtime && mtime_ns == o.mtime_ns;
	}
	bool operator!=(const FileStamp& o) const { return !(*this == o); }
};

FileStamp StampOf(const std::string& path)
{
	FileStamp s;
	s.path = path;
	struct stat st = {};
	if (!path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
	{
		s.size = st.st_size;
		s.mtime = st.st_mtime;
		s.mtime_ns = st.st_mtim.tv_nsec; // two copies within a second, of the same size, are still told apart
	}
	return s;
}

struct Cheat
{
	cht::Entry entry;
	bool usable = false;
	std::vector<cheatcode::Code> codes;
};

struct Session
{
	bool loaded = false;
	std::string path, name;
	System system = System::Md;
	std::string official; // the No-Intro / Redump name, "" if unknown
	bool official_done = false;
	System official_system = System::Md;
	std::vector<Cheat> list; // the file's cheats, usable or not, in its order
	std::vector<int> shown; // the usable ones: what the cheat manager lists
	bool dirty = false; // a switch not saved yet
	// the status line: in English for the log, shown translated from its format
	const char* status_fmt = "No cheat file checked";
	std::string status_file;
	int status_count = 0;
	// The file the cheats came from (or were last saved to) as it was then, and a different state of it seen once:
	// reloaded when the next look finds it unchanged, so a file that is still being copied is left alone.
	FileStamp file, seen;
	double next_frame_look = 0, next_menu_look = 0;
	bool warned_limit = false;
};
Session s;

bool MdCodes()
{
	return s.system == System::Md || s.system == System::SegaCd;
}

bool NetworkPath(const std::string& p)
{
	return p.rfind("/smb/", 0) == 0 || p.rfind("/nfs/", 0) == 0;
}

// libretro-database's folder for a system: "Sega - Mega Drive - Genesis" (the thumbnails repository's name)
std::string DatabaseFolder(System sys)
{
	std::string n = Info(sys).thumbs;
	for (char& c : n)
		if (c == '_')
			c = ' ';
	return n;
}

// what a switch writes: cheats/<system>/<game>.cht
std::string OwnPath()
{
	return OrbisDir("cheats") + "/" + Info(s.system).folder + "/" + s.name + ".cht";
}

void Official()
{
	if (s.official_done)
		return;
	s.official_done = true;
	System found = s.system;
	std::string n = gamedb::Exact(s.system, s.name, &found);
	if (n.empty() && s.system != System::SegaCd && !NetworkPath(s.path))
	{
		uint32_t whole = 0, body = 0;
		if (RomCrc32(s.path, &whole, &body))
		{
			n = gamedb::ByCrc(s.system, whole, &found);
			if (n.empty() && body != whole)
				n = gamedb::ByCrc(s.system, body, &found);
		}
	}
	if (n.empty())
		n = gamedb::Loose(s.system, s.name, &found);
	s.official = n;
	s.official_system = n.empty() ? s.system : found;
}

// Users copy "Sonic The Hedgehog (USA, Europe).cht" for "Sonic.md", or libretro-database's folders as they are.
// Only a .cht close to the game's names is taken: never another game's.
std::string FindFile()
{
	const std::string own = OwnPath();
	if (OrbisIsFile(own))
		return own;
	Official();
	const std::string root = OrbisDir("cheats");
	std::vector<std::string> dirs = {root + "/" + Info(s.system).folder, root, root + "/" + DatabaseFolder(s.system)};
	if (s.official_system != s.system)
	{
		dirs.push_back(root + "/" + Info(s.official_system).folder);
		dirs.push_back(root + "/" + DatabaseFolder(s.official_system));
	}
	const size_t slash = s.path.find_last_of('/');
	if (slash != std::string::npos && !NetworkPath(s.path)) // cheats stay on the console: no listing of a share
		dirs.push_back(s.path.substr(0, slash));
	std::vector<std::string> files;
	for (const std::string& dir : dirs)
	{
		DIR* d = opendir(dir.c_str());
		if (!d)
			continue;
		while (dirent* e = readdir(d))
		{
			const std::string file = e->d_name;
			if (!cht::EndsWithCht(file))
				continue;
			const std::string full = dir + "/" + file;
			struct stat st = {};
			if (lstat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
				files.push_back(full);
		}
		closedir(d);
	}
	return cht::Best(files, s.name, s.official);
}

// The cheats that are on, to the core: libretro.c takes them all back (retro_cheat_reset gives the ROM its bytes
// back) and gets the ones that are on, one code at a time, as RetroArch gives them.
void Apply()
{
	retro_cheat_reset();
	int codes = 0, cheats = 0;
	bool over = false;
	for (size_t i = 0; i < s.list.size(); i++)
	{
		const Cheat& c = s.list[i];
		if (!c.usable || !c.entry.enable)
			continue;
		if (codes + int(c.codes.size()) > kMaxCodes)
		{
			over = true;
			continue;
		}
		for (const cheatcode::Code& code : c.codes)
			retro_cheat_set(unsigned(i), true, code.text.c_str());
		codes += int(c.codes.size());
		cheats++;
	}
	OrbisLog("[cheats] %d on (%d codes)%s", cheats, codes, over ? ", more left out: the core keeps 150 codes" : "");
	if (over && !s.warned_limit)
	{
		s.warned_limit = true;
		emu::Osd("Too many cheats on: only the first 150 codes work");
	}
}

// Reads a .cht into the list; false (the list left as it was) when it has no cheat the core can use.
bool Import(const std::string& path)
{
	s.file = StampOf(path); // before the read: a change made while it runs is seen by the next look
	s.seen = FileStamp();
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		s.status_fmt = "Unable to open cheat file";
		OrbisLog("[cheats] unable to open %s", path.c_str());
		return false;
	}
	std::stringstream ss;
	ss << in.rdbuf();
	std::vector<cht::Entry> entries;
	const bool parsed = cht::Parse(ss.str(), &entries);
	std::vector<Cheat> list;
	std::vector<int> shown;
	for (const cht::Entry& e : entries)
	{
		Cheat c;
		c.entry = e;
		c.usable = cheatcode::DecodeAll(e.code, MdCodes(), s.system == System::SegaCd, &c.codes);
		if (c.usable)
			shown.push_back(int(list.size()));
		list.push_back(std::move(c));
	}
	s.status_file = Basename(path);
	if (!parsed || shown.empty())
	{
		s.status_fmt = "Found %s but no supported codes";
		OrbisLog("[cheats] Found %s but no supported codes", s.status_file.c_str());
		return false;
	}
	s.list = std::move(list);
	s.shown = std::move(shown);
	s.dirty = false;
	s.status_fmt = "Loaded %d cheats from %s";
	s.status_count = int(s.shown.size());
	OrbisLog("[cheats] Loaded %d cheats from %s (%zu in the file)", s.status_count, path.c_str(), s.list.size());
	return true;
}

bool LoadForGame()
{
	const std::string file = FindFile();
	if (file.empty())
	{
		s.status_fmt = "No matching .cht file in /data/genplus/cheats";
		OrbisLog("[cheats] no .cht for %s%s%s", s.name.c_str(), s.official.empty() ? "" : " / ", s.official.c_str());
		return false;
	}
	return Import(file);
}

bool Save()
{
	if (!s.loaded || s.list.empty())
		return true;
	const std::string path = OwnPath();
	OrbisMkdirs(path.substr(0, path.find_last_of('/')));
	std::vector<cht::Entry> entries;
	for (const Cheat& c : s.list)
		entries.push_back(c.entry);
	const std::string text = cht::Write(entries);
	const std::string tmp = path + ".part";
	FILE* f = fopen(tmp.c_str(), "wb");
	bool ok = f != nullptr;
	if (f)
	{
		ok = fwrite(text.data(), 1, text.size(), f) == text.size();
		ok = fflush(f) == 0 && ok;
		ok = fsync(fileno(f)) == 0 && ok;
		ok = fclose(f) == 0 && ok;
	}
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
	{
		remove(tmp.c_str());
		OrbisLog("[cheats] save failed: %s", path.c_str());
		s.dirty = true;
		return false;
	}
	s.file = StampOf(path); // our own write is not a change
	s.seen = FileStamp();
	s.dirty = false;
	OrbisLog("[cheats] saved %zu cheats to %s", s.list.size(), path.c_str());
	return true;
}

// A cheat file changed on disk (a code added over FTP) replaces the cheats in the game, no restart needed. The disk
// is the truth: every switch is saved at once, so only a switch that could not be written is lost. True: the
// cheats were replaced. immediate: reload at the first look that sees a change (the cheat manager was just opened,
// a switch is about to be saved); otherwise the file must look the same on two looks. search: look through the
// folders for a file when the game's own does not exist (directory listings: for the menu only).
bool Sync(bool immediate, bool search = false)
{
	if (!s.loaded)
		return false;
	std::string src = OwnPath(); // what a switch writes, so it wins once it exists
	if (!OrbisIsFile(src))
		src = search ? FindFile() : s.file.path;
	const FileStamp now = StampOf(src);
	if (now == s.file)
	{
		s.seen = FileStamp();
		return false;
	}
	if (now.size <= 0)
		return false; // gone or empty: the cheats that run stay
	if (!immediate && now != s.seen)
	{
		s.seen = now;
		return false;
	}
	s.seen = FileStamp();
	if (!Import(src))
	{
		OrbisLog("[cheats] %s has no usable cheat: the %zu that run stay", Basename(src).c_str(), s.shown.size());
		return false;
	}
	Apply();
	OrbisLog("[cheats] reloaded: %zu cheats from %s", s.shown.size(), Basename(src).c_str());
	return true;
}
} // namespace

void GameStarted(const std::string& path, const std::string& name, System system)
{
	s = Session();
	s.loaded = true;
	s.path = path;
	s.name = name;
	s.system = system;
	s.next_frame_look = Now() + 1.0;
	if (LoadForGame())
		Apply();
	else
		retro_cheat_reset();
}

void GameClosing()
{
	if (!s.loaded)
		return;
	if (s.dirty)
		Save();
	retro_cheat_reset(); // the ROM gets its bytes back; no RAM value is written into the next game
	s = Session();
}

void Frame()
{
	if (!s.loaded)
		return;
	// A cheat added to the file while the game runs is picked up within a few seconds: one stat a second, no search.
	const double t = Now();
	if (t < s.next_frame_look)
		return;
	s.next_frame_look = t + 1.0;
	if (Sync(false))
	{
		char buf[128];
		snprintf(buf, sizeof(buf), T("Cheats updated: %d"), Count());
		emu::Osd(buf);
	}
}

int Count()
{
	return s.loaded ? int(s.shown.size()) : 0;
}

std::string Name(int index)
{
	return index >= 0 && index < Count() ? s.list[size_t(s.shown[size_t(index)])].entry.desc : "";
}

bool Enabled(int index)
{
	return index >= 0 && index < Count() && s.list[size_t(s.shown[size_t(index)])].entry.enable;
}

bool Toggle(int index)
{
	if (Sync(true))
		return true; // the file was changed: the list is new, so nothing is switched
	if (index < 0 || index >= Count())
		return false;
	Cheat& c = s.list[size_t(s.shown[size_t(index)])];
	c.entry.enable = !c.entry.enable;
	s.warned_limit = false;
	Apply();
	OrbisLog("[cheats] %s: %s", c.entry.desc.c_str(), c.entry.enable ? "on" : "off");
	return Save();
}

void SetAll(bool enabled)
{
	if (!s.loaded)
		return;
	Sync(true); // all of the file's cheats, not of the ones it replaced
	if (s.shown.empty())
		return;
	for (int i : s.shown)
		s.list[size_t(i)].entry.enable = enabled;
	s.warned_limit = false;
	Apply();
	Save();
}

bool Refresh(bool now)
{
	// The cheat manager is redrawn at 60 Hz; looking at the files once a second is enough (listing a 2,000-file
	// folder every frame would stall the pad). The first look comes the moment the menu opens.
	const double t = Now();
	if (!s.loaded || (!now && t < s.next_menu_look))
		return false;
	s.next_menu_look = t + 1.0;
	// Without cheats the folders are searched too: a file named after the game's official name may have been added.
	return Sync(now, s.shown.empty());
}

std::string Status()
{
	char buf[512];
	const char* fmt = T(s.status_fmt);
	if (strcmp(s.status_fmt, "Loaded %d cheats from %s") == 0)
		snprintf(buf, sizeof(buf), fmt, s.status_count, s.status_file.c_str());
	else if (strcmp(s.status_fmt, "Found %s but no supported codes") == 0)
		snprintf(buf, sizeof(buf), fmt, s.status_file.c_str());
	else
		return fmt;
	return buf;
}
} // namespace cheats
} // namespace fe

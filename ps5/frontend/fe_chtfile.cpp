// Genesis Plus GX PS5 frontend: libretro cheat files (fe_chtfile.h).
// SPDX-License-Identifier: MIT

#include "fe_chtfile.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <map>

namespace fe
{
namespace cht
{
namespace
{
std::string Lower(std::string s)
{
	for (char& c : s)
		c = char(tolower(uint8_t(c)));
	return s;
}

std::string Trim(const std::string& s)
{
	const size_t first = s.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return "";
	const size_t last = s.find_last_not_of(" \t\r\n");
	return s.substr(first, last - first + 1);
}

// a value: trimmed, its quotes taken off
std::string Value(std::string s)
{
	s = Trim(s);
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
		s = s.substr(1, s.size() - 2);
	return s;
}

// a value as written back: one line, no quote inside the quotes
std::string Clean(std::string s)
{
	for (char& c : s)
		if (c == '\r' || c == '\n')
			c = ' ';
		else if (c == '"')
			c = '\'';
	return s;
}

std::string Stem(const std::string& path)
{
	const size_t slash = path.find_last_of("/\\");
	const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
	return EndsWithCht(base) ? base.substr(0, base.size() - 4) : base;
}
} // namespace

bool Parse(const std::string& text, std::vector<Entry>* out)
{
	std::map<int, Entry> cheats;
	bool recognized = false;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t end = text.find('\n', pos);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = text.substr(pos, end - pos);
		pos = end + 1;
		if (line.size() > 32768)
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = Trim(line.substr(0, eq));
		if (key.size() < 8 || key.compare(0, 5, "cheat") != 0)
			continue;
		char* tail = nullptr;
		const long idx = strtol(key.c_str() + 5, &tail, 10);
		if (tail == key.c_str() + 5 || idx < 0 || idx >= 4096 || *tail != '_')
			continue;
		const std::string field = tail + 1;
		const std::string val = Value(line.substr(eq + 1));
		Entry& c = cheats[int(idx)];
		if (field == "code")
		{
			c.code = val;
			recognized = true;
		}
		else if (field == "desc")
			c.desc = val;
		else if (field == "enable")
			c.enable = val == "true" || val == "1";
	}
	if (out)
	{
		out->clear();
		for (const auto& it : cheats)
		{
			Entry e = it.second;
			if (e.desc.empty())
				e.desc = "Cheat " + std::to_string(it.first + 1);
			out->push_back(e);
		}
	}
	return recognized;
}

std::string Write(const std::vector<Entry>& cheats)
{
	std::string s = "cheats = " + std::to_string(cheats.size()) + "\n";
	for (size_t i = 0; i < cheats.size(); i++)
	{
		const std::string n = "cheat" + std::to_string(i);
		s += "\n" + n + "_desc = \"" + Clean(cheats[i].desc) + "\"\n";
		s += n + "_code = \"" + Clean(cheats[i].code) + "\"\n";
		s += n + "_enable = " + (cheats[i].enable ? "true" : "false") + "\n";
	}
	return s;
}

bool EndsWithCht(const std::string& name)
{
	return name.size() > 4 && Lower(name.substr(name.size() - 4)) == ".cht";
}

std::string Key(const std::string& name)
{
	std::string stem = name;
	const size_t dot = stem.find_last_of('.');
	if (dot != std::string::npos && stem.size() - dot <= 5 && stem.find_first_of(" )]", dot) == std::string::npos)
		stem.resize(dot); // ".cht", ".md", ".zip"...: an extension, not "Vol. 2"
	std::string out;
	int depth = 0;
	for (unsigned char c : stem)
	{
		if (c == '(' || c == '[')
		{
			++depth;
			continue;
		}
		if (c == ')' || c == ']')
		{
			if (depth)
				--depth;
			continue;
		}
		if (depth == 0 && isalnum(c))
			out += char(tolower(c));
	}
	return out;
}

std::string Region(const std::string& name)
{
	for (const char* tag : {"USA", "Europe", "Japan", "World", "Brazil", "Korea"})
		if (name.find(std::string("(") + tag) != std::string::npos || name.find(std::string(", ") + tag) != std::string::npos)
			return tag;
	return "";
}

int Distance(const std::string& a, const std::string& b, int limit)
{
	if (a == b)
		return 0;
	if (a.empty() || b.empty() || a.size() > b.size() + size_t(limit) || b.size() > a.size() + size_t(limit))
		return limit + 1;
	std::vector<int> prev(b.size() + 1), curr(b.size() + 1);
	for (size_t j = 0; j <= b.size(); ++j)
		prev[j] = int(j);
	for (size_t i = 1; i <= a.size(); ++i)
	{
		curr[0] = int(i);
		int min_row = curr[0];
		for (size_t j = 1; j <= b.size(); ++j)
		{
			curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
			min_row = std::min(min_row, curr[j]);
		}
		if (min_row > limit)
			return limit + 1;
		prev.swap(curr);
	}
	return prev[b.size()];
}

std::string Best(const std::vector<std::string>& files, const std::string& rom, const std::string& official)
{
	const std::string expected = Key(!official.empty() ? official : rom);
	const std::string alt = Key(rom);
	if (expected.size() < 4 && alt.size() < 4)
		return "";
	const std::string wanted_region = Region(official.empty() ? rom : official);
	std::string best, best_key;
	int best_score = INT_MIN;
	bool ambiguous = false;
	for (const std::string& file : files)
	{
		if (!EndsWithCht(file))
			continue;
		const std::string candidate = Stem(file);
		const std::string key = Key(candidate);
		if (key.size() < 4)
			continue;
		const std::string& compare = expected.empty() ? alt : expected;
		const int limit = compare.size() < 8 ? 0 : compare.size() < 15 ? 1 : compare.size() < 32 ? 2 : 3;
		int distance = Distance(compare, key, limit);
		if (alt != compare && !alt.empty())
			distance = std::min(distance, Distance(alt, key, limit));
		if (distance > limit)
			continue;
		const std::string lower = Lower(candidate);
		const std::string region = Region(candidate);
		int score = 1000 - distance * 100;
		if (lower == Lower(rom))
			score += 500;
		if (!official.empty() && lower == Lower(official))
			score += 450;
		if (!wanted_region.empty() && region == wanted_region)
			score += 60;
		else if (!wanted_region.empty() && !region.empty())
			score -= 60;
		else if (wanted_region.empty() && region == "USA")
			score += 15;
		if (lower.find("(game genie)") != std::string::npos)
			score -= 10;
		if (lower.find("(action replay)") != std::string::npos)
			score -= 10;
		if (lower.find("(rev ") != std::string::npos)
			score -= 2;
		if (score > best_score)
		{
			best_score = score;
			best = file;
			best_key = key;
			ambiguous = false;
		}
		else if (score == best_score && key != best_key)
			ambiguous = true;
	}
	return ambiguous ? "" : best;
}
} // namespace cht
} // namespace fe

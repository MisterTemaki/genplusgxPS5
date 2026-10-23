// Genesis Plus GX PS5 host test of the 7z reader (frontend/fe_7z.cpp with third_party/lzma7z), on the archives of
// tests/7z/ (tests/make_7z_fixtures.py): each method and filter, a solid archive, an empty one, broken ones, encrypted
// ones, a method the decoder lacks, a file and a solid block over the limits, a decoder over its memory budget; then
// every cut-short length and a changed byte at many places of an archive (under ASan: nothing read out of bounds).
//
//   7z-test <tests/7z>
//
// SPDX-License-Identifier: MIT

#include "fe_7z.h"

#include <zlib.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using fe::sevenzip::Archive;
using fe::sevenzip::Status;

namespace
{
int g_pass = 0, g_fail = 0;

void Check(bool ok, const std::string& what)
{
	printf("  %s: %s\n", ok ? "ok" : "FAIL", what.c_str());
	(ok ? g_pass : g_fail)++;
}

std::string Name(Status s)
{
	return fe::sevenzip::StatusText(s);
}

constexpr uint64_t kRom = 16ull << 20, kBlock = 64ull << 20;

uint32_t Crc(const std::vector<uint8_t>& d)
{
	return uint32_t(crc32(0L, d.data(), uInt(d.size())));
}

struct Want
{
	std::string name;
	uint64_t size;
	uint32_t crc;
};

std::vector<uint8_t> ReadAll(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string TempWith(const std::vector<uint8_t>& data)
{
	char path[] = "/tmp/genplus-7z-test-XXXXXX";
	const int fd = mkstemp(path);
	if (fd < 0)
		return "";
	size_t at = 0;
	while (at < data.size())
	{
		const ssize_t n = write(fd, data.data() + at, data.size() - at);
		if (n <= 0)
			break;
		at += size_t(n);
	}
	close(fd);
	return path;
}

// Opens and extracts every file; true if nothing went wrong. Any result is fine for a broken archive, as long as it is
// not a crash, and a file that comes out whole has its own CRC (the decoder checks it).
Status OpenAndExtractAll(const std::string& path, size_t* extracted = nullptr)
{
	Archive a;
	Status s = a.Open(path);
	if (s != Status::Ok)
		return s;
	size_t n = 0;
	for (size_t i = 0; i < a.Entries().size(); i++)
	{
		if (a.Entries()[i].dir)
			continue;
		std::vector<uint8_t> out;
		const Status e = a.Extract(i, &out, kRom, kBlock);
		if (e != Status::Ok)
			return e;
		if (out.size() != a.Entries()[i].size || (a.Entries()[i].has_crc && Crc(out) != a.Entries()[i].crc))
			return Status::Damaged;
		n++;
	}
	if (extracted)
		*extracted = n;
	return Status::Ok;
}
} // namespace

int main(int argc, char** argv)
{
	const std::string dir = argc > 1 ? argv[1] : "tests/7z";
	std::map<std::string, std::vector<Want>> manifest;
	{
		std::ifstream f(dir + "/manifest.txt");
		std::string line;
		while (std::getline(f, line))
		{
			if (line.empty() || line[0] == '#')
				continue;
			std::vector<std::string> col;
			std::stringstream ss(line);
			std::string c;
			while (std::getline(ss, c, '\t'))
				col.push_back(c);
			if (col.size() == 4)
				manifest[col[0]].push_back({col[1], std::stoull(col[2]), uint32_t(std::stoul(col[3], nullptr, 16))});
		}
	}
	Check(manifest.size() >= 10, "manifest.txt lists the good archives (" + std::to_string(manifest.size()) + ")");

	printf("== every good archive: its list (names, sizes, stored CRCs) and each file's bytes\n");
	for (const auto& kv : manifest)
	{
		Archive a;
		const Status s = a.Open(dir + "/" + kv.first);
		Check(s == Status::Ok, kv.first + ": opens (" + Name(s) + ")");
		if (s != Status::Ok)
			continue;
		const std::vector<fe::sevenzip::Entry>& e = a.Entries();
		bool list_ok = e.size() == kv.second.size();
		for (size_t i = 0; list_ok && i < e.size(); i++)
			list_ok = e[i].name == kv.second[i].name && e[i].size == kv.second[i].size && e[i].has_crc && e[i].crc == kv.second[i].crc;
		Check(list_ok, kv.first + ": " + std::to_string(e.size()) + " file(s) listed in order, with their size and CRC");
		for (size_t i = 0; i < e.size() && i < kv.second.size(); i++)
		{
			std::vector<uint8_t> out;
			const Status x = a.Extract(i, &out, kRom, kBlock);
			Check(x == Status::Ok && out.size() == kv.second[i].size && Crc(out) == kv.second[i].crc,
				kv.first + ": " + e[i].name + " comes out whole (" + Name(x) + ")");
		}
	}
	{
		Archive a;
		const Status s = a.Open(dir + "/solid.7z");
		Check(s == Status::Ok && a.Entries().size() == 3 && a.BlockSize(0) == a.BlockSize(2) &&
				  a.BlockSize(1) == 520 + 32768 + 131072,
			"solid.7z: its three files share one block (" + std::to_string(a.BlockSize(1)) + " bytes)");
		Check(a.Entries().size() == 3 && a.Entries()[1].name == "Game B.gg", "solid.7z: Game B.gg comes before Game A.md (archive order)");
	}

	printf("== archives that can't be used\n");
	{
		Archive a;
		const Status s = a.Open(dir + "/empty.7z");
		Check(s == Status::Ok && a.Entries().empty(), "empty.7z: opens, no files (" + Name(s) + ")");
	}
	{
		Archive a;
		const Status s = a.Open(dir + "/truncated.7z");
		Check(s == Status::Damaged, "truncated.7z: damaged (" + Name(s) + ")");
	}
	{
		Archive a;
		std::vector<uint8_t> out;
		const Status s = a.Open(dir + "/corrupt.7z");
		const Status x = s == Status::Ok ? a.Extract(0, &out, kRom, kBlock) : s;
		Check(s == Status::Ok && x == Status::Damaged && out.empty(), "corrupt.7z: the list opens, the file is refused as damaged (" + Name(x) + ")");
	}
	{
		Archive a;
		const Status s = a.Open(dir + "/notarchive.7z");
		Check(s == Status::NotArchive, "notarchive.7z: not a 7z (" + Name(s) + ")");
	}
	{
		Archive a;
		const Status s = a.Open(dir + "/does-not-exist.7z");
		Check(s == Status::CantOpen, "a missing file: can't be opened (" + Name(s) + ")");
	}
	{
		Archive a;
		std::vector<uint8_t> out;
		const Status s = a.Open(dir + "/encrypted.7z");
		const bool listed = s == Status::Ok && a.Entries().size() == 1 && a.Entries()[0].name == "game.md" && a.Entries()[0].has_crc;
		const Status x = s == Status::Ok ? a.Extract(0, &out, kRom, kBlock) : s;
		Check(listed && x == Status::Encrypted, "encrypted.7z: its list is readable (with the stored CRC), its data refused as encrypted (" + Name(x) + ")");
	}
	{
		Archive a;
		std::vector<uint8_t> out;
		const Status s = a.Open(dir + "/encrypted.7z");
		const Status x = s == Status::Ok ? a.Extract(0, &out, 1, 1) : s;
		Check(x == Status::Encrypted, "encrypted.7z with limits it is over: still said to be encrypted (checked first) (" + Name(x) + ")");
	}
	{
		Archive a, b;
		const Status s = a.Open(dir + "/solid.7z", nullptr, 2);
		const Status t = b.Open(dir + "/solid.7z", nullptr, 3);
		Check(s == Status::Damaged && t == Status::Ok, "a list longer than the file limit is refused (3 files: limit 2 refused, limit 3 ok) (" + Name(s) + ")");
	}
	{
		Archive a;
		const Status s = a.Open(dir + "/encrypted-header.7z");
		Check(s == Status::Encrypted, "encrypted-header.7z: refused as encrypted (" + Name(s) + ")");
	}
	{
		Archive a;
		std::vector<uint8_t> out;
		const Status s = a.Open(dir + "/bzip2.7z");
		const Status x = s == Status::Ok ? a.Extract(0, &out, kRom, kBlock) : s;
		Check(x == Status::Unsupported, "bzip2.7z: BZip2 is not supported (" + Name(x) + ")");
	}
	{
		Archive a;
		std::vector<uint8_t> out;
		const Status s = a.Open(dir + "/toobig.7z");
		const Status x = s == Status::Ok ? a.Extract(0, &out, kRom, kBlock) : s;
		Check(x == Status::TooBig && out.empty(), "toobig.7z: a 17 MiB file is over the 16 MiB limit (" + Name(x) + ")");
	}
	{
		Archive a;
		std::vector<uint8_t> out;
		const Status s = a.Open(dir + "/block.7z");
		const bool listed = s == Status::Ok && a.Entries().size() == 2 && a.Entries()[1].name == "game.md";
		const Status x = listed ? a.Extract(1, &out, kRom, kBlock) : s;
		Check(listed && x == Status::BlockTooBig && a.BlockSize(1) > kBlock,
			"block.7z: game.md sits in a 65 MiB solid block, over the 64 MiB limit: refused before decoding (" + Name(x) + ")");
		const Status y = listed ? a.Extract(1, &out, kRom, 80ull << 20) : s;
		Check(y == Status::Ok && out.size() == 131072, "block.7z: with a limit over the block, it comes out (" + Name(y) + ")");
	}
	{
		Archive a;
		std::vector<uint8_t> out;
		const Status s = a.Open(dir + "/ppmd-mem.7z");
		const Status x = s == Status::Ok ? a.Extract(0, &out, kRom, kBlock) : s;
		Check(x == Status::TooBig, "ppmd-mem.7z: a 192 MiB PPMd model is over the decoder's memory budget (" + Name(x) + ")");
	}
	{
		std::atomic<bool> cancel{true};
		Archive a;
		const Status s = a.Open(dir + "/lzma2.7z", &cancel);
		Check(s == Status::Cancelled, "a cancel flag already set: the read stops (" + Name(s) + ")");
	}

	printf("== broken copies of solid.7z: every length cut short, a changed byte at many places (nothing may crash)\n");
	{
		const std::vector<uint8_t> whole = ReadAll(dir + "/solid.7z");
		size_t opened = 0, good = 0, total = 0;
		bool wrong_ok = false;
		for (size_t len = 0; len < whole.size(); len++)
		{
			const std::string p = TempWith(std::vector<uint8_t>(whole.begin(), whole.begin() + long(len)));
			const Status s = OpenAndExtractAll(p);
			unlink(p.c_str());
			total++;
			wrong_ok |= s == Status::Ok;
		}
		Check(!wrong_ok && total == whole.size(), "each of the " + std::to_string(total) + " shorter copies is refused");
		for (size_t at = 0; at < whole.size(); at++)
		{
			std::vector<uint8_t> bad = whole;
			bad[at] ^= uint8_t(0x5A + at);
			const std::string p = TempWith(bad);
			size_t n = 0;
			const Status s = OpenAndExtractAll(p, &n);
			unlink(p.c_str());
			if (s != Status::NotArchive && s != Status::Damaged)
				opened++;
			if (s == Status::Ok)
				good++;
		}
		// a changed byte the archive doesn't check (an attribute, a time) may leave it whole; the files then come out with
		// their CRCs right (OpenAndExtractAll checks them)
		Check(true, "each of the " + std::to_string(whole.size()) + " copies with a changed byte: no crash (" + std::to_string(good) +
						" still whole, " + std::to_string(opened - good) + " refused otherwise, the rest damaged)");
	}

	printf("passed %d, failed %d\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

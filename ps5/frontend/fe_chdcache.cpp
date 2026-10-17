// Genesis Plus GX PS5 frontend: CHD hunks decompressed ahead, on a thread of their own.
//
// The core reads a .chd one hunk (8 sectors: about 0.1 s of CD audio) at a time, through libchdr's chd_read, on the
// emulation thread (cd_hw/cdd.c). A compressed hunk (FLAC, LZMA, zstd) takes a few milliseconds to decompress: on
// the PS5 the core's frame time jumped from 1.5 ms to 10-12 ms each time CD audio crossed a hunk. Here chd_read and
// chd_close are wrapped at link time (--wrap, the core unchanged): a worker thread decompresses the next hunks while
// the game plays the current one, and chd_read hands them over with a memcpy. Every call into libchdr goes through
// one lock (a chd_file is not thread-safe); a hunk not ready yet is read as before.
//
// SPDX-License-Identifier: MIT

#include "OrbisPaths.h"
#include "ProsperoThread.h"

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include "libchdr/chd.h"

extern "C"
{
	// the originals, under the names --wrap gives them
	chd_error __real_chd_read(chd_file* chd, uint32_t hunknum, void* buffer);
	void __real_chd_close(chd_file* chd);
}

namespace
{
constexpr int kAhead = 3; // hunks decompressed ahead of the one being played (~0.3 s of CD audio)

struct Cache
{
	std::mutex m; // the cache's state
	std::mutex lib; // every call into libchdr
	std::condition_variable cv;
	chd_file* chd = nullptr;
	uint32_t hunkbytes = 0, total = 0;
	std::map<uint32_t, std::vector<uint8_t>> ready;
	std::deque<uint32_t> wanted;
	int64_t decoding = -1;
	bool stop = false;
	ps5::BigThread worker;
	bool started = false;
	uint64_t hits = 0, misses = 0; // since the disc was opened (logged at chd_close)
	uint64_t hits_total = 0, misses_total = 0; // since the last fe_chdcache_take
};
Cache* g_cache = nullptr;

Cache& C()
{
	static Cache* c = new Cache; // lives as long as the app (a static destructor would race the worker at exit)
	g_cache = c;
	return *c;
}

void Worker()
{
	Cache& c = C();
	std::unique_lock<std::mutex> lk(c.m);
	for (;;)
	{
		c.cv.wait(lk, [&] { return c.stop || !c.wanted.empty(); });
		if (c.stop)
			return;
		const uint32_t hunk = c.wanted.front();
		c.wanted.pop_front();
		if (c.ready.count(hunk) || !c.chd || hunk >= c.total)
			continue;
		chd_file* chd = c.chd;
		std::vector<uint8_t> buf(c.hunkbytes);
		c.decoding = hunk;
		lk.unlock();
		chd_error err;
		{
			std::lock_guard<std::mutex> l(c.lib);
			err = __real_chd_read(chd, hunk, buf.data());
		}
		lk.lock();
		c.decoding = -1;
		if (err == CHDERR_NONE && c.chd == chd)
			c.ready[hunk] = std::move(buf);
		c.cv.notify_all();
	}
}

// the hunks after `hunk`, for the worker
void Want(Cache& c, uint32_t hunk)
{
	for (int i = 1; i <= kAhead; i++)
	{
		const uint32_t h = hunk + uint32_t(i);
		if (h < c.total && !c.ready.count(h) && int64_t(h) != c.decoding)
		{
			bool queued = false;
			for (uint32_t q : c.wanted)
				queued |= q == h;
			if (!queued)
				c.wanted.push_back(h);
		}
	}
	// only what lies ahead is kept
	for (auto it = c.ready.begin(); it != c.ready.end();)
		it = (it->first < hunk || it->first > hunk + uint32_t(kAhead)) ? c.ready.erase(it) : std::next(it);
	c.cv.notify_all();
}
} // namespace

extern "C" chd_error __wrap_chd_read(chd_file* chd, uint32_t hunknum, void* buffer)
{
	Cache& c = C();
	std::unique_lock<std::mutex> lk(c.m);
	if (c.chd != chd)
	{
		// another disc: forget the last one's hunks
		c.chd = chd;
		c.ready.clear();
		c.wanted.clear();
		const chd_header* h = chd_get_header(chd);
		c.hunkbytes = h ? h->hunkbytes : 0;
		c.total = h ? h->totalhunks : 0;
		if (!c.started)
		{
			c.started = true;
			c.worker = ps5::BigThread(Worker, 1 << 20);
		}
	}
	// being decompressed right now: wait for it
	c.cv.wait(lk, [&] { return int64_t(hunknum) != c.decoding; });
	auto it = c.ready.find(hunknum);
	if (it != c.ready.end() && it->second.size() == c.hunkbytes)
	{
		memcpy(buffer, it->second.data(), c.hunkbytes);
		c.hits++;
		c.hits_total++;
		Want(c, hunknum);
		return CHDERR_NONE;
	}
	c.misses++;
	c.misses_total++;
	lk.unlock();
	chd_error err;
	{
		std::lock_guard<std::mutex> l(c.lib);
		err = __real_chd_read(chd, hunknum, buffer);
	}
	lk.lock();
	Want(c, hunknum);
	return err;
}

extern "C" void __wrap_chd_close(chd_file* chd)
{
	Cache& c = C();
	{
		std::unique_lock<std::mutex> lk(c.m);
		if (c.chd == chd)
		{
			c.wanted.clear();
			c.cv.wait(lk, [&] { return c.decoding < 0; }); // the worker is done with this file
			c.ready.clear();
			c.chd = nullptr;
			if (c.hits + c.misses)
				OrbisLog("[chd] %llu hunk(s) read: %llu decompressed ahead, %llu on the spot",
					(unsigned long long)(c.hits + c.misses), (unsigned long long)c.hits, (unsigned long long)c.misses);
			c.hits = c.misses = 0;
		}
	}
	std::lock_guard<std::mutex> l(c.lib);
	__real_chd_close(chd);
}

// For the performance log: the hunks read since the last call, and how many were already decompressed.
extern "C" void fe_chdcache_take(unsigned long long* hits, unsigned long long* misses)
{
	Cache& c = C();
	std::lock_guard<std::mutex> lk(c.m);
	*hits = c.hits_total;
	*misses = c.misses_total;
	c.hits_total = c.misses_total = 0;
}

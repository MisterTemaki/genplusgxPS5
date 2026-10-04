// Genesis Plus GX PS5: the picture, through libSceVideoOut.
//
// The scan-out setup and the tile swizzle are the ones of the ps5-payload-dev SDK's SDL2 port
// (src/video/ps5/SDL_ps5video.c and SDL_ps5tilemap.c, (C) 2026 John Tornblom, zlib licence), which a
// payload ELF can use without any GPU driver:
//   - sceVideoOutOpen(255, 0, 0, NULL) on the system user, a 64 MiB direct-memory block split in two
//     scan-out buffers, sceVideoOutSetBufferAttribute2(A8B8G8R8, 1920x1080), RegisterBuffers2;
//   - a flip event queue to know when a buffer is on screen;
//   - the buffers are tiled (64 KiB tiles of 512x128 pixels, with an XOR swizzle inside), so every frame
//     the changed rectangle of a linear surface is swizzled in with AVX2 streaming stores.
// Each scan-out buffer remembers what changed since it was last written ("pending"), so with two buffers
// only the dirty rectangle has to be copied, never the whole screen.
//
// SPDX-License-Identifier: MIT

#include "ProsperoVideo.h"

#include "OrbisPaths.h"
#include "ProsperoSce.h"

#include <immintrin.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>

namespace ps5video
{
namespace
{
constexpr uint32_t kTileW = 512;
constexpr uint32_t kTileH = 128;
constexpr uint32_t kTileSize = kTileW * kTileH;
constexpr uint32_t kBand = 8;
constexpr size_t k2M = 0x200000;

struct R
{
	int x = 0, y = 0, w = 0, h = 0;
	bool Empty() const { return w <= 0 || h <= 0; }
};

void Union(R& acc, const R& r)
{
	if (r.Empty())
		return;
	if (acc.Empty())
	{
		acc = r;
		return;
	}
	const int x0 = std::min(acc.x, r.x), y0 = std::min(acc.y, r.y);
	const int x1 = std::max(acc.x + acc.w, r.x + r.w), y1 = std::max(acc.y + acc.h, r.y + r.h);
	acc = {x0, y0, x1 - x0, y1 - y0};
}

bool Intersect(R& r, const R& clip)
{
	const int x0 = std::max(r.x, clip.x), y0 = std::max(r.y, clip.y);
	const int x1 = std::min(r.x + r.w, clip.x + clip.w), y1 = std::min(r.y + r.h, clip.y + clip.h);
	r = {x0, y0, x1 - x0, y1 - y0};
	return !r.Empty();
}

uint32_t TileOffset(uint32_t x, uint32_t y)
{
	return (((x)&1u) << 0 | ((x >> 1) & 1u) << 1 | ((y >> 0) & 1u) << 2 | ((y >> 1) & 1u) << 3 |
			((y >> 2) & 1u) << 4 | ((x >> 2) & 1u) << 5 | (((x >> 3) ^ (y >> 3)) & 1u) << 6 |
			(((x >> 4) ^ (y >> 4)) & 1u) << 7 | (((x >> 6) ^ (y >> 5)) & 1u) << 8 |
			(((x >> 5) ^ (y >> 6)) & 1u) << 9 | ((y >> 3) & 1u) << 10 | ((x >> 4) & 1u) << 11 |
			((y >> 6) & 1u) << 12 | ((x >> 6) & 1u) << 13 | ((x >> 7) & 1u) << 14 | ((x >> 8) & 1u) << 15);
}

uint32_t TilePixel(uint32_t x, uint32_t y, uint32_t width)
{
	return (x / kTileW) * kTileSize + (y / kTileH) * (kTileH * width) +
		   (TileOffset(x % kTileW, 0) ^ TileOffset(0, y % kTileH));
}

size_t TiledBufferSize(uint32_t width, uint32_t height)
{
	const uint32_t last_band = ((height - 1) / kTileH) * (kTileH * width);
	const uint32_t last_tile = ((width - 1) / kTileW) * kTileSize;
	return size_t(last_band + last_tile + kTileSize) * sizeof(uint32_t);
}

struct State
{
	bool ok = false;
	int handle = -1;
	SceKernelEqueue flip_queue = nullptr;
	intptr_t phys = 0; // direct memory (0 when the buffers are flexible memory)
	void* vmem = nullptr;
	size_t vmem_len = 0;
	int out_w = kWidth, out_h = kHeight; // scan-out size: 1920x1080, or 1280x720 when memory is short
	std::vector<uint32_t> scaled; // the surface scaled to out_w x out_h (720p only)
	std::vector<uint32_t> xmap, ymap; // out pixel -> surface pixel
	SceVideoOutBuffers vbuf[2] = {};
	uint32_t* surface = nullptr;
	std::vector<uint32_t> colx; // tiled offset of every 4th column
	uint32_t yoff[kTileH] = {};
	R pending[2];
	bool full[2] = {true, true};
	uint32_t frame = 0;
	int flips_in_flight = 0;
};
State g;

__attribute__((target("avx2"))) void TileArea(const uint32_t* src, uint32_t pitch, uint32_t* dst, uint32_t x0,
	uint32_t x1, uint32_t y0, uint32_t y1)
{
	const uint32_t* colx = g.colx.data();
	const uint32_t width = g.out_w;
	const uint32_t quads = x1 / 4;

	for (uint32_t y = y0; y < y1; y += kBand)
	{
		const uint32_t base = (y / kTileH) * (kTileH * width);
		const uint32_t yo = g.yoff[y % kTileH];
		const uint32_t* s = src + size_t(y) * pitch;
		uint32_t* d = dst + base;
		const uint32_t rows = std::min(kBand, y1 - y);

		for (uint32_t q = x0 / 4; q < quads; q++)
		{
			const uint32_t* p = s + (q << 2);
			uint32_t* o = d + (colx[q] ^ yo);
			uint32_t k = 0;
			for (; k + 1 < rows; k += 2)
			{
				const __m256i v = _mm256_inserti128_si256(
					_mm256_castsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))),
					_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + pitch)), 1);
				_mm256_stream_si256(reinterpret_cast<__m256i*>(o), v);
				p += 2 * pitch;
				o += 8;
			}
			if (rows & 1)
				_mm_stream_si128(reinterpret_cast<__m128i*>(o), _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)));
		}
		for (uint32_t k = 0; k < rows; k++)
		{
			const uint32_t* p = s + size_t(k) * pitch;
			for (uint32_t x = quads * 4; x < x1; x++)
				dst[TilePixel(x, y + k, width)] = p[x];
		}
	}
	_mm_sfence();
}

void TileBlit(int buf_idx, R damage)
{
	const R screen = {0, 0, g.out_w, g.out_h};
	R area;
	if (g.full[buf_idx])
		area = screen;
	else
	{
		area = g.pending[buf_idx];
		Union(area, damage);
	}
	g.full[buf_idx] = false;
	g.pending[buf_idx] = R();
	Union(g.pending[buf_idx ^ 1], damage); // the other buffer still shows the old picture there

	if (area.Empty())
		return;

	area.w += area.x & 3;
	area.x &= ~3;
	area.w = std::min((area.w + 3) & ~3, g.out_w - area.x);
	area.h += area.y & (kBand - 1);
	area.y &= ~(kBand - 1);
	area.h = std::min(int((area.h + kBand - 1) & ~(kBand - 1)), g.out_h - area.y);

	if (g.out_w != kWidth)
	{
		// 720p scan-out: nearest-neighbour downscale of the (aligned) area first
		for (int y = area.y; y < area.y + area.h; y++)
		{
			const uint32_t* src = g.surface + size_t(g.ymap[y]) * kWidth;
			uint32_t* dst = g.scaled.data() + size_t(y) * g.out_w;
			for (int x = area.x; x < area.x + area.w; x++)
				dst[x] = src[g.xmap[x]];
		}
	}
	const uint32_t* src = g.out_w != kWidth ? g.scaled.data() : g.surface;
	TileArea(src, g.out_w, static_cast<uint32_t*>(g.vbuf[buf_idx].data), area.x, area.x + area.w, area.y,
		area.y + area.h);
}

void WaitOneFlip()
{
	alignas(16) uint8_t ev[128]; // one struct kevent (64 bytes on FreeBSD 12+)
	int out = 0;
	const int r = sceKernelWaitEqueue(g.flip_queue, ev, 1, &out, nullptr);
	if (r != 0)
		OrbisLog("[video] sceKernelWaitEqueue -> %x", r);
	if (g.flips_in_flight > 0)
		g.flips_in_flight--;
}

// ---- scan-out memory --------------------------------------------------------------------------------
// The SDK's SDL port asks for 64 MiB of main direct memory. A payload process can get much less than that
// (the first console logs: sceKernelAllocateMainDirectMemory -> 0x80020023, EAGAIN), so ask only for what
// two buffers need, then try other ways to get it, then 720p.
void FreeScanout()
{
	if (g.phys)
		sceKernelReleaseDirectMemory(g.phys, g.vmem_len); // also unmaps
	else if (g.vmem)
		munmap(g.vmem, g.vmem_len);
	g.phys = 0;
	g.vmem = nullptr;
	g.vmem_len = 0;
}

bool TryRegister(int w, int h)
{
	memset(g.vmem, 0, g.vmem_len);
	g.vbuf[0].data = g.vmem;
	g.vbuf[1].data = static_cast<uint8_t*>(g.vmem) + g.vmem_len / 2;
	SceVideoOutBufferAttribute2 attr;
	memset(&attr, 0, sizeof(attr));
	sceVideoOutSetBufferAttribute2(&attr, SCE_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB, 0, w, h, 0, 0, 0);
	const int r = sceVideoOutRegisterBuffers2(g.handle, 0, 0, g.vbuf, 2, &attr, 0, nullptr);
	OrbisLog("[video]   sceVideoOutRegisterBuffers2(%dx%d) -> %x", w, h, r);
	return r == 0;
}

bool TryDirect(const char* how, int r, size_t len, size_t align, int w, int h)
{
	OrbisLog("[video]   %s(%zu MiB) -> %x phys=%lx", how, len >> 20, r, long(g.phys));
	if (r != 0)
	{
		g.phys = 0;
		return false;
	}
	g.vmem_len = len;
	g.vmem = nullptr;
	r = sceKernelMapDirectMemory(&g.vmem, len, 0x33, 0, g.phys, align);
	OrbisLog("[video]   sceKernelMapDirectMemory -> %x at %p", r, g.vmem);
	if (r != 0)
	{
		sceKernelReleaseDirectMemory(g.phys, len);
		g.phys = 0;
		g.vmem = nullptr;
		return false;
	}
	if (TryRegister(w, h))
		return true;
	FreeScanout();
	return false;
}

bool AllocScanout(int w, int h)
{
	const size_t each = (TiledBufferSize(w, h) + k2M - 1) & ~(k2M - 1);
	const size_t len = each * 2;
	const int64_t dsize = int64_t(sceKernelGetDirectMemorySize());
	int64_t avail_start = 0;
	size_t avail = 0;
	const int ar = sceKernelAvailableDirectMemorySize(0, dsize, 0x10000, &avail_start, &avail);
	size_t flex = 0;
	sceKernelAvailableFlexibleMemorySize(&flex);
	OrbisLog("[video] %dx%d needs %zu MiB; direct memory %lld MiB, largest free %zu MiB (%x), flexible free %zu MiB",
		w, h, len >> 20, (long long)(dsize >> 20), avail >> 20, ar, flex >> 20);

	static const int types[] = {3, 0};
	for (int type : types)
	{
		int r = sceKernelAllocateMainDirectMemory(len, k2M, type, &g.phys);
		if (TryDirect(type == 3 ? "AllocateMainDirectMemory type 3" : "AllocateMainDirectMemory type 0", r, len,
				k2M, w, h))
			return true;
		r = sceKernelAllocateDirectMemory(0, dsize, len, k2M, type, &g.phys);
		if (TryDirect(type == 3 ? "AllocateDirectMemory type 3" : "AllocateDirectMemory type 0", r, len, k2M, w,
				h))
			return true;
	}
	// last resort: flexible memory mapped for the GPU (not known to be accepted by the display yet)
	void* addr = nullptr;
	const int r = sceKernelMapNamedFlexibleMemory(&addr, len, 0x33, 0, "genplus scanout");
	OrbisLog("[video]   MapNamedFlexibleMemory(%zu MiB) -> %x at %p", len >> 20, r, addr);
	if (r == 0 && addr)
	{
		g.phys = 0;
		g.vmem = addr;
		g.vmem_len = len;
		if (TryRegister(w, h))
			return true;
		FreeScanout();
	}
	return false;
}

void SetOutputSize(int w, int h)
{
	g.out_w = w;
	g.out_h = h;
	g.colx.assign(w / 4 + 1, 0);
	for (uint32_t x = 0; x < uint32_t(w); x += 4)
		g.colx[x / 4] = (x / kTileW) * kTileSize + TileOffset(x % kTileW, 0);
	if (w != kWidth)
	{
		g.scaled.assign(size_t(w) * h, Rgb(0, 0, 0));
		g.xmap.resize(w);
		g.ymap.resize(h);
		for (int x = 0; x < w; x++)
			g.xmap[x] = uint32_t((int64_t(x) * kWidth + kWidth / 2) / w);
		for (int y = 0; y < h; y++)
			g.ymap[y] = uint32_t((int64_t(y) * kHeight + kHeight / 2) / h);
	}
	else
	{
		g.scaled.clear();
		g.xmap.clear();
		g.ymap.clear();
	}
	g.full[0] = g.full[1] = true;
}
} // namespace

bool Init()
{
	if (g.ok)
		return true;

	g.surface = static_cast<uint32_t*>(aligned_alloc(64, size_t(kWidth) * kHeight * 4));
	if (!g.surface)
	{
		OrbisLog("[video] out of memory for the surface");
		return false;
	}
	std::fill(g.surface, g.surface + size_t(kWidth) * kHeight, Rgb(0, 0, 0));
	for (uint32_t y = 0; y < kTileH; y++)
		g.yoff[y] = TileOffset(0, y);

	// 0x80290009: the screen is still held by another process (the 1.5 console log: a second copy started
	// while the first still ran). Give it a few seconds to let go.
	for (int attempt = 0;; attempt++)
	{
		g.handle = sceVideoOutOpen(SCE_USER_SERVICE_USER_ID_SYSTEM, 0, 0, nullptr);
		OrbisLog("[video] sceVideoOutOpen -> %d (%x)", g.handle, unsigned(g.handle));
		if (g.handle >= 0 || unsigned(g.handle) != 0x80290009u || attempt >= 10)
			break;
		usleep(500 * 1000);
	}
	if (g.handle < 0)
		return false;

	int r = sceKernelCreateEqueue(&g.flip_queue, "genplus flip");
	if (r != 0)
	{
		OrbisLog("[video] sceKernelCreateEqueue -> %x", r);
		return false;
	}
	r = sceVideoOutAddFlipEvent(g.flip_queue, g.handle, nullptr);
	if (r != 0)
	{
		OrbisLog("[video] sceVideoOutAddFlipEvent -> %x", r);
		return false;
	}
	sceVideoOutSetFlipRate(g.handle, 0); // 60 Hz

	// When started from the dashboard icon, the launcher app still holds the game memory for a moment
	// after it hands us over: keep trying for a few seconds before giving up.
	bool ok = false;
	for (int attempt = 0; attempt < 12 && !ok; attempt++)
	{
		if (attempt > 0)
		{
			OrbisLog("[video] no scan-out memory yet, retrying (%d)", attempt);
			usleep(500000);
		}
		static const int sizes[][2] = {{kWidth, kHeight}, {1280, 720}};
		for (const auto& sz : sizes)
		{
			if (AllocScanout(sz[0], sz[1]))
			{
				SetOutputSize(sz[0], sz[1]);
				ok = true;
				break;
			}
		}
	}
	if (!ok)
	{
		OrbisLog("[video] could not get memory for the scan-out buffers");
		return false;
	}
	OrbisLog("[video] scan-out %dx%d, %zu MiB of %s memory", g.out_w, g.out_h, g.vmem_len >> 20,
		g.phys ? "direct" : "flexible");

	g.ok = true;
	Present(0, 0, 0, 0, true); // black screen over the home screen
	return true;
}

void Shutdown()
{
	if (g.handle >= 0)
	{
		while (g.flips_in_flight > 0)
			WaitOneFlip();
		if (g.flip_queue)
			sceVideoOutDeleteFlipEvent(g.flip_queue, g.handle);
		sceVideoOutClose(g.handle);
		g.handle = -1;
	}
	if (g.flip_queue)
	{
		sceKernelDeleteEqueue(g.flip_queue);
		g.flip_queue = nullptr;
	}
	FreeScanout();
	free(g.surface);
	g.surface = nullptr;
	g.ok = false;
}

uint32_t* Surface()
{
	return g.surface;
}

void Present(int x, int y, int w, int h, bool wait_vsync)
{
	if (!g.ok)
		return;
	// The buffer about to be written may still be on screen until its last flip completes.
	while (g.flips_in_flight > 0)
		WaitOneFlip();

	R damage = (w <= 0 || h <= 0) ? R{0, 0, kWidth, kHeight} : R{x, y, w, h};
	Intersect(damage, R{0, 0, kWidth, kHeight});

	const int idx = g.frame & 1;
	TileBlit(idx, damage);

	const int r = sceVideoOutSubmitFlip(g.handle, idx, SCE_VIDEO_OUT_FLIP_MODE_VSYNC, g.frame);
	if (r != 0)
	{
		OrbisLog("[video] sceVideoOutSubmitFlip -> %x", r);
		return;
	}
	g.flips_in_flight++;
	g.frame++;
	if (wait_vsync)
		WaitOneFlip();
}

void FillRect(int x, int y, int w, int h, uint32_t color)
{
	R r{x, y, w, h};
	if (!g.surface || !Intersect(r, R{0, 0, kWidth, kHeight}))
		return;
	for (int j = r.y; j < r.y + r.h; j++)
		std::fill(g.surface + size_t(j) * kWidth + r.x, g.surface + size_t(j) * kWidth + r.x + r.w, color);
}

void DarkenRect(int x, int y, int w, int h)
{
	R r{x, y, w, h};
	if (!g.surface || !Intersect(r, R{0, 0, kWidth, kHeight}))
		return;
	for (int j = r.y; j < r.y + r.h; j++)
	{
		uint32_t* p = g.surface + size_t(j) * kWidth + r.x;
		for (int i = 0; i < r.w; i++)
			p[i] = ((p[i] >> 1) & 0x007f7f7fu) | 0xff000000u;
	}
}

// ---------------------------------------------------------------------------------------------------------
// The game picture
namespace
{
struct FrameGeom
{
	int src_w = 0, src_h = 0, base_h = 0;
	double aspect = 0;
	Scale scale = Scale::Count;
	bool smooth = false;
	bool scanlines = false;
	Rect dst = {0, 0, 0, 0};
	std::vector<uint32_t> xsrc; // dst column -> src column (nearest), or the left column (smooth)
	std::vector<uint16_t> xw; // smooth: weight of the right column, 0..256
	std::vector<uint32_t> ysrc; // dst row -> src row
	std::vector<uint16_t> yw; // smooth: weight of the lower row, 0..256
	std::vector<uint8_t> dark; // dst row is a scanline row
};
FrameGeom s_geom;

inline uint32_t ArgbToSurface(uint32_t c)
{
	// the frame: 0xAARRGGBB; the surface: 0xAABBGGRR
	return 0xff000000u | ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
}

inline uint32_t Lerp(uint32_t a, uint32_t b, uint32_t w) // w: weight of b, 0..256 (alpha byte dropped)
{
	const uint32_t iw = 256 - w;
	const uint32_t rb = (((a & 0x00ff00ffu) * iw + (b & 0x00ff00ffu) * w) >> 8) & 0x00ff00ffu;
	const uint32_t g = (((a & 0x0000ff00u) * iw + (b & 0x0000ff00u) * w) >> 8) & 0x0000ff00u;
	return rb | g;
}

Rect ComputeRect(int base_h, double aspect, Scale scale)
{
	int dw = kWidth, dh = kHeight;
	if (aspect <= 0.01)
		aspect = 4.0 / 3.0;
	switch (scale)
	{
		case Scale::Fit:
			dh = kHeight;
			dw = int(kHeight * aspect + 0.5);
			if (dw > kWidth)
			{
				dw = kWidth;
				dh = int(kWidth / aspect + 0.5);
			}
			break;
		case Scale::Integer:
		{
			const int k = std::max(1, kHeight / std::max(1, base_h));
			dh = std::min(kHeight, base_h * k);
			dw = int(dh * aspect + 0.5);
			if (dw > kWidth)
			{
				dw = kWidth;
				dh = std::min(dh, int(kWidth / aspect + 0.5));
			}
			break;
		}
		default:
			break;
	}
	dw = std::max(2, std::min(dw, int(kWidth)) & ~1);
	dh = std::max(1, std::min(dh, int(kHeight)));
	return Rect{(kWidth - dw) / 2, (kHeight - dh) / 2, dw, dh};
}

void Setup(int w, int h, int base_h, double aspect, Scale scale, bool smooth, bool scanlines)
{
	s_geom.src_w = w;
	s_geom.src_h = h;
	s_geom.base_h = base_h;
	s_geom.aspect = aspect;
	s_geom.scale = scale;
	s_geom.smooth = smooth;
	s_geom.scanlines = scanlines;
	s_geom.dst = ComputeRect(base_h, aspect, scale);
	const Rect& d = s_geom.dst;
	// Smoothing only when a source pixel doesn't land on a whole number of screen pixels both ways.
	const bool whole = (d.w % w) == 0 && (d.h % h) == 0;
	const bool lerp = smooth && !whole;
	s_geom.xsrc.resize(d.w);
	s_geom.xw.assign(d.w, 0);
	for (int x = 0; x < d.w; x++)
	{
		if (lerp)
		{
			const double fx = (x + 0.5) * w / d.w - 0.5;
			const int x0 = std::max(0, std::min(w - 1, int(std::floor(fx))));
			s_geom.xsrc[x] = uint32_t(x0);
			s_geom.xw[x] = x0 + 1 < w ? uint16_t(std::max(0.0, std::min(1.0, fx - x0)) * 256.0) : 0;
		}
		else
			s_geom.xsrc[x] = uint32_t((int64_t(x) * w) / d.w);
	}
	s_geom.ysrc.resize(d.h);
	s_geom.yw.assign(d.h, 0);
	s_geom.dark.assign(d.h, 0);
	for (int y = 0; y < d.h; y++)
	{
		if (lerp)
		{
			const double fy = (y + 0.5) * h / d.h - 0.5;
			const int y0 = std::max(0, std::min(h - 1, int(std::floor(fy))));
			s_geom.ysrc[y] = uint32_t(y0);
			s_geom.yw[y] = y0 + 1 < h ? uint16_t(std::max(0.0, std::min(1.0, fy - y0)) * 256.0) : 0;
		}
		else
			s_geom.ysrc[y] = uint32_t((int64_t(y) * h) / d.h);
		// scanlines follow the console's own line grid (base_h), filtered or not
		if (scanlines && y + 1 < d.h && d.h / std::max(1, base_h) >= 3)
		{
			const int line_here = int((int64_t(y) * base_h) / d.h);
			const int line_next = int((int64_t(y + 1) * base_h) / d.h);
			s_geom.dark[y] = line_here != line_next;
		}
	}
	FillRect(0, 0, kWidth, kHeight, Rgb(0, 0, 0));
	OrbisLog("[video] picture %dx%d (%d lines, aspect %.4f, %s%s%s) -> %d,%d %dx%d", w, h, base_h, aspect,
		ScaleName(scale), lerp ? ", smooth" : "", scanlines ? ", scanlines" : "", d.x, d.y, d.w, d.h);
}
} // namespace

const char* ScaleName(Scale s)
{
	switch (s)
	{
		case Scale::Fit: return "Fit to screen";
		case Scale::Integer: return "Integer scale";
		case Scale::Stretch: return "Stretch to 16:9";
		default: return "?";
	}
}

Rect LastFrameRect()
{
	return s_geom.dst;
}

void InvalidateFrame()
{
	s_geom.src_w = 0;
}

Rect DrawFrame(const uint32_t* argb, int w, int h, int base_h, double aspect, Scale scale, bool smooth, bool scanlines)
{
	if (!g.surface || !argb || w <= 0 || h <= 0)
		return Rect{0, 0, 0, 0};
	if (base_h <= 0)
		base_h = h;

	bool cleared = false;
	if (w != s_geom.src_w || h != s_geom.src_h || base_h != s_geom.base_h || std::fabs(aspect - s_geom.aspect) > 1e-6 ||
		scale != s_geom.scale || smooth != s_geom.smooth || scanlines != s_geom.scanlines)
	{
		Setup(w, h, base_h, aspect, scale, smooth, scanlines);
		cleared = true;
	}

	const Rect& d = s_geom.dst;
	const uint32_t* xs = s_geom.xsrc.data();
	const uint16_t* xw = s_geom.xw.data();
	const bool lerp = smooth && !((d.w % w) == 0 && (d.h % h) == 0);
	uint32_t* prev = nullptr; // last row written at full brightness
	int prev_src = -1;
	uint32_t prev_w = 0;
	for (int y = 0; y < d.h; y++)
	{
		uint32_t* out = g.surface + size_t(d.y + y) * kWidth + d.x;
		const int sy = int(s_geom.ysrc[y]);
		const uint32_t wy = s_geom.yw[y];
		if (prev && sy == prev_src && wy == prev_w)
		{
			if (s_geom.dark[y])
				for (int x = 0; x < d.w; x++)
					out[x] = ((prev[x] >> 1) & 0x007f7f7fu) | 0xff000000u;
			else
				memcpy(out, prev, size_t(d.w) * 4);
			continue;
		}
		const uint32_t* in = argb + size_t(sy) * w;
		if (!lerp)
		{
			for (int x = 0; x < d.w; x++)
				out[x] = ArgbToSurface(in[xs[x]]);
		}
		else
		{
			const uint32_t* in2 = wy ? in + w : in;
			for (int x = 0; x < d.w; x++)
			{
				const uint32_t x0 = xs[x];
				const uint32_t x1 = xw[x] ? x0 + 1 : x0;
				const uint32_t top = Lerp(in[x0], in[x1], xw[x]);
				const uint32_t bot = wy ? Lerp(in2[x0], in2[x1], xw[x]) : top;
				out[x] = ArgbToSurface(Lerp(top, bot, wy));
			}
		}
		if (s_geom.dark[y])
			for (int x = 0; x < d.w; x++)
				out[x] = ((out[x] >> 1) & 0x007f7f7fu) | 0xff000000u;
		else
		{
			prev = out;
			prev_src = sy;
			prev_w = wy;
		}
	}
	return cleared ? Rect{0, 0, kWidth, kHeight} : d;
}

void BlendOverlay(const uint32_t* argb, int w, int h, const uint8_t* row_used)
{
	if (!g.surface || !argb || w <= 0 || h <= 0)
		return;
	// The overlay covers the whole screen, scaled from its own size.
	for (int y = 0; y < kHeight; y++)
	{
		const int sy = int((int64_t(y) * h) / kHeight);
		if (row_used && !row_used[sy])
			continue;
		const uint32_t* in = argb + size_t(sy) * w;
		uint32_t* out = g.surface + size_t(y) * kWidth;
		for (int x = 0; x < kWidth; x++)
		{
			const uint32_t c = in[(int64_t(x) * w) / kWidth];
			const uint32_t a = c >> 24;
			if (a == 0)
				continue;
			const uint32_t src = ArgbToSurface(c) & 0x00ffffffu;
			out[x] = a >= 255 ? (src | 0xff000000u) : (Lerp(out[x], src, a + (a >> 7)) | 0xff000000u);
		}
	}
}
} // namespace ps5video

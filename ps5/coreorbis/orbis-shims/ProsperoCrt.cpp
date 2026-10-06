// Genesis Plus GX PS5: CRT shaders on the CPU (ProsperoCrt.h).
//
// The shaders ported here come from libretro's slang-shaders (https://github.com/libretro/slang-shaders, crt/),
// with their default parameters. Their authors and licences:
//   - crt-lottes, crt-lottes-fast: Timothy Lottes, public domain (crt-lottes-fast: "CRT Simple" / CRTS, public
//     domain); slang ports by hunterk and others.
//   - crt-1tap, crt-2tap: fishku, public domain (CC0).
//   - monoCRT: hunterk, public domain.
//   - newpixie-mini: Mattias Gustavsson, public domain (Unlicense) or MIT, at the user's choice; slang port by
//     hunterk. Used here under the Unlicense.
//   - crt-hyllian-fast: Copyright (C) 2011-2015 Hyllian - sergiogdb@gmail.com, MIT licence (below); GLSL/slang port
//     by DariusG & hunterk, with cgwg's magenta/green dot mask.
//   - crt-nobody: Copyright (C) 2011-2025 Hyllian - sergiogdb@gmail.com, MIT licence (below).
//   - crt-blurPi: Oriol Ferrer Mesia (armadillu, http://uri.cat), MIT licence (below).
// The "Easymode style" shader is original code written for this port: it aims at the look of EasyMode's
// crt-easymode (flat screen, sharp Lanczos horizontal filter, scanlines that widen with brightness, an aperture
// grille), not at its code, which is GPL.
//
// MIT licence of crt-hyllian-fast, crt-nobody and crt-blurPi (their copyright lines are above):
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without
//   limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
//   Software, and to permit persons to whom the Software is furnished to do so, subject to the following
//   conditions: The above copyright notice and this permission notice shall be included in all copies or
//   substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
//   TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
//   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
//
// How a GPU shader becomes CPU work here: the screen rectangle is dw x dh, the source w x h. In the shaders,
// u = (X + 0.5) / dw and v = (Y + 0.5) / dh are the pixel's texture coordinates. Anything that depends only on a
// source line and on u (the horizontal filter) is computed once per source line and screen column into a "row
// buffer"; per screen pixel only the vertical part, the mask and a gamma table remain. Curved shaders keep a
// per-pixel map (where each screen pixel looks in the source) and read the row buffers between two columns.
//
// SPDX-License-Identifier: MIT
#include "ProsperoCrt.h"

#include "ProsperoThread.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ps5crt
{
namespace
{
// ---- small vector maths ------------------------------------------------------------------------------------
struct V3
{
	float r, g, b;
};
inline V3 operator+(V3 a, V3 b)
{
	return {a.r + b.r, a.g + b.g, a.b + b.b};
}
inline V3 operator-(V3 a, V3 b)
{
	return {a.r - b.r, a.g - b.g, a.b - b.b};
}
inline V3 operator*(V3 a, float s)
{
	return {a.r * s, a.g * s, a.b * s};
}
inline V3 operator*(V3 a, V3 b)
{
	return {a.r * b.r, a.g * b.g, a.b * b.b};
}
inline V3 Mix(V3 a, V3 b, float t)
{
	return a + (b - a) * t;
}
inline float Max3(V3 c)
{
	return std::max(c.r, std::max(c.g, c.b));
}
inline float Sat(float v)
{
	return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}
inline float Fract(float v)
{
	return v - std::floor(v);
}
inline int Clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}
inline float SmoothStep(float e0, float e1, float x)
{
	const float t = Sat((x - e0) / (e1 - e0));
	return t * t * (3.f - 2.f * t);
}
inline uint32_t Pack(uint8_t r, uint8_t g, uint8_t b) // the surface: 0xAABBGGRR
{
	return 0xff000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
}
inline uint8_t To8(float v)
{
	return v <= 0.f ? 0 : (v >= 1.f ? 255 : uint8_t(v * 255.f + 0.5f));
}

// ---- tables --------------------------------------------------------------------------------------------------
// A [0,1] -> 8-bit transfer curve (gamma out), 65536 steps.
struct OutLut
{
	std::vector<uint8_t> t;
	template <class F>
	void Build(F f)
	{
		t.resize(65536);
		for (int i = 0; i < 65536; i++)
			t[size_t(i)] = To8(f(float(i) / 65535.f));
	}
	uint8_t operator()(float v) const
	{
		if (!(v > 0.f))
			return t[0];
		if (v >= 1.f)
			return t[65535];
		return t[size_t(v * 65535.f + 0.5f)];
	}
};
// A [0,1] -> float curve (gamma in after filtering), 4096 steps, linear between them.
struct InLut
{
	std::vector<float> t;
	template <class F>
	void Build(F f)
	{
		t.resize(4097);
		for (int i = 0; i <= 4096; i++)
			t[size_t(i)] = f(float(i) / 4096.f);
	}
	float operator()(float v) const
	{
		if (!(v > 0.f))
			return t[0];
		if (v >= 1.f)
			return t[4096];
		const float p = v * 4096.f;
		const int i = int(p);
		return t[size_t(i)] + (t[size_t(i) + 1] - t[size_t(i)]) * (p - float(i));
	}
};

float SrgbToLinear(float c)
{
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float LinearToSrgb(float c) // crt-lottes' ToSrgb
{
	return c < 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 0.41666f) - 0.055f;
}

// ---- the source picture as floats --------------------------------------------------------------------------
struct Src
{
	int w = 0, h = 0;
	std::vector<V3> px;
	// lut: the 8-bit value -> float (identity /255 when null)
	void Build(const uint32_t* argb, int sw, int sh, const float* lut)
	{
		w = sw;
		h = sh;
		px.resize(size_t(w) * h);
		for (size_t i = 0; i < px.size(); i++)
		{
			const uint32_t c = argb[i];
			const int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
			px[i] = lut ? V3{lut[r], lut[g], lut[b]} : V3{r / 255.f, g / 255.f, b / 255.f};
		}
	}
	V3 Clamp(int x, int y) const
	{
		return px[size_t(Clampi(y, 0, h - 1)) * w + Clampi(x, 0, w - 1)];
	}
	V3 Border(int x, int y) const // clamp_to_border: black outside
	{
		if (unsigned(x) >= unsigned(w) || unsigned(y) >= unsigned(h))
			return {0.f, 0.f, 0.f};
		return px[size_t(y) * w + x];
	}
	// nearest texel at texture coordinates (u, v), black outside
	V3 Nearest(float u, float v) const
	{
		return Border(int(std::floor(u * w)), int(std::floor(v * h)));
	}
	// bilinear at texture coordinates, black outside
	V3 Bilinear(float u, float v) const
	{
		const float x = u * w - 0.5f, y = v * h - 0.5f;
		const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
		const float fx = x - x0, fy = y - y0;
		return Mix(Mix(Border(x0, y0), Border(x0 + 1, y0), fx), Mix(Border(x0, y0 + 1), Border(x0 + 1, y0 + 1), fx), fy);
	}
};

// One value per source line (or per "virtual" line) and screen column.
struct Rows
{
	int n = 0, dw = 0;
	std::vector<V3> d;
	std::vector<V3> zero;
	void Size(int lines, int cols)
	{
		n = lines;
		dw = cols;
		d.resize(size_t(n) * dw);
		zero.assign(size_t(cols) + 2, V3{0.f, 0.f, 0.f});
	}
	V3* Row(int r)
	{
		return d.data() + size_t(r) * dw;
	}
	const V3* RowOr0(int r) const // black outside the picture
	{
		return unsigned(r) < unsigned(n) ? d.data() + size_t(r) * dw : zero.data();
	}
	const V3* RowClamp(int r) const
	{
		return d.data() + size_t(Clampi(r, 0, n - 1)) * dw;
	}
};
// Where a curved pixel reads the row buffers: two columns and the weight of the second; inside == false when
// the pixel looks outside the picture (black).
struct ColPos
{
	int c0, c1;
	float t;
	bool inside;
};
inline ColPos ColAt(int dw, float col)
{
	ColPos p;
	p.inside = col > -0.5f && col < float(dw) - 0.5f;
	const float c = std::max(0.f, col);
	p.c0 = std::min(int(c), dw - 1);
	p.c1 = std::min(p.c0 + 1, dw - 1);
	p.t = c - float(p.c0);
	return p;
}
inline V3 At(const V3* row, const ColPos& p)
{
	return Mix(row[p.c0], row[p.c1], p.t);
}

// ---- threads ---------------------------------------------------------------------------------------------------
class Pool
{
public:
	static Pool& Get()
	{
		static Pool* p = new Pool; // lives until the process ends: its threads wait for work forever
		return *p;
	}
	// fn(i0, i1) over [0, n) in chunks, on every thread (the caller's included); returns when all are done.
	void For(int n, const std::function<void(int, int)>& fn)
	{
		if (n <= 0)
			return;
		if (m_count <= 1 || n < 8)
		{
			fn(0, n);
			return;
		}
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_fn = &fn;
			m_n = n;
			m_chunks = std::min(n, m_count * 6);
			m_next = 0;
			m_pending = m_count - 1;
			m_gen++;
		}
		m_cv.notify_all();
		Work(&fn, n, std::min(n, m_count * 6));
		std::unique_lock<std::mutex> lock(m_lock);
		m_done.wait(lock, [this] { return m_pending == 0; });
		m_fn = nullptr;
	}

private:
	Pool()
	{
		unsigned n = std::thread::hardware_concurrency();
		n = std::clamp(n == 0 ? 4u : n, 1u, 6u);
		m_count = int(n);
		for (unsigned i = 1; i < n; i++)
			m_threads.emplace_back([this] { Worker(); }, 256 * 1024);
	}
	void Work(const std::function<void(int, int)>* fn, int n, int chunks)
	{
		for (;;)
		{
			const int c = m_next.fetch_add(1);
			if (c >= chunks)
				return;
			const int i0 = int(int64_t(n) * c / chunks), i1 = int(int64_t(n) * (c + 1) / chunks);
			if (i0 < i1)
				(*fn)(i0, i1);
		}
	}
	void Worker()
	{
		uint64_t seen = 0;
		for (;;)
		{
			const std::function<void(int, int)>* fn;
			int n, chunks;
			{
				std::unique_lock<std::mutex> lock(m_lock);
				m_cv.wait(lock, [&] { return m_gen != seen; });
				seen = m_gen;
				fn = m_fn;
				n = m_n;
				chunks = m_chunks;
			}
			if (fn)
				Work(fn, n, chunks);
			{
				std::lock_guard<std::mutex> lock(m_lock);
				if (--m_pending == 0)
					m_done.notify_one();
			}
		}
	}
	std::vector<ps5::BigThread> m_threads; // the pool lives as long as the app; never joined
	int m_count = 1;
	std::mutex m_lock;
	std::condition_variable m_cv, m_done;
	uint64_t m_gen = 0;
	int m_pending = 0;
	const std::function<void(int, int)>* m_fn = nullptr;
	int m_n = 0, m_chunks = 0;
	std::atomic<int> m_next{0};
};

void ParallelFor(int n, const std::function<void(int, int)>& fn)
{
	Pool::Get().For(n, fn);
}

// What every shader is given.
struct Job
{
	const uint32_t* argb;
	int w, h;
	uint32_t* surf; // the rectangle's top left
	int pitch;
	int dx, dw, dh; // dx: the rectangle's left edge on the screen (masks follow the screen's pixels)
	uint64_t frame;
};

// ===================================================================================================================
// Easymode style (original). Horizontal: 4-tap Lanczos (a = 2) on the gamma-encoded picture, its phase pushed a
// little towards the nearest texel for sharpness, ringing clamped to the two middle texels. Vertical: the two
// nearest lines blended with a smoothstep. Light: gamma 2.0 in; each line is a beam whose profile is a Gaussian
// that gets wider as the colour gets brighter, so dark lines show more black between them; aperture grille
// (R, G, B columns) at 30%; gamma 1/1.8 out with a 1.2 brightness boost.
// ===================================================================================================================
struct Easymode
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	std::vector<int> cx; // per column: the left one of the two middle texels
	std::vector<float> cwt; // per column: 4 weights
	Src src;
	Rows rows;
	OutLut out;
	static constexpr int kLevels = 64; // brightness steps of the beam table
	std::vector<float> beam; // per screen row: kLevels factors
	std::vector<int> row0;
	std::vector<float> rowt;

	static float Lanczos2(float x)
	{
		x = std::fabs(x);
		if (x < 1e-5f)
			return 1.f;
		if (x >= 2.f)
			return 0.f;
		const float px = 3.14159265f * x;
		return 2.f * std::sin(px) * std::sin(px * 0.5f) / (px * px);
	}
	void Setup(int w, int h, int dw, int dh)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh;
		cx.resize(size_t(dw));
		cwt.resize(size_t(dw) * 4);
		for (int c = 0; c < dw; c++)
		{
			const float p = (c + 0.5f) * w / dw - 0.5f;
			const int i = int(std::floor(p));
			float f = p - i;
			f += (f * f * (3.f - 2.f * f) - f) * 0.35f; // sharper: towards the nearer texel
			float k[4] = {Lanczos2(1.f + f), Lanczos2(f), Lanczos2(1.f - f), Lanczos2(2.f - f)};
			const float s = k[0] + k[1] + k[2] + k[3];
			cx[size_t(c)] = i;
			for (int j = 0; j < 4; j++)
				cwt[size_t(c) * 4 + j] = k[j] / s;
		}
		row0.resize(size_t(dh));
		rowt.resize(size_t(dh));
		beam.resize(size_t(dh) * kLevels);
		const bool lines = h < 400; // interlaced / high-resolution pictures: no scanlines
		for (int y = 0; y < dh; y++)
		{
			const float sy = (y + 0.5f) * h / dh;
			const float p = sy - 0.5f;
			const int r = int(std::floor(p));
			float t = p - r;
			t = t * t * (3.f - 2.f * t);
			row0[size_t(y)] = r;
			rowt[size_t(y)] = t;
			const float d = std::fabs(Fract(sy) - 0.5f) * 2.f; // 0 at a line's centre, 1 between two lines
			for (int l = 0; l < kLevels; l++)
			{
				const float b = float(l) / (kLevels - 1);
				float f = 1.f;
				if (lines)
				{
					const float width = 0.62f + 0.45f * b; // the beam widens with brightness
					const float g = std::exp(-2.0f * (d / width) * (d / width));
					const float floor_ = 0.30f + 0.32f * b; // light that bleeds between the lines
					f = floor_ + (1.f - floor_) * g;
				}
				beam[size_t(y) * kLevels + l] = f;
			}
		}
		rows.Size(h, dw);
		if (out.t.empty())
			out.Build([](float v) { return std::min(1.f, std::pow(v, 1.f / 1.8f) * 1.2f); });
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
			Setup(j.w, j.h, j.dw, j.dh);
		src.Build(j.argb, j.w, j.h, nullptr);
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				V3* o = rows.Row(r);
				for (int c = 0; c < j.dw; c++)
				{
					const int i = cx[size_t(c)];
					const float* k = &cwt[size_t(c) * 4];
					const V3 a = src.Clamp(i - 1, r), b = src.Clamp(i, r), cc = src.Clamp(i + 1, r), d = src.Clamp(i + 2, r);
					V3 v = a * k[0] + b * k[1] + cc * k[2] + d * k[3];
					// no ringing beyond the two middle texels
					v.r = std::min(std::max(v.r, std::min(b.r, cc.r)), std::max(b.r, cc.r));
					v.g = std::min(std::max(v.g, std::min(b.g, cc.g)), std::max(b.g, cc.g));
					v.b = std::min(std::max(v.b, std::min(b.b, cc.b)), std::max(b.b, cc.b));
					o[c] = v;
				}
			}
		});
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const V3* a = rows.RowClamp(row0[size_t(y)]);
				const V3* b = rows.RowClamp(row0[size_t(y)] + 1);
				const float t = rowt[size_t(y)];
				const float* bt = &beam[size_t(y) * kLevels];
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				int m = (j.dx) % 3;
				for (int c = 0; c < j.dw; c++)
				{
					V3 v = Mix(a[c], b[c], t);
					v = v * v; // gamma 2.0 in
					const float luma = 0.2126f * v.r + 0.7152f * v.g + 0.0722f * v.b;
					const float bright = Sat(0.5f * (Max3(v) + luma));
					v = v * bt[int(bright * (kLevels - 1) + 0.5f)];
					// aperture grille: the column's own phosphor at full strength, the other two at 70%
					const float mr = m == 0 ? 1.f : 0.7f, mg = m == 1 ? 1.f : 0.7f, mb = m == 2 ? 1.f : 0.7f;
					o[c] = Pack(out(v.r * mr), out(v.g * mg), out(v.b * mb));
					if (++m == 3)
						m = 0;
				}
			}
		});
	}
};

// ===================================================================================================================
// crt-1tap / crt-2tap (fishku, CC0): MIN_THICK 0.1, MAX_THICK 0.95, H_SMOOTH 0.8, V_SMOOTH 1, SUBPX_POS 0.31,
// THICK_FALLOFF 0.45; bilinear source (filter_linear0 = true).
// ===================================================================================================================
struct Ntap
{
	bool two;
	explicit Ntap(bool two_taps) : two(two_taps) {}
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	std::vector<int> cx;
	std::vector<float> cf;
	Src src;
	Rows rows;
	float width_tab[1025]; // signal (1024 steps) -> beam width
	bool tables = false;
	OutLut sqrt_lut;
	float Width(float s) const
	{
		return width_tab[s <= 0.f ? 0 : (s >= 1.f ? 1024 : int(s * 1024.f + 0.5f))];
	}
	static constexpr float kMin = 0.1f, kMax = 0.95f, kHSmooth = 0.8f, kVSmooth = 1.f, kSubpx = 0.31f, kFalloff = 0.45f;

	void Setup(int w, int h, int dw, int dh)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh;
		cx.resize(size_t(dw));
		cf.resize(size_t(dw));
		const float slope = 6.f + (1.f - 6.f) * kHSmooth;
		for (int c = 0; c < dw; c++)
		{
			// get_sample_x: a smooth-sign curve inside each texel, then one bilinear tap
			const float src_x = (c + 0.5f) / dw * w - 0.5f;
			const float xi = std::floor(src_x);
			const float x = 2.f * (src_x - xi) - 1.f;
			const float off = 0.5f + 0.5f * slope * x / std::sqrt(1.f + (slope * slope - 1.f) * x * x);
			cx[size_t(c)] = int(xi); // the bilinear tap at xi + off + 0.5 blends texels xi and xi + 1
			cf[size_t(c)] = off;
		}
		rows.Size(h, dw);
		if (!tables)
		{
			for (int i = 0; i <= 1024; i++)
				width_tab[i] = std::min(kMin + (kMax - kMin) * std::pow(i / 1024.f, kFalloff), 1.f);
			sqrt_lut.Build([](float v) { return std::sqrt(v); });
			tables = true;
		}
	}
	// get_beam_prefix(y, width) = cell * width + clamp(phase - 0.5 + 0.5 width, 0, width), cell and phase of y
	struct Y
	{
		float cell, phase;
	};
	static Y Split(float y)
	{
		const float c = std::floor(y);
		return {c, y - c};
	}
	static float Prefix(Y y, float width)
	{
		return y.cell * width + std::min(std::max(y.phase - 0.5f + 0.5f * width, 0.f), width);
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
			Setup(j.w, j.h, j.dw, j.dh);
		src.Build(j.argb, j.w, j.h, nullptr);
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				V3* o = rows.Row(r);
				for (int c = 0; c < j.dw; c++)
					o[c] = Mix(src.Clamp(cx[size_t(c)], r), src.Clamp(cx[size_t(c)] + 1, r), cf[size_t(c)]);
			}
		});
		const float fh = float(j.h) / j.dh * kVSmooth;
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const float sy = (y + 0.5f) / j.dh * j.h - kSubpx;
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				if (!two)
				{
					const V3* s = rows.RowClamp(int(std::floor(sy)));
					const Y lo = Split(sy - 0.5f * fh), hi = Split(sy + 0.5f * fh);
					for (int c = 0; c < j.dw; c++)
					{
						const V3 sig = s[c];
						const float wr = Width(sig.r), wg = Width(sig.g), wb = Width(sig.b);
						const float ar = Prefix(hi, wr) - Prefix(lo, wr), ag = Prefix(hi, wg) - Prefix(lo, wg),
									ab = Prefix(hi, wb) - Prefix(lo, wb);
						o[c] = Pack(sqrt_lut(sig.r * sig.r * ar / fh), sqrt_lut(sig.g * sig.g * ag / fh),
							sqrt_lut(sig.b * sig.b * ab / fh));
					}
				}
				else
				{
					const int r0 = int(std::floor(sy - 0.5f));
					const V3* s0 = rows.RowClamp(r0);
					const V3* s1 = rows.RowClamp(r0 + 1);
					const float lbf = sy - 0.5f * fh, ubf = sy + 0.5f * fh;
					const Y lb = Split(lbf), ub = Split(ubf), split = Split(std::min(std::max(float(r0) + 1.f, lbf), ubf));
					for (int c = 0; c < j.dw; c++)
					{
						const V3 a = s0[c], b = s1[c];
						auto ch = [&](float sa, float sb) {
							const float wa = Width(sa), wb = Width(sb);
							const float area0 = Prefix(split, wa) - Prefix(lb, wa);
							const float area1 = Prefix(ub, wb) - Prefix(split, wb);
							return sqrt_lut((sa * sa * area0 + sb * sb * area1) / fh);
						};
						o[c] = Pack(ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b));
					}
				}
			}
		});
	}
};

// ===================================================================================================================
// crt-hyllian-fast (Hyllian, MIT): MASK_INTENSITY 0.5, InputGamma 2.4, OutputGamma 2.2, BRIGHTBOOST 1.5,
// SCANLINES 0.72, SHARPER 0 (Catmull-Rom horizontally); nearest source.
// ===================================================================================================================
struct HyllianFast
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	std::vector<int> cx;
	std::vector<float> cwt;
	std::vector<uint8_t> parity;
	Src src;
	Rows rows;
	InLut gin;
	OutLut gout;
	static constexpr float kMask = 0.5f, kBoost = 1.5f, kScan = 0.72f;

	void Setup(int w, int h, int dw, int dh)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh;
		cx.resize(size_t(dw));
		cwt.resize(size_t(dw) * 4);
		parity.resize(size_t(dw));
		for (int c = 0; c < dw; c++)
		{
			// vTexCoord.x = TexCoord.x - 0.49999 texel
			const float X = (c + 0.5f) / dw * w - 0.49999f;
			const float xi = std::floor(X);
			const float fp = X - xi;
			const float l3 = fp * fp * fp, l2 = fp * fp, l1 = fp;
			cx[size_t(c)] = int(xi);
			cwt[size_t(c) * 4 + 0] = -0.5f * l3 + 1.0f * l2 - 0.5f * l1;
			cwt[size_t(c) * 4 + 1] = 1.5f * l3 - 2.5f * l2 + 1.0f;
			cwt[size_t(c) * 4 + 2] = -1.5f * l3 + 2.0f * l2 + 0.5f * l1;
			cwt[size_t(c) * 4 + 3] = 0.5f * l3 - 0.5f * l2;
			const float mod_factor = X / w * dw; // vTexCoord.x * OutputSize.x
			parity[size_t(c)] = uint8_t(int(std::floor(mod_factor - 2.f * std::floor(mod_factor / 2.f))) & 1);
		}
		rows.Size(h, dw);
		if (gin.t.empty())
		{
			gin.Build([](float v) { return std::pow(v, 2.4f); });
			gout.Build([](float v) { return std::pow(v, 1.f / 2.2f); });
		}
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
			Setup(j.w, j.h, j.dw, j.dh);
		src.Build(j.argb, j.w, j.h, nullptr);
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				V3* o = rows.Row(r);
				for (int c = 0; c < j.dw; c++)
				{
					const int i = cx[size_t(c)];
					const float* k = &cwt[size_t(c) * 4];
					const V3 v = src.Clamp(i - 1, r) * k[0] + src.Clamp(i, r) * k[1] + src.Clamp(i + 1, r) * k[2] +
						src.Clamp(i + 2, r) * k[3];
					o[c] = V3{gin(v.r), gin(v.g), gin(v.b)};
				}
			}
		});
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const float Y = (y + 0.5f) / j.dh * j.h;
				const float fy = Fract(Y);
				const V3* s = rows.RowClamp(int(std::floor(Y)));
				const float d1 = Sat(1.5f - kScan - std::fabs(fy - 0.5f));
				const float d = d1 * d1 * (3.f + kBoost - 2.f * d1);
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				for (int c = 0; c < j.dw; c++)
				{
					const V3 v = s[c] * d;
					if (parity[size_t(c)] == 0)
						o[c] = Pack(gout(v.r), gout(v.g * (1.f - kMask)), gout(v.b));
					else
						o[c] = Pack(gout(v.r * (1.f - kMask)), gout(v.g), gout(v.b * (1.f - kMask)));
				}
			}
		});
	}
};

// ===================================================================================================================
// crt-blurPi (Oriol Ferrer Mesia, MIT): scanlineGain 0.30, rgbExtraGain 0.10, blurGain 0.15, blurRadius 1.5.
// sharp: nearest source; soft: bilinear source.
// ===================================================================================================================
struct BlurPi
{
	bool soft;
	explicit BlurPi(bool linear) : soft(linear) {}
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	Src src;
	Rows main_rows, side_rows; // per source line: centre + left/right taps, and the centre tap alone
	static constexpr float kScan = 0.30f, kExtra = 0.10f, kBlur = 0.15f, kRadius = 1.5f;

	// One source line sampled horizontally at u (nearest or linear).
	V3 Tap(int r, float x) const // x in texels
	{
		if (!soft)
			return src.Border(int(std::floor(x)), r);
		const float p = x - 0.5f;
		const int i = int(std::floor(p));
		return Mix(src.Border(i, r), src.Border(i + 1, r), p - i);
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
		{
			key_w = j.w, key_h = j.h, key_dw = j.dw, key_dh = j.dh;
			main_rows.Size(j.h, j.dw);
			side_rows.Size(j.h, j.dw);
		}
		src.Build(j.argb, j.w, j.h, nullptr);
		const float g0 = (1.f - 0.75f * kBlur) * (1.f + kExtra), g1 = 0.25f * kBlur;
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				V3* m = main_rows.Row(r);
				V3* s = side_rows.Row(r);
				for (int c = 0; c < j.dw; c++)
				{
					const float x = (c + 0.5f) / j.dw * j.w;
					const V3 centre = Tap(r, x);
					m[c] = centre * g0 + (Tap(r, x - kRadius) + Tap(r, x + kRadius)) * g1;
					s[c] = centre * g1;
				}
			}
		});
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const float Y = (y + 0.5f) / j.dh * j.h; // v * SourceSize.y
				// mod(int(v * OutputSize.y), 2): every other screen row at full light, the others at 70%
				const float scan = (y & 1) ? 1.f : 1.f - kScan;
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				auto line = [&](const Rows& rows, float yy, int c) {
					if (!soft)
						return rows.RowOr0(int(std::floor(yy)))[c];
					const float p = yy - 0.5f;
					const int i = int(std::floor(p));
					return Mix(rows.RowOr0(i)[c], rows.RowOr0(i + 1)[c], p - i);
				};
				for (int c = 0; c < j.dw; c++)
				{
					const V3 v = (line(main_rows, Y, c) + line(side_rows, Y + kRadius, c)) * scan;
					o[c] = Pack(To8(v.r), To8(v.g), To8(v.b));
				}
			}
		});
	}
};

// ===================================================================================================================
// monoCRT (hunterk, public domain): brightBoost 1, scanMax 2.2, horzSharp 2, gammaIn 2.2, gammaOut 2.2, white.
// The shader's beam jitter (a few ten-thousandths of the screen, random every frame) is left out.
// ===================================================================================================================
struct MonoCrt
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	Src src;
	std::vector<float> env; // per source line and column: the line's local brightness
	std::vector<float> lin_lut;
	InLut gout; // pow(1 / 2.2)

	void Render(const Job& j)
	{
		if (lin_lut.empty())
		{
			lin_lut.resize(256);
			for (int i = 0; i < 256; i++)
				lin_lut[size_t(i)] = std::pow(i / 255.f, 2.2f);
			gout.Build([](float v) { return std::pow(v, 1.f / 2.2f); });
		}
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
		{
			key_w = j.w, key_h = j.h, key_dw = j.dw, key_dh = j.dh;
			env.resize(size_t(j.h) * j.dw);
		}
		src.Build(j.argb, j.w, j.h, lin_lut.data());
		const int scale_y = int(float(j.dh) / j.h);
		const int samples = std::clamp((scale_y * 3 - 1) / 2, 1, 33);
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				float* e = &env[size_t(r) * j.dw];
				for (int c = 0; c < j.dw; c++)
				{
					const float u = (c + 0.5f) / j.dw + 0.01f; // vTexCoord.x is moved by 0.01
					float sum = 0.f;
					for (int i = 0; i < samples; i++)
					{
						const V3 s = src.Border(int(std::floor((u + float(i - 16) / j.dw) * j.w)), r);
						sum += 0.299f * s.r + 0.587f * s.g + 0.114f * s.b;
					}
					e[c] = gout(sum / samples);
				}
			}
		});
		const float amp = 0.6f / j.h, edge = (30.f - 1.f) / 10000.f;
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const float v = (y + 0.5f) / j.dh;
				const int row = Clampi(int(std::floor(v * j.h)), 0, j.h - 1);
				const float dist = std::fabs(v - (row + 0.5f) / j.h);
				const float* e = &env[size_t(row) * j.dw];
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				for (int c = 0; c < j.dw; c++)
				{
					const float half = e[c] * 2.2f * amp * 0.5f;
					const uint8_t g = To8(SmoothStep(half, half - edge, dist));
					o[c] = Pack(g, g, g);
				}
			}
		});
	}
};

// ===================================================================================================================
// crt-lottes-fast (CRTS, Timothy Lottes, public domain): MASK 1, MASK_INTENSITY 0.5, SCANLINE_THINNESS 0.5,
// SCAN_BLUR 2.5, CURVATURE 0.02, TRINITRON_CURVE 0, CORNER 3, CRT_GAMMA 2.4; 4 taps across, 2 lines.
// ===================================================================================================================
struct LottesFast
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	Src src;
	Rows rows; // per source line: the 4-tap Gaussian at each column's (flat) position
	std::vector<float> map; // per pixel: source x, source y (texels), vin
	std::vector<float> scan_lut; // 1025 x (scanA, scanB)
	OutLut out;
	std::vector<float> lin;
	float tone_y = 0, tone_z = 0;
	std::vector<int> cx0;
	std::vector<float> cwt;
	static constexpr float kThin = 0.5f + 0.5f * 0.5f, kBlur = -2.5f, kMaskDark = 1.f - 0.5f, kCurv = 0.02f;

	static void Taps(float x, int* x0, float* wt) // 4 Gaussian weights around x (texels)
	{
		const float xs = std::floor(x - 1.5f) + 0.5f;
		*x0 = int(xs - 0.5f);
		const float off0 = x - xs;
		float s = 0.f;
		for (int k = 0; k < 4; k++)
		{
			const float o = off0 - float(k);
			wt[k] = std::exp2(kBlur * o * o);
			s += wt[k];
		}
		for (int k = 0; k < 4; k++)
			wt[k] /= s;
	}
	void Setup(int w, int h, int dw, int dh)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh;
		rows.Size(h, dw);
		cx0.resize(size_t(dw));
		cwt.resize(size_t(dw) * 4);
		for (int c = 0; c < dw; c++)
			Taps((c + 0.5f) / dw * w, &cx0[size_t(c)], &cwt[size_t(c) * 4]);
		map.resize(size_t(dw) * dh * 3);
		const float wx = kCurv, wy = 0.75f * kCurv; // warp: x * (1 + y^2 wx), y * (1 + x^2 wy)
		for (int y = 0; y < dh; y++)
			for (int x = 0; x < dw; x++)
			{
				float px = (x + 0.5f) * 2.f / dw - 1.f, py = (y + 0.5f) * 2.f / dh - 1.f;
				const float nx = px * (1.f + py * py * wx), ny = py * (1.f + px * px * wy);
				float vin = 1.f - ((1.f - Sat(nx * nx)) * (1.f - Sat(ny * ny))) * (0.998f + 0.001f * 3.f);
				vin = Sat(-vin * h + h);
				float* m = &map[(size_t(y) * dw + x) * 3];
				m[0] = nx * w * 0.5f + w * 0.5f;
				m[1] = ny * h * 0.5f + h * 0.5f;
				m[2] = vin;
			}
		if (scan_lut.empty())
		{
			scan_lut.resize(1025 * 2);
			for (int i = 0; i <= 1024; i++)
			{
				const float off = i / 1024.f, pi2 = 6.28318530717958f;
				scan_lut[size_t(i) * 2] = std::cos(std::min(0.5f, off * kThin) * pi2) * 0.5f + 0.5f;
				scan_lut[size_t(i) * 2 + 1] = std::cos(std::min(0.5f, -off * kThin + kThin) * pi2) * 0.5f + 0.5f;
			}
			out.Build(LinearToSrgb);
			lin.resize(256);
			for (int i = 0; i < 256; i++)
			{
				const float c = i / 255.f;
				lin[size_t(i)] = c <= 0.04045f ? c / 12.92f : std::pow(c / 1.055f + 0.055f / 1.055f, 2.4f);
			}
			// CrtsTone(contrast 1, saturation 0, thin, mask): MASK 1 -> mask = 0.5 + mask * 0.5
			const float mask = 0.5f + kMaskDark * 0.5f;
			const float mid_out = 0.18f / ((1.5f - kThin) * (0.5f * mask + 0.5f));
			const float p_mid_in = 0.18f;
			tone_y = (-p_mid_in + mid_out) / ((1.f - p_mid_in) * mid_out);
			tone_z = (-p_mid_in * mid_out + p_mid_in) / (mid_out * (-p_mid_in) + mid_out);
		}
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
			Setup(j.w, j.h, j.dw, j.dh);
		src.Build(j.argb, j.w, j.h, lin.data());
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				V3* o = rows.Row(r);
				for (int c = 0; c < j.dw; c++)
				{
					const int x0 = cx0[size_t(c)];
					const float* wt = &cwt[size_t(c) * 4];
					o[c] = src.Border(x0, r) * wt[0] + src.Border(x0 + 1, r) * wt[1] + src.Border(x0 + 2, r) * wt[2] +
						src.Border(x0 + 3, r) * wt[3];
				}
			}
		});
		const float col_scale = float(j.dw) / j.w;
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				const float* m = &map[size_t(y) * j.dw * 3];
				for (int c = 0; c < j.dw; c++, m += 3)
				{
					const float px = m[0], py = m[1], vin = m[2];
					const float a = py - 0.5f;
					const int ra = int(std::floor(a));
					const float off = a - float(ra);
					const float* sl = &scan_lut[size_t(off * 1024.f + 0.5f) * 2];
					const ColPos cp = ColAt(j.dw, px * col_scale - 0.5f);
					if (!cp.inside || vin <= 0.f)
					{
						o[c] = Pack(0, 0, 0);
						continue;
					}
					V3 v = (At(rows.RowOr0(ra), cp) * sl[0] + At(rows.RowOr0(ra + 1), cp) * sl[1]) * vin;
					// CrtsMask (MASK 1): the column's phosphor dimmed, in thirds of the screen's pixels
					const int mx = (c + 3000000) % 3; // fract((x + 0.5) / 3) < 1/3, < 2/3, else
					if (mx == 0)
						v.r *= kMaskDark;
					else if (mx == 1)
						v.g *= kMaskDark;
					else
						v.b *= kMaskDark;
					// CrtsTone
					const float peak = std::max(1.f / (256.f * 65536.f), Max3(v));
					const float k = (peak / (peak * tone_y + tone_z)) / peak;
					o[c] = Pack(out(v.r * k), out(v.g * k), out(v.b * k));
				}
			}
		});
	}
};

// ===================================================================================================================
// crt-lottes (Timothy Lottes, public domain): hardScan -8, hardPix -3, warpX 0.031, warpY 0.041, maskDark 0.5,
// maskLight 1.5, scaleInLinearGamma 1, shadowMask 3, brightBoost 1, bloom (-1.5, -2, 0.15), shape 2.
// ===================================================================================================================
struct Lottes
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	Src src;
	Rows h3, h5, h7; // per source line: Horz3 / Horz5 / Horz7 at each column's flat position
	std::vector<float> map; // per pixel: source x, source y (texels)
	std::vector<float> wlut; // 1025 x 8: the three scan weights and the five bloom weights by line distance
	std::vector<float> lin;
	OutLut out;
	std::vector<int> cxi; // per column: floor(x)
	std::vector<float> cw; // per column: 3 + 5 + 7 normalised Gaussian weights
	static constexpr float kHardScan = -8.f, kHardPix = -3.f, kWarpX = 0.031f, kWarpY = 0.041f, kDark = 0.5f,
						   kLight = 1.5f, kBloomPix = -1.5f, kBloomScan = -2.f, kBloom = 0.15f;

	static float Gaus(float pos, float scale)
	{
		return std::exp2(scale * pos * pos); // shape 2
	}
	void Setup(int w, int h, int dw, int dh)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh;
		h3.Size(h, dw);
		h5.Size(h, dw);
		h7.Size(h, dw);
		cxi.resize(size_t(dw));
		cw.resize(size_t(dw) * 15);
		for (int c = 0; c < dw; c++)
		{
			const float x = (c + 0.5f) / dw * w;
			const int xi = int(std::floor(x));
			const float dst = -(x - float(xi) - 0.5f); // Dist(pos).x
			cxi[size_t(c)] = xi;
			float* t = &cw[size_t(c) * 15];
			int at = 0;
			for (int taps : {3, 5, 7})
			{
				const float scale = taps == 7 ? kBloomPix : kHardPix;
				float sum = 0.f;
				for (int k = -(taps / 2); k <= taps / 2; k++)
					sum += (t[at + k + taps / 2] = Gaus(dst + float(k), scale)); // texel xi + k: Gaus(dst + k)
				for (int k = 0; k < taps; k++)
					t[at + k] /= sum;
				at += taps;
			}
		}
		map.resize(size_t(dw) * dh * 2);
		for (int y = 0; y < dh; y++)
			for (int x = 0; x < dw; x++)
			{
				float px = (x + 0.5f) / dw * 2.f - 1.f, py = (y + 0.5f) / dh * 2.f - 1.f;
				const float nx = px * (1.f + py * py * kWarpX), ny = py * (1.f + px * px * kWarpY);
				map[(size_t(y) * dw + x) * 2] = (nx * 0.5f + 0.5f) * w;
				map[(size_t(y) * dw + x) * 2 + 1] = (ny * 0.5f + 0.5f) * h;
			}
		if (wlut.empty())
		{
			wlut.resize(1025 * 8);
			for (int i = 0; i <= 1024; i++)
			{
				// dst = -(fract(y) - 0.5), from 0.5 down to -0.5
				const float dst = 0.5f - i / 1024.f;
				float* t = &wlut[size_t(i) * 8];
				t[0] = Gaus(dst - 1.f, kHardScan);
				t[1] = Gaus(dst, kHardScan);
				t[2] = Gaus(dst + 1.f, kHardScan);
				for (int k = 0; k < 5; k++)
					t[3 + k] = Gaus(dst + float(k - 2), kBloomScan);
			}
			lin.resize(256);
			for (int i = 0; i < 256; i++)
				lin[size_t(i)] = SrgbToLinear(i / 255.f);
			out.Build(LinearToSrgb);
		}
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
			Setup(j.w, j.h, j.dw, j.dh);
		src.Build(j.argb, j.w, j.h, lin.data());
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				V3* a = h3.Row(r);
				V3* b = h5.Row(r);
				V3* c7 = h7.Row(r);
				for (int c = 0; c < j.dw; c++)
				{
					const int xi = cxi[size_t(c)];
					const float* t = &cw[size_t(c) * 15];
					V3 t7[7];
					for (int k = 0; k < 7; k++)
						t7[k] = src.Border(xi - 3 + k, r);
					a[c] = t7[2] * t[0] + t7[3] * t[1] + t7[4] * t[2];
					b[c] = t7[1] * t[3] + t7[2] * t[4] + t7[3] * t[5] + t7[4] * t[6] + t7[5] * t[7];
					V3 s7{0.f, 0.f, 0.f};
					for (int k = 0; k < 7; k++)
						s7 = s7 + t7[k] * t[8 + k];
					c7[c] = s7;
				}
			}
		});
		const float col_scale = float(j.dw) / j.w;
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				const float* m = &map[size_t(y) * j.dw * 2];
				const int mask_row = 3 * y + 2;
				for (int c = 0; c < j.dw; c++, m += 2)
				{
					const float px = m[0], py = m[1];
					const int r = int(std::floor(py));
					const float* t = &wlut[size_t((py - float(r)) * 1024.f + 0.5f) * 8];
					const ColPos cp = ColAt(j.dw, px * col_scale - 0.5f);
					if (!cp.inside)
					{
						o[c] = Pack(0, 0, 0);
						continue;
					}
					V3 tri = At(h3.RowOr0(r - 1), cp) * t[0] + At(h5.RowOr0(r), cp) * t[1] + At(h3.RowOr0(r + 1), cp) * t[2];
					const V3 bloom = At(h5.RowOr0(r - 2), cp) * t[3] + At(h7.RowOr0(r - 1), cp) * t[4] +
						At(h7.RowOr0(r), cp) * t[5] + At(h7.RowOr0(r + 1), cp) * t[6] + At(h5.RowOr0(r + 2), cp) * t[7];
					tri = tri + bloom * kBloom;
					// shadowMask 3: fract(((x + 0.5) + 3 (y + 0.5)) / 6) in thirds -> (x + 3y + 2) mod 6, two per colour
					const int k6 = (c + mask_row) % 6;
					if (k6 < 2)
						tri.r *= kLight, tri.g *= kDark, tri.b *= kDark;
					else if (k6 < 4)
						tri.r *= kDark, tri.g *= kLight, tri.b *= kDark;
					else
						tri.r *= kDark, tri.g *= kDark, tri.b *= kLight;
					o[c] = Pack(out(tri.r), out(tri.g), out(tri.b));
				}
			}
		});
	}
};

// ===================================================================================================================
// crt-nobody (Hyllian, MIT), defaults: InputGamma 2.4, OutputGamma 2.2, BrightBoost 1, vignette off, beam width
// 0.80-1.0, scanline size 0.86, horizontal, phosphor layout 1 (magenta/green aperture), mask strength 1, mask
// gamma 2.4, RGB; curvature on: sphere, radius 6, corner 0.04 / 0.5, overscan 100%. Interlaced pictures (289 to 576
// lines) are shown field by field, as the shader does.
// ===================================================================================================================
struct Nobody
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0, key_ty = 0;
	Src src;
	Rows rows; // per virtual line (TextureSize.y of them): c(x) = A wx0 + B wx1 at each column's flat position
	std::vector<float> map; // per pixel: uv.x * TSx, uv.y, corner factor
	std::vector<float> lin;
	OutLut out;
	InLut mask_on; // 1 - (1 - c)^2.4
	static constexpr float kPixSize = 1.111111f, kScanSize = 0.86f, kBeamMin = 0.80f, kBeamMax = 1.f, kRadius = 6.f,
						   kCorner = 0.04f, kCornerSmooth = 0.005f;

	static float Wgt(float s)
	{
		s = std::min(std::max(s, -1.f), 1.f);
		s = 1.f - s * s;
		return s * s * s;
	}
	void Setup(int w, int h, int dw, int dh, int ty)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh, key_ty = ty;
		rows.Size(ty, dw);
		map.resize(size_t(dw) * dh * 3);
		const float r2 = kRadius * kRadius;
		const float aspy = float(dh) / dw;
		const float cornersize = kCorner * std::min(1.f, aspy);
		for (int y = 0; y < dh; y++)
			for (int x = 0; x < dw; x++)
			{
				// vTexCoord = 2 * TexCoord - 1 with curvature on; h_warp, sphere
				const float ux = ((x + 0.5f) / dw) * 2.f - 1.f, uy = ((y + 0.5f) / dh) * 2.f - 1.f;
				const float sphere = kRadius / std::sqrt(std::max(1e-6f, r2 + 1.f - (ux * ux + uy * uy)));
				const float wx = ux * sphere * 0.5f + 0.5f, wy = uy * sphere * 0.5f + 0.5f;
				// h_corner
				const float dxx = std::fabs((2.f * wx - 1.f) * 1.f) - (1.f - cornersize);
				const float dyy = std::fabs((2.f * wy - 1.f) * aspy) - (aspy - cornersize);
				const float mx = std::max(dxx, 0.f), my = std::max(dyy, 0.f);
				const float borderline = std::sqrt(mx * mx + my * my) + std::min(std::max(dxx, dyy), 0.f) - cornersize;
				const float cval = SmoothStep(kCornerSmooth, -kCornerSmooth, borderline);
				float* m = &map[(size_t(y) * dw + x) * 3];
				m[0] = wx * w;
				m[1] = wy;
				m[2] = cval;
			}
		if (lin.empty())
		{
			lin.resize(256);
			for (int i = 0; i < 256; i++)
				lin[size_t(i)] = std::pow(i / 255.f, 2.4f);
			out.Build([](float v) { return std::pow(v, 1.f / 2.2f); });
			mask_on.Build([](float c) { return 1.f - std::pow(1.f - c, 2.4f); });
		}
	}
	void Render(const Job& j)
	{
		const bool interlaced = j.h > 288 && j.h < 577;
		const int ty = interlaced ? j.h / 2 : j.h;
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh || ty != key_ty)
			Setup(j.w, j.h, j.dw, j.dh, ty);
		src.Build(j.argb, j.w, j.h, lin.data());
		const float field = interlaced ? float(j.frame & 1) : 0.f;
		const float cn_off_y = interlaced ? 0.5f + 0.5f * (field - 0.5f) : 0.5f;
		const float scan_off_y = interlaced ? 0.5f * field : 0.f;
		// c(x) on virtual line k: texels A (the one under x) and B (its neighbour on x's side), Wgt weights
		ParallelFor(ty, [&](int k0, int k1) {
			for (int k = k0; k < k1; k++)
			{
				const int sr = int(std::floor((float(k) + cn_off_y) / ty * j.h)); // nearest texel row of tc.y
				V3* o = rows.Row(k);
				for (int c = 0; c < j.dw; c++)
				{
					const float pc = (c + 0.5f) / j.dw * j.w;
					const float fl = std::floor(pc);
					float pos = pc - fl - 0.5f;
					const int dir = pos < 0.f ? -1 : (pos > 0.f ? 1 : 0);
					pos = std::fabs(pos);
					const V3 a = src.Border(int(fl), sr), b = src.Border(int(fl) + dir, sr);
					o[c] = a * Wgt(pos / kPixSize) + b * Wgt((1.f - pos) / kPixSize);
				}
			}
		});
		const float col_scale = float(j.dw) / j.w;
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				const float* m = &map[size_t(y) * j.dw * 3];
				for (int c = 0; c < j.dw; c++, m += 3)
				{
					const float cval = m[2];
					if (cval <= 0.f)
					{
						o[c] = Pack(0, 0, 0);
						continue;
					}
					const float pcy = m[1] * ty - scan_off_y;
					const float fl = std::floor(pcy);
					float pos = pcy - fl - cn_off_y;
					const int dir = pos < 0.f ? -1 : (pos > 0.f ? 1 : 0);
					pos = std::fabs(pos);
					const int k = int(fl);
					const ColPos cp = ColAt(j.dw, m[0] * col_scale - 0.5f);
					const V3 c0 = At(rows.RowOr0(k), cp), c1 = At(rows.RowOr0(k + dir), cp);
					const float ssy0 = kScanSize * (kBeamMin + (kBeamMax - kBeamMin) * Max3(c0));
					const float ssy1 = kScanSize * (kBeamMin + (kBeamMax - kBeamMin) * Max3(c1));
					V3 v = c0 * Wgt(pos / ssy0) + c1 * Wgt((1.f - pos) / ssy1);
					// mask layout 1: magenta / green columns; mask gamma: lit channels 1 - (1 - c)^2.4, others kept
					const int mcol = int(std::floor(float(c) + 0.5f - j.dw * 0.5f)); // mask_coords.x
					v.r = Sat(v.r), v.g = Sat(v.g), v.b = Sat(v.b);
					if ((mcol & 1) == 0)
						v.r = mask_on(v.r), v.b = mask_on(v.b);
					else
						v.g = mask_on(v.g);
					o[c] = Pack(out(v.r * cval), out(v.g * cval), out(v.b * cval));
				}
			}
		});
	}
};

// ===================================================================================================================
// newpixie-mini (Mattias Gustavsson, Unlicense): curvature 2, vignette 1; bilinear source.
// ===================================================================================================================
struct NewpixieMini
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	Src src;
	std::vector<float> map; // per pixel: scuv.x, scuv.y (texture coordinates), vignette x scanline
	InLut gin; // pow(c, 2.2) * 1.25
	static constexpr float kCurv = 2.f, kVignette = 1.f;

	static void Curve(float& x, float& y)
	{
		x -= 0.5f, y -= 0.5f;
		x *= 0.925f, y *= 1.095f;
		x *= kCurv, y *= kCurv;
		x *= 1.f + (std::fabs(y) / 4.f) * (std::fabs(y) / 4.f);
		y *= 1.f + (std::fabs(x) / 3.f) * (std::fabs(x) / 3.f);
		x /= kCurv, y /= kCurv;
		x += 0.5f, y += 0.5f;
		x = x * 0.92f + 0.04f, y = y * 0.92f + 0.04f;
	}
	void Setup(int w, int h, int dw, int dh)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh;
		map.resize(size_t(dw) * dh * 3);
		for (int y = 0; y < dh; y++)
			for (int x = 0; x < dw; x++)
			{
				const float u = (x + 0.5f) / dw, v = (y + 0.5f) / dh;
				float cx = u, cy = v;
				Curve(cx, cy);
				cx = cx + (u - cx) * 0.4f; // mix(curve(uv), uv, 0.4)
				cy = cy + (v - cy) * 0.4f;
				const float scale = -0.101f;
				const float sx = cx * (1.f - scale) + scale / 2.f + 0.003f, sy = cy * (1.f - scale) + scale / 2.f - 0.001f;
				float vig = (1.f - 0.99f * kVignette) + 4.f * cx * cy * (1.f - cx) * (1.f - cy);
				vig = 1.3f * std::sqrt(std::max(0.f, vig));
				const float scans = Sat(0.35f + 0.18f * std::sin(cy * dh * 1.5f));
				float* m = &map[(size_t(y) * dw + x) * 3];
				m[0] = sx;
				m[1] = sy;
				m[2] = vig * std::pow(scans, 0.9f);
			}
		if (gin.t.empty())
			gin.Build([](float c) { return std::pow(c, 2.2f) * 1.25f; });
	}
	std::vector<float> plane[3]; // R, G, B planes
	float Tex(const float* p, int x, int y) const
	{
		return (unsigned(x) < unsigned(key_w) && unsigned(y) < unsigned(key_h)) ? p[size_t(y) * key_w + x] : 0.f;
	}
	// one channel, bilinear (black outside), at tsample's texture coordinates
	float Sample(float tx, float ty, int ch) const
	{
		const float x = (tx * 1.025f - 0.0125f) * key_w - 0.5f, y = (ty * 0.92f + 0.04f) * key_h - 0.5f;
		const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
		const float fx = x - x0, fy = y - y0;
		const float* p = plane[ch].data();
		float a, b, c, d;
		if (x0 >= 0 && y0 >= 0 && x0 + 1 < key_w && y0 + 1 < key_h)
		{
			const float* q = p + size_t(y0) * key_w + x0;
			a = q[0], b = q[1], c = q[key_w], d = q[key_w + 1];
		}
		else
			a = Tex(p, x0, y0), b = Tex(p, x0 + 1, y0), c = Tex(p, x0, y0 + 1), d = Tex(p, x0 + 1, y0 + 1);
		const float top = a + (b - a) * fx, bot = c + (d - c) * fx;
		return gin(top + (bot - top) * fy);
	}
	static float Filmic(float c)
	{
		const float x = std::max(0.f, c - 0.004f);
		return (x * (6.2f * x + 0.5f)) / (x * (6.2f * x + 1.7f) + 0.06f);
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
			Setup(j.w, j.h, j.dw, j.dh);
		for (auto& p : plane)
			p.resize(size_t(j.w) * j.h);
		for (size_t i = 0; i < size_t(j.w) * j.h; i++)
		{
			const uint32_t c = j.argb[i];
			plane[0][i] = ((c >> 16) & 255) / 255.f;
			plane[1][i] = ((c >> 8) & 255) / 255.f;
			plane[2][i] = (c & 255) / 255.f;
		}
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const float o_ = std::sin((y + 0.5f) * 1.5f) / j.dw * 0.25f; // the beam's horizontal wobble
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				const float* m = &map[size_t(y) * j.dw * 3];
				for (int c = 0; c < j.dw; c++, m += 3)
				{
					const float sx = m[0] + o_, sy = m[1];
					V3 col{Sample(sx + 0.0009f, sy + 0.0009f, 0) + 0.02f, Sample(sx, sy - 0.0011f, 1) + 0.02f,
						Sample(sx - 0.0015f, sy, 2) + 0.02f};
					auto curves = [](float x) {
						const float x2 = x * x;
						return std::min(std::max(x + x2 + x2 * x2 * x, 0.f), 10.f);
					};
					col = V3{curves(col.r), curves(col.g), curves(col.b)} * m[2];
					// vertical lines: 1 - 0.23 * clamp(mod(x, 3) / 2, 0, 1)
					static const float kLines[3] = {1.f - 0.23f * 0.25f, 1.f - 0.23f * 0.75f, 1.f - 0.23f};
					col = col * kLines[(j.dx + c) % 3]; // 1 - 0.23 clamp(mod(x + 0.5, 3) / 2, 0, 1)
					o[c] = Pack(To8(Filmic(col.r)), To8(Filmic(col.g)), To8(Filmic(col.b)));
				}
			}
		});
	}
};

// One of each, built on first use.
Easymode& TheEasymode()
{
	static Easymode s;
	return s;
}
} // namespace

const char* Name(Shader s)
{
	switch (s)
	{
		case Shader::Off: return "Off";
		case Shader::EasymodeStyle: return "CRT Easymode style";
		case Shader::Lottes: return "crt-lottes";
		case Shader::LottesFast: return "crt-lottes-fast";
		case Shader::OneTap: return "crt-1tap";
		case Shader::TwoTap: return "crt-2tap";
		case Shader::HyllianFast: return "crt-hyllian-fast";
		case Shader::Nobody: return "crt-nobody";
		case Shader::NewpixieMini: return "newpixie-mini";
		case Shader::BlurPiSharp: return "crt-blurPi-sharp";
		case Shader::BlurPiSoft: return "crt-blurPi-soft";
		case Shader::MonoCrt: return "monoCRT";
		default: return "?";
	}
}

void Render(Shader s, const uint32_t* argb, int w, int h, uint32_t* surface, int pitch, int dx, int dy, int dw, int dh,
	uint64_t frame)
{
	if (!argb || !surface || w <= 0 || h <= 0 || dw <= 0 || dh <= 0)
		return;
	const Job j{argb, w, h, surface + size_t(dy) * pitch + dx, pitch, dx, dw, dh, frame};
	switch (s)
	{
		case Shader::EasymodeStyle: TheEasymode().Render(j); break;
		case Shader::Lottes:
		{
			static Lottes x;
			x.Render(j);
			break;
		}
		case Shader::LottesFast:
		{
			static LottesFast x;
			x.Render(j);
			break;
		}
		case Shader::OneTap:
		{
			static Ntap x(false);
			x.Render(j);
			break;
		}
		case Shader::TwoTap:
		{
			static Ntap x(true);
			x.Render(j);
			break;
		}
		case Shader::HyllianFast:
		{
			static HyllianFast x;
			x.Render(j);
			break;
		}
		case Shader::Nobody:
		{
			static Nobody x;
			x.Render(j);
			break;
		}
		case Shader::NewpixieMini:
		{
			static NewpixieMini x;
			x.Render(j);
			break;
		}
		case Shader::BlurPiSharp:
		{
			static BlurPi x(false);
			x.Render(j);
			break;
		}
		case Shader::BlurPiSoft:
		{
			static BlurPi x(true);
			x.Render(j);
			break;
		}
		case Shader::MonoCrt:
		{
			static MonoCrt x;
			x.Render(j);
			break;
		}
		default: break;
	}
}
} // namespace ps5crt

// Border pack tests: PNG decode, finding the window, the backdrop and the rim, the lookup order and the Select / Reselect /
// Release calls (frontend/fe_border.cpp, fe_borderimg.cpp, fe_borderfetch.cpp). Ported from Snes9x PS5 2.7's test (pull
// request #3 by thatboialex), with the Sega systems' folders and the picture's own shape. The renderer, the cover matcher and the game naming are
// stand-ins below: the real ones are covered by the host groups in run-tests.sh. Pictures are generated PNGs.
// SPDX-License-Identifier: MIT

#include "fe_border.h"
#include "fe_borderfetch.h"
#include "fe_borderimg.h"
#include "fe_covers.h"
#include "fe_games.h"
#include "fe_settings.h"

#include "OrbisPaths.h"
#include "ProsperoVideo.h"

#include <zlib.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

// ---- stand-ins for what the border code calls ----
// BigThread arms the crash handler per thread; the host test has none.
namespace crashlog
{
void* ArmThread() { return nullptr; }
void DisarmThread(void*) {}
} // namespace crashlog

// g_shown is set before g_set_calls goes up, so a test that has seen the count may read the border (the download watcher sets
// it from its own thread).
namespace ps5video
{
std::shared_ptr<const Border> g_shown;
std::atomic<int> g_set_calls{0};
void SetBorder(std::shared_ptr<const Border> border)
{
	g_shown = std::move(border);
	g_set_calls++;
}
} // namespace ps5video

namespace fe
{
// The systems' table (fe_games.cpp): the ids and folders the pack uses.
const SystemInfo& Info(System s)
{
	static const SystemInfo systems[] = {
		{"md", "Mega Drive", "Sega_-_Mega_Drive_-_Genesis", Family::Md, "MegaDrive"},
		{"scd", "Sega CD", "Sega_-_Mega-CD_-_Sega_CD", Family::SegaCd, "SegaCD"},
		{"sms", "Master System", "Sega_-_Master_System_-_Mark_III", Family::Sms, "MasterSystem"},
		{"gg", "Game Gear", "Sega_-_Game_Gear", Family::Gg, "GameGear"},
		{"sg", "SG-1000", "Sega_-_SG-1000", Family::Sms, "SG1000"},
	};
	return systems[size_t(s) < 5 ? size_t(s) : 0];
}

// The real matcher is MatchArtName over the pack's names (fe_covers.cpp); here a name is matched when it is the game's
// No-Intro name, which is the one rule these tests need.
std::string MatchPackName(const std::vector<std::string>& names, const GameInfo& game)
{
	return std::find(names.begin(), names.end(), game.nointro) != names.end() ? game.nointro : std::string();
}

// fe_coverfetch.cpp follows git symlinks with it; nothing here downloads.
std::string UrlEncode(const std::string& s)
{
	return s;
}

// The index of The Bezel Project's names: a game is matched by its No-Intro name, or by the name its CRC has in the extra
// table when that is one of them (the real matcher is covered by test_titlematch and the host groups).
struct PackMatcher::Impl
{
	std::vector<std::string> names;
};
PackMatcher::PackMatcher(const std::vector<std::string>& names, bool) : m_i(new Impl)
{
	m_i->names = names;
}
PackMatcher::~PackMatcher() = default;
std::string PackMatcher::Match(const GameInfo& game) const
{
	const auto& n = m_i->names;
	if (std::find(n.begin(), n.end(), game.nointro) != n.end())
		return game.nointro;
	return !game.crc_name.empty() && std::find(n.begin(), n.end(), game.crc_name) != n.end() ? game.crc_name : std::string();
}

GameInfo DescribeGame(const std::string& path)
{
	GameInfo g;
	g.path = path;
	const std::string name = path.substr(path.find_last_of('/') + 1);
	g.file_base = name.substr(0, name.find_last_of('.'));
	g.system = name.size() > 3 && name.compare(name.size() - 3, 3, ".gg") == 0 ? System::Gg : System::Md;
	// "Mapped Name.md" is the ROM of the game the pack calls "Mapped Name (USA)"
	g.nointro = g.file_base.rfind("Mapped", 0) == 0 ? g.file_base + " (USA)" : g.file_base;
	// "Loose Name.md" is a translation of Sonic The Hedgehog 2: the CRC table knows it by the index's name
	if (g.file_base.rfind("Loose", 0) == 0)
		g.crc_name = "Sonic The Hedgehog 2 (World)";
	return g;
}
} // namespace fe

namespace
{
int g_failed = 0;
int g_checked = 0;
#define CHECK(cond, what)                                                                          \
	do                                                                                             \
	{                                                                                              \
		g_checked++;                                                                               \
		if (!(cond))                                                                               \
		{                                                                                          \
			g_failed++;                                                                            \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what);                                  \
		}                                                                                          \
	} while (0)

using ps5video::Rect;
using fe::border::Image;

// ---- a PNG writer: just enough for the tests (filter 0, one IDAT) ----
struct Px
{
	uint8_t r, g, b, a;
};

void Be32(std::vector<uint8_t>& o, uint32_t v)
{
	for (int s = 24; s >= 0; s -= 8)
		o.push_back(uint8_t(v >> s));
}

void Chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data)
{
	Be32(o, uint32_t(data.size()));
	std::vector<uint8_t> body(type, type + 4);
	body.insert(body.end(), data.begin(), data.end());
	o.insert(o.end(), body.begin(), body.end());
	Be32(o, uint32_t(crc32(0, body.data(), uInt(body.size()))));
}

std::vector<uint8_t> Png(int w, int h, int depth, int ctype, const std::vector<uint8_t>& scan, const std::vector<uint8_t>& plte = {},
	const std::vector<uint8_t>& trns = {})
{
	std::vector<uint8_t> o = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	std::vector<uint8_t> ihdr;
	Be32(ihdr, uint32_t(w));
	Be32(ihdr, uint32_t(h));
	ihdr.insert(ihdr.end(), {uint8_t(depth), uint8_t(ctype), 0, 0, 0});
	Chunk(o, "IHDR", ihdr);
	if (!plte.empty())
		Chunk(o, "PLTE", plte);
	if (!trns.empty())
		Chunk(o, "tRNS", trns);
	uLongf n = compressBound(uLong(scan.size()));
	std::vector<uint8_t> z(n);
	compress2(z.data(), &n, scan.data(), uLong(scan.size()), 1);
	z.resize(n);
	Chunk(o, "IDAT", z);
	Chunk(o, "IEND", {});
	return o;
}

template <class F> std::vector<uint8_t> PngRgba(int w, int h, F px, int depth = 8)
{
	std::vector<uint8_t> scan;
	for (int y = 0; y < h; y++)
	{
		scan.push_back(0);
		for (int x = 0; x < w; x++)
		{
			const Px p = px(x, y);
			for (uint8_t c : {p.r, p.g, p.b, p.a})
			{
				scan.push_back(c);
				if (depth == 16)
					scan.push_back(c);
			}
		}
	}
	return Png(w, h, depth, 6, scan);
}

template <class F> std::vector<uint8_t> PngRgb(int w, int h, F px)
{
	std::vector<uint8_t> scan;
	for (int y = 0; y < h; y++)
	{
		scan.push_back(0);
		for (int x = 0; x < w; x++)
		{
			const Px p = px(x, y);
			scan.insert(scan.end(), {p.r, p.g, p.b});
		}
	}
	return Png(w, h, 8, 2, scan);
}

// ---- the art: a frame with a clear window, a soft 2 pixel edge inside it, a soft ring outside it, one opaque corner ----
const Rect kWinA = {261, 21, 1398, 1038};
const Rect kWinB = {200, 40, 1500, 1000};
const Rect kWinC = {300, 60, 1300, 960};
const Rect kWinD = {100, 100, 1000, 700};

Px Art(const Rect& win, int x, int y)
{
	const bool in = x >= win.x && x < win.x + win.w && y >= win.y && y < win.y + win.h;
	if (in)
	{
		if (x == win.x && y == win.y)
			return {200, 100, 50, 255}; // a rounded corner of the glass, opaque
		const int d = std::min(std::min(x - win.x, win.x + win.w - 1 - x), std::min(y - win.y, win.y + win.h - 1 - y));
		if (d < 2)
			return {255, 255, 255, 10}; // the soft edge: counts as clear, still drawn over the picture
		return {0, 0, 0, 0};
	}
	const int dx = std::max(std::max(win.x - x, x - (win.x + win.w - 1)), 0);
	const int dy = std::max(std::max(win.y - y, y - (win.y + win.h - 1)), 0);
	if (std::max(dx, dy) <= 3)
		return {20, 30, 40, 128}; // the frame's own soft edge, outside the window
	return {200, 40, 40, 255};
}

std::vector<uint8_t> ArtPng(const Rect& win)
{
	return PngRgba(1920, 1080, [&](int x, int y) { return Art(win, x, y); });
}

// ---- files ----
std::string g_root;

void WriteBytes(const std::string& path, const std::vector<uint8_t>& b)
{
	FILE* f = fopen(path.c_str(), "wb");
	if (f)
	{
		fwrite(b.data(), 1, b.size(), f);
		fclose(f);
	}
}

void WriteText(const std::string& path, const std::string& text)
{
	WriteBytes(path, std::vector<uint8_t>(text.begin(), text.end()));
}

void Remove(const std::string& path)
{
	unlink(path.c_str());
}

bool SameRect(const Rect& a, const Rect& b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

fe::GameInfo Game(const char* file_base, const char* nointro = "", fe::System system = fe::System::Md)
{
	fe::GameInfo g;
	g.file_base = file_base;
	g.nointro = nointro;
	g.system = system;
	return g;
}

// The shape of a picture (fe_border.h): 4:3 Fit by default.
fe::border::Shape ShapeOf(double aspect, int base_h = 224, ps5video::Scale scale = ps5video::Scale::Fit)
{
	fe::border::Shape s;
	s.valid = true;
	s.aspect = aspect;
	s.base_h = base_h;
	s.scale = scale;
	return s;
}

// ---- the tests ----
void TestDecode()
{
	using fe::border::DecodePng;
	Image img;
	std::string why;

	const std::vector<uint8_t> rgba = ArtPng(kWinA);
	CHECK(DecodePng(rgba.data(), rgba.size(), &img, &why), "an RGBA 1920x1080 PNG decodes");
	CHECK(img.w == 1920 && img.h == 1080 && img.px.size() == 1920u * 1080u, "decoded size");
	CHECK(img.px[0] == 0xff2828c8u, "pixel (0,0) is 0xAABBGGRR of the frame colour (200, 40, 40)");
	CHECK(img.px[size_t(540) * 1920 + 960] == 0u, "the centre is clear (alpha 0)");

	const std::vector<uint8_t> rgb = PngRgb(1920, 1080, [](int, int) { return Px{10, 20, 30, 0}; });
	CHECK(DecodePng(rgb.data(), rgb.size(), &img, &why), "an RGB PNG decodes");
	CHECK(img.px[12345] == (0xff000000u | (30u << 16) | (20u << 8) | 10u), "RGB gets alpha 255");

	// palette + tRNS, as The Bezel Project's files are: index 0 clear, 1 the frame, 2 half-clear white
	std::vector<uint8_t> scan;
	for (int y = 0; y < 1080; y++)
	{
		scan.push_back(0);
		for (int x = 0; x < 1920; x++)
			scan.push_back(x >= 261 && x < 1659 && y >= 21 && y < 1059 ? (x == 261 ? 2 : 0) : 1);
	}
	const std::vector<uint8_t> pal =
		Png(1920, 1080, 8, 3, scan, {0, 0, 0, 200, 40, 40, 255, 255, 255}, {0, 255, 128});
	CHECK(DecodePng(pal.data(), pal.size(), &img, &why), "a palette PNG with tRNS decodes");
	CHECK((img.px[size_t(540) * 1920 + 960] >> 24) == 0, "palette index 0 is clear through tRNS");
	CHECK((img.px[5] >> 24) == 255, "palette index 1 is opaque");
	CHECK((img.px[size_t(540) * 1920 + 261] >> 24) == 128, "palette index 2 has alpha 128");

	const std::vector<uint8_t> small = PngRgba(100, 100, [](int, int) { return Px{0, 0, 0, 0}; });
	CHECK(!DecodePng(small.data(), small.size(), &img, &why) && why.find("100 x 100") != std::string::npos,
		"a PNG that is not 1920x1080 is refused, naming its size");

	const std::vector<uint8_t> deep = PngRgba(1920, 1080, [](int, int) { return Px{0, 0, 0, 0}; }, 16);
	CHECK(!DecodePng(deep.data(), deep.size(), &img, &why) && why.find("16 bits") != std::string::npos, "16 bits per channel is refused");

	const std::vector<uint8_t> junk(300, 0x41);
	CHECK(!DecodePng(junk.data(), junk.size(), &img, &why) && why.find("not a PNG") != std::string::npos, "bytes that are no PNG are refused");
	CHECK(!DecodePng(junk.data(), 0, &img, &why) && why.find("empty") != std::string::npos, "an empty file is refused");

	std::vector<uint8_t> cut(rgba.begin(), rgba.begin() + long(rgba.size() / 2));
	CHECK(!DecodePng(cut.data(), cut.size(), &img, &why), "a truncated PNG is refused");
	CHECK(why.find("damaged") != std::string::npos, "and called damaged");

	std::vector<uint8_t> big(fe::border::kMaxFileBytes + 1, 0);
	CHECK(!DecodePng(big.data(), big.size(), &img, &why) && why.find("8 MiB") != std::string::npos, "a file over 8 MiB is refused");
}

void TestWindow()
{
	using fe::border::FindWindow;
	Image img;
	std::string why;
	Rect win = {};

	const std::vector<uint8_t> a = ArtPng(kWinA);
	fe::border::DecodePng(a.data(), a.size(), &img, &why);
	CHECK(FindWindow(img, &win, &why) && SameRect(win, kWinA), "the window is the bounding box of the clear region (261,21 1398x1038)");

	// a clear island far away and a clear diagonal that touches the window only at a corner are not part of it
	Image tricky = img;
	for (int y = 600; y < 610; y++)
		for (int x = 40; x < 50; x++)
			tricky.px[size_t(y) * 1920 + x] = 0;
	for (int k = 0; k < 10; k++)
		tricky.px[size_t(kWinA.y - k) * 1920 + size_t(kWinA.x - 1 - k)] = 0;
	CHECK(FindWindow(tricky, &win, &why) && SameRect(win, kWinA), "a clear island, and a diagonal-only link, do not widen the window");

	Image opaque = img;
	for (int y = 530; y < 550; y++)
		for (int x = 950; x < 970; x++)
			opaque.px[size_t(y) * 1920 + x] = 0xff000000u;
	CHECK(!FindWindow(opaque, &win, &why) && why.find("centre") != std::string::npos, "an opaque centre is refused");

	const std::vector<uint8_t> tiny = ArtPng({900, 500, 300, 200});
	Image t;
	fe::border::DecodePng(tiny.data(), tiny.size(), &t, &why);
	CHECK(!FindWindow(t, &win, &why) && why.find("too small") != std::string::npos, "a window under 480x270 is refused");

	Image wrong;
	wrong.w = 100;
	wrong.h = 100;
	CHECK(!FindWindow(wrong, &win, &why), "an image of the wrong size is refused");

	CHECK(fe::border::WindowFits({0, 0, 1920, 1080}), "a window may be the whole surface");
	CHECK(fe::border::WindowFits({100, 50, 480, 270}), "480x270 is the smallest given window");
	CHECK(!fe::border::WindowFits({100, 50, 479, 270}), "a given window under 480 wide is refused");
	CHECK(!fe::border::WindowFits({100, 50, 480, 269}), "a given window under 270 high is refused");
	CHECK(!fe::border::WindowFits({1700, 0, 400, 400}), "a window past the right edge is refused");
	CHECK(!fe::border::WindowFits({0, 700, 400, 400}), "a window past the bottom edge is refused");
	CHECK(!fe::border::WindowFits({-1, 0, 400, 400}), "a window left of the surface is refused");
}

void TestBuild()
{
	using fe::border::BuildBorder;
	Image img;
	std::string why;
	const std::vector<uint8_t> a = ArtPng(kWinA);
	fe::border::DecodePng(a.data(), a.size(), &img, &why);
	const auto b = BuildBorder(img, kWinA, &why);
	CHECK(b != nullptr, "a border is built");
	if (!b)
		return;
	CHECK(SameRect(b->window, kWinA), "the border keeps its window");
	CHECK(b->backdrop.size() == 1920u * 1080u, "the backdrop is the whole surface");
	CHECK(b->backdrop[0] == 0xff2828c8u, "an opaque frame pixel is itself, alpha 255");
	CHECK(b->backdrop[size_t(540) * 1920 + 960] == 0xff000000u, "a clear window pixel is black");
	// the frame's soft ring at (260, 500): colour (20,30,40) at alpha 128 on black
	CHECK(b->backdrop[size_t(500) * 1920 + 260] == 0xff140f0au, "a half-clear pixel outside the window is composited on black");
	// the soft edge inside the window: white at alpha 10 -> 10 on black, and it is a rim pixel
	const uint32_t edge = uint32_t(500) * 1920 + 262; // two in from the window's left edge: d = 1
	CHECK(b->backdrop[edge - 1] == 0xff0a0a0au, "a soft edge pixel inside the window is on black in the backdrop");

	size_t expected = 0;
	for (int y = kWinA.y; y < kWinA.y + kWinA.h; y++)
		for (int x = kWinA.x; x < kWinA.x + kWinA.w; x++)
			expected += Art(kWinA, x, y).a != 0;
	CHECK(b->rim.size() == expected && expected > 0, "the rim holds every window pixel with alpha 1..255");
	CHECK(std::is_sorted(b->rim.begin(), b->rim.end(), [](const ps5video::RimPixel& p, const ps5video::RimPixel& q) { return p.index < q.index; }),
		"the rim is sorted by index");
	const auto find = [&](uint32_t index) { return std::find_if(b->rim.begin(), b->rim.end(), [&](const ps5video::RimPixel& p) { return p.index == index; }); };
	const auto soft = find(edge - 1);
	CHECK(soft != b->rim.end() && soft->pre == 0x000a0a0au && soft->inv == 245u, "a soft edge pixel: colour times alpha, and 255 - alpha");
	const auto corner = find(uint32_t(kWinA.y) * 1920 + uint32_t(kWinA.x));
	CHECK(corner != b->rim.end() && corner->pre == 0x003264c8u && corner->inv == 0u, "an opaque corner of the glass is in the rim, inv 0");
	CHECK(find(uint32_t(540) * 1920 + 960) == b->rim.end(), "a clear pixel is not in the rim");
	CHECK(find(uint32_t(500) * 1920 + 260) == b->rim.end(), "a pixel outside the window is not in the rim");

	// art that covers most of the window is refused: it would cost frame time
	Image veil = img;
	for (uint32_t& p : veil.px)
		if ((p >> 24) == 0)
			p = 0x20000000u;
	CHECK(BuildBorder(veil, kWinA, &why) == nullptr && why.find("covered") != std::string::npos, "a window covered by art is refused");

	Image bad;
	bad.w = 10;
	bad.h = 10;
	CHECK(BuildBorder(bad, kWinA, &why) == nullptr, "an image of the wrong size builds nothing");
}

// ---- the window follows the aspect ratio ----
// The art for the fit tests: opaque margins that count their own pixels (red = x / 8, green = y / 5), the window of kWinA with
// kWinA's soft edge and one opaque corner, as Art() draws them.
Image GradientArt()
{
	Image img;
	img.w = 1920;
	img.h = 1080;
	img.px.resize(size_t(1920) * 1080);
	for (int y = 0; y < 1080; y++)
		for (int x = 0; x < 1920; x++)
		{
			Px p = Art(kWinA, x, y);
			if (p.a == 255 && !(x == kWinA.x && y == kWinA.y))
				p = {uint8_t(x / 8), uint8_t(y / 5), 90, 255};
			img.px[size_t(y) * 1920 + x] = uint32_t(p.a) << 24 | uint32_t(p.b) << 16 | uint32_t(p.g) << 8 | p.r;
		}
	return img;
}

bool Shaped(const Rect& w, int rw, int rh) // the window has the shape rw:rh, to a pixel of rounding
{
	return std::abs(w.w * rh - w.h * rw) <= std::max(rw, rh) * 2;
}

void TestFit()
{
	using namespace fe::border;
	std::string why;

	const double r43 = 4.0 / 3.0, r87 = 8.0 / 7.0, r169 = 16.0 / 9.0;
	// where the window goes
	CHECK(SameRect(FitOpening(kWinA, r43), kWinA), "a window within 3% of 4:3 stays in 4:3 (1398x1038 is 1.347)");
	const Rect w87 = FitOpening(kWinA, r87);
	CHECK(SameRect(w87, Rect{367, 21, 1186, 1038}), "8:7: the height stays, the width follows (1186), the side margins grow in proportion");
	const Rect w169 = FitOpening(kWinA, r169);
	CHECK(SameRect(w169, Rect{104, 59, 1712, 963}), "16:9: the side margins shrink to 40% (261 -> 104), the height gives way (963)");
	CHECK(w169.x * 2 + w169.w == 1920 && std::abs(w169.y * 2 + w169.h - 1080) <= 1, "and the window of a centred frame stays centred");
	const Rect wb = FitOpening(kWinB, r43);
	CHECK(Shaped(wb, 4, 3) && wb.h == kWinB.h && (wb.w & 1) == 0, "a 3:2 window becomes 4:3 at the same height, even width");
	CHECK(SameRect(FitOpening({0, 0, 1920, 1080}, r43), Rect{240, 0, 1440, 1080}), "a window that fills the screen is centred when it narrows");
	CHECK(SameRect(FitOpening({0, 0, 1920, 1080}, r169), Rect{0, 0, 1920, 1080}), "and stays when it already has the shape");
	// the Game Gear's picture (160x144 at 6:5 pixels: 4:3) and the Master System's square pixels (256x192: 4:3) take the 4:3 window;
	// the Mega Drive's square pixels (320x224, 10:7) a wider one
	CHECK(SameRect(FitOpening(kWinA, 160.0 * 6 / 5 / 144), kWinA) && SameRect(FitOpening(kWinA, 256.0 / 192), kWinA), "a 4:3 Game Gear or Master System picture keeps a 4:3 window");
	CHECK(Shaped(FitOpening(kWinA, 320.0 / 224), 10, 7), "a 320x224 square-pixel picture gets a 10:7 window");
	CHECK(SameRect(FitOpening(kWinA, 0), kWinA) && SameRect(FitOpening(kWinA, 100), kWinA), "a ratio that makes no sense keeps the window");
	// Integer scale: the opening is the whole-multiple picture: 4x of a 224-line 4:3 picture is 1194x896
	CHECK(SameRect(IntegerOpening(kWinA, 224, r43), Rect{363, 92, 1194, 896}), "Integer scale: 1398x1038 becomes 1194x896 (4x of 224 lines at 4:3), centred");
	CHECK(SameRect(IntegerOpening(Rect{363, 92, 1194, 896}, 224, r43), Rect{363, 92, 1194, 896}), "a window that is 4x already stays");
	CHECK(SameRect(IntegerOpening(kWinA, 144, r43), Rect{288, 36, 1344, 1008}), "a 144-line Game Gear picture: 7x, 1344x1008");
	CHECK(SameRect(IntegerOpening(kWinA, 192, r43), Rect{320, 60, 1280, 960}), "a 192-line Master System picture: 5x, 1280x960");
	CHECK(SameRect(IntegerOpening(Rect{480, 270, 480, 270}, 224, r43), Rect{541, 285, 298, 224}), "a window that only fits 1x gets 298x224");
	CHECK(SameRect(IntegerOpening(Rect{480, 270, 300, 200}, 224, r43), Rect{480, 270, 300, 200}), "a window too small for 1x stays");
	// every window and shape ends up inside the surface, even, in shape (or untouched), and with margins that keep their proportions
	bool sane = true;
	for (const Rect& win : {kWinA, kWinB, kWinC, kWinD, Rect{0, 0, 1920, 1080}, Rect{480, 270, 960, 540}, Rect{100, 20, 1700, 960}})
		for (const std::pair<int, int>& ratio : {std::make_pair(4, 3), std::make_pair(8, 7), std::make_pair(16, 9), std::make_pair(10, 7)})
		{
			const Rect r = FitOpening(win, double(ratio.first) / ratio.second);
			const bool same = SameRect(r, win);
			const bool inside = r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x + r.w <= 1920 && r.y + r.h <= 1080;
			const int mw = 1920 - win.w, mh = 1080 - win.h;
			const bool kept = same || ((mw == 0 || std::abs(r.x * mw - win.x * (1920 - r.w)) <= mw) && (mh == 0 || std::abs(r.y * mh - win.y * (1080 - r.h)) <= mh));
			sane = sane && inside && (same || ((r.w & 1) == 0 && Shaped(r, ratio.first, ratio.second) && kept));
		}
	CHECK(sane, "FitOpening stays inside the surface, in shape, with the margins in proportion, for a range of windows");
	sane = true;
	for (const Rect& win : {kWinA, kWinB, kWinC, kWinD, Rect{0, 0, 1920, 1080}, Rect{480, 270, 960, 540}, Rect{100, 20, 1700, 960}, Rect{363, 92, 1194, 896}})
		for (const int lines : {144, 192, 224, 240})
		{
			const Rect r = IntegerOpening(win, lines, r43);
			const int k = r.h / lines;
			const int mw = 1920 - win.w, mh = 1080 - win.h;
			const bool inside = r.x >= 0 && r.y >= 0 && r.x + r.w <= 1920 && r.y + r.h <= 1080 && r.w <= win.w && r.h <= win.h;
			const bool kept = (mw == 0 || std::abs(r.x * mw - win.x * (1920 - r.w)) <= mw) && (mh == 0 || std::abs(r.y * mh - win.y * (1080 - r.h)) <= mh);
			sane = sane && inside && kept && k >= 1 && r.h == lines * k && r.w == (int(lines * k * r43 + 0.5) & ~1);
		}
	CHECK(sane, "IntegerOpening is a whole multiple of the picture inside the drawn window, with the margins in proportion, for a range of windows");

	// the art
	Image src = GradientArt();
	Image img = src;
	Rect win = kWinA;
	CHECK(FitToShape(&img, &win, Shape(), &why) && SameRect(win, kWinA) && img.px == src.px, "no shape: nothing is touched");
	CHECK(FitToShape(&img, &win, ShapeOf(r43), &why) && SameRect(win, kWinA) && img.px == src.px, "4:3 on a 4:3 window: nothing is touched");

	// 8:7: x changes (the side margins grow), y does not
	win = kWinA;
	CHECK(FitToShape(&img, &win, ShapeOf(r87), &why) && SameRect(win, w87), "8:7: the window is the new opening");
	Rect found = {};
	CHECK(FindWindow(img, &found, &why) && SameRect(found, w87), "and it is the clear hole the window search finds in the stretched art");
	const auto px = [&](int x, int y) { return img.px[size_t(y) * 1920 + x]; };
	const auto src_px = [&](int x, int y) { return src.px[size_t(y) * 1920 + x]; };
	CHECK(px(0, 0) == src_px(0, 0) && px(1919, 1079) == src_px(1919, 1079), "the panels keep their anchor corners: the left one from the top, the right one from the bottom");
	CHECK(px(w87.x - 1, 300) == src_px(kWinA.x - 1, 300), "the last column left of the window is the art's last column left of its window (no blend across the cut)");
	CHECK(px(w87.x + w87.w, 300) == src_px(kWinA.x + kWinA.w, 300), "and the first column right of the window likewise");
	// the left panel (237 px of art, 343 px in the result) is scaled up evenly: x = 200, y = 300 show the art at (138.0, 207.1)
	// (red counts x / 8, green y / 5); a panel squeezed to the height would show y = 300
	CHECK(std::abs(int(px(200, 300) & 0xff) - 17) <= 1 && std::abs(int((px(200, 300) >> 8) & 0xff) - 41) <= 1,
		"the side panel is scaled up evenly and cropped to the height, not stretched sideways");
	bool rim_same = true;
	for (int y = 0; y < 1080; y++)
		for (int i = 0; i < 24; i++)
			rim_same = rim_same && px(w87.x - 24 + i, y) == src_px(kWinA.x - 24 + i, y) &&
					   px(w87.x + w87.w + i, y) == src_px(kWinA.x + kWinA.w + i, y);
	CHECK(rim_same, "the 24 px rim along the window is the art as drawn, top to bottom");
	bool corner_same = true;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			corner_same = corner_same && px(w87.x + x, w87.y + y) == src_px(kWinA.x + x, kWinA.y + y);
	CHECK(corner_same && px(w87.x, w87.y) == 0xff3264c8u, "the 64x64 corner of the window is copied as drawn, the opaque corner pixel with it");
	const auto b87 = BuildBorder(img, win, &why);
	CHECK(b87 && SameRect(b87->window, w87) && b87->rim.size() > 0, "the stretched art builds into a border with the new window");

	// 16:9: both axes change; the window is still found, the corner is still as drawn
	img = src;
	win = kWinA;
	CHECK(FitToShape(&img, &win, ShapeOf(r43, 224, ps5video::Scale::Stretch), &why) && SameRect(win, w169), "16:9: the window is the new opening");
	CHECK(FindWindow(img, &found, &why) && SameRect(found, w169), "and it is the clear hole the window search finds");
	corner_same = true;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			corner_same = corner_same && px(w169.x + x, w169.y + y) == src_px(kWinA.x + x, kWinA.y + y);
	CHECK(corner_same, "the corner is as drawn");
	CHECK((px(0, 0) & 0x00ffffff) == (src_px(0, 0) & 0x00ffffff), "the outer corner of the frame is the art's");
	bool outer_same = true; // the margins shrink (261 -> 104): the panel is its outer part, as drawn, not squeezed
	for (int y = 0; y < 1080; y++)
		for (int x = 0; x < 80; x++)
			outer_same = outer_same && px(x, y) == src_px(x, y) && px(1919 - x, y) == src_px(1919 - x, y);
	CHECK(outer_same, "a margin that shrinks shows the outer part of its panel as drawn");
	CHECK(BuildBorder(img, win, &why) != nullptr, "the 16:9 art builds into a border");

	// Integer scale: the window is 1194x896; both axes shrink, so the margins stretch (4.4x tall at the top and bottom)
	img = src;
	win = kWinA;
	const Rect wint = {363, 92, 1194, 896};
	CHECK(FitToShape(&img, &win, ShapeOf(r43, 224, ps5video::Scale::Integer), &why) && SameRect(win, wint), "Integer scale: the window is the whole-multiple picture");
	CHECK(FindWindow(img, &found, &why) && SameRect(found, wint), "and it is the clear hole the window search finds in the stretched art");
	CHECK(px(0, 0) == src_px(0, 0) && px(1919, 1079) == src_px(1919, 1079), "the outer corners of the frame are the art's");
	CHECK(px(wint.x - 1, 300) == src_px(kWinA.x - 1, 300) && px(wint.x + wint.w, 300) == src_px(kWinA.x + kWinA.w, 300), "no blend across the cut at the sides");
	corner_same = true;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			corner_same = corner_same && px(wint.x + x, wint.y + y) == src_px(kWinA.x + x, kWinA.y + y);
	CHECK(corner_same, "the corner of the window is as drawn");
	CHECK(BuildBorder(img, win, &why) != nullptr, "the Integer art builds into a border");
	img = src;
	win = wint;
	CHECK(FitToShape(&img, &win, ShapeOf(r43, 224, ps5video::Scale::Integer), &why) && SameRect(win, wint) && img.px == src.px, "a window that is the whole multiple already: nothing is touched");

	// a window given by hand, an RGB picture (no hole of its own): punched, then stretched with its window
	Image rgb;
	rgb.w = 1920;
	rgb.h = 1080;
	rgb.px.assign(size_t(1920) * 1080, 0xff804020u);
	const Rect given = {300, 100, 1200, 800};
	PunchWindow(&rgb, given);
	win = given;
	CHECK(FitToShape(&rgb, &win, ShapeOf(r87), &why) && Shaped(win, 8, 7) && FindWindow(rgb, &found, &why) && SameRect(found, win),
		"a window from an .ini is stretched too, and found again in the result");

	// a window that touches the edge has no margin to stretch: the surface still ends up with a hole of the new shape
	Image full;
	full.w = 1920;
	full.h = 1080;
	full.px.assign(size_t(1920) * 1080, 0);
	win = {0, 0, 1920, 1080};
	CHECK(FitToShape(&full, &win, ShapeOf(r43), &why) && SameRect(win, Rect{240, 0, 1440, 1080}), "a full-screen window narrows to 4:3, centred");

	Image wrong;
	wrong.w = 10;
	wrong.h = 10;
	win = kWinA;
	CHECK(!RefitImage(&wrong, kWinA, w87, &why), "an image of the wrong size is refused");
}

// ---- the pack ----
void MakePack(const std::string& root)
{
	mkdir((root + "/borders").c_str(), 0777);
	mkdir((root + "/borders/MegaDrive").c_str(), 0777);
	mkdir((root + "/borders/GameGear").c_str(), 0777);
}

void ClearPack(const std::string& pack)
{
	const std::string games = pack + "/MegaDrive", gg = pack + "/GameGear";
	if (FILE* p = popen(("rm -rf '" + games + "' '" + gg + "' '" + pack + "/default.png' '" + pack + "/default.ini'").c_str(), "r"))
		pclose(p);
	mkdir(games.c_str(), 0777);
	mkdir(gg.c_str(), 0777);
}

void TestChoose()
{
	using fe::border::Choose;
	const std::string pack = g_root + "/borders";
	const std::string games = pack + "/MegaDrive/";
	ClearPack(pack);

	const std::vector<uint8_t> png_a = ArtPng(kWinA), png_b = ArtPng(kWinB), png_c = ArtPng(kWinC), png_d = ArtPng(kWinD);

	// nothing in the pack: the built-in frame
	fe::border::Pick pick = Choose(pack, 1, Game("Sonic The Hedgehog 2 (World)", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.border && pick.source == "built-in", "an empty pack falls back to the built-in frame");
	CHECK(pick.border && pick.border->window.w >= 480 && pick.border->window.h >= 270, "the built-in frame has a usable window");
	pick = Choose("/nonexistent/pack", 1, Game("X"));
	CHECK(pick.border && pick.source == "built-in", "a missing pack folder falls back to the built-in frame");

	// default.png
	WriteBytes(pack + "/default.png", png_d);
	pick = Choose(pack, 1, Game("Sonic The Hedgehog 2 (World)", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "default.png" && pick.border && SameRect(pick.border->window, kWinD), "no border of its own: default.png");

	// the matcher: the pack calls the game by its No-Intro name, the ROM is called something else
	WriteBytes(games + "Sonic The Hedgehog 2 (World).png", png_c);
	pick = Choose(pack, 1, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "matched" && pick.border && SameRect(pick.border->window, kWinC), "a renamed ROM finds its game's border by the matcher");
	CHECK(pick.file == "MegaDrive/Sonic The Hedgehog 2 (World).png", "and the pick names the file, in its system's folder");

	// the ROM's own file name wins over the matcher
	WriteBytes(games + "sth2.png", png_b);
	pick = Choose(pack, 1, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "own file" && pick.border && SameRect(pick.border->window, kWinB), "MegaDrive/<ROM name>.png wins over the matcher");

	// Default frame ignores the game's own
	pick = Choose(pack, 2, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "default.png" && pick.border && SameRect(pick.border->window, kWinD), "Default frame uses default.png, not the game's border");
	Remove(pack + "/default.png");
	pick = Choose(pack, 2, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "built-in", "Default frame without default.png uses the built-in frame");
	WriteBytes(pack + "/default.png", png_d);

	// a file that is there but unusable is skipped, the next choice applies
	WriteBytes(games + "sth2.png", std::vector<uint8_t>(500, 7));
	pick = Choose(pack, 1, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "matched" && pick.border && SameRect(pick.border->window, kWinC), "a corrupt own file falls through to the matcher");
	WriteBytes(games + "Sonic The Hedgehog 2 (World).png", ArtPng({900, 500, 300, 200}));
	pick = Choose(pack, 1, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "default.png", "own and matched both unusable (corrupt, tiny window): default.png");
	WriteBytes(pack + "/default.png", PngRgba(64, 64, [](int, int) { return Px{0, 0, 0, 0}; }));
	pick = Choose(pack, 1, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "built-in" && pick.border, "a default.png of the wrong size falls through to the built-in frame");
	WriteBytes(pack + "/default.png", png_d);
	WriteBytes(games + "sth2.png", png_b);
	WriteBytes(games + "Sonic The Hedgehog 2 (World).png", png_c);

	// .ini: off
	WriteText(games + "sth2.ini", "off=1\n");
	pick = Choose(pack, 1, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "off" && !pick.border, "sth2.ini with off=1: no border at all");
	Remove(games + "sth2.png");
	pick = Choose(pack, 1, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "off" && !pick.border, "off=1 beside the ROM's name works without a picture of that name");
	Remove(games + "sth2.ini");
	WriteText(games + "Sonic The Hedgehog 2 (World).ini", "# no border here\noff = 1\n");
	pick = Choose(pack, 1, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "off" && !pick.border, "off=1 beside the matched picture: no border");
	pick = Choose(pack, 2, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "default.png", "off=1 does not touch the Default frame setting");
	WriteText(games + "Sonic The Hedgehog 2 (World).ini", "off=maybe\n");
	pick = Choose(pack, 1, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "matched", "a bad off value is ignored");
	Remove(games + "Sonic The Hedgehog 2 (World).ini");

	// .ini: window
	WriteText(games + "Sonic The Hedgehog 2 (World).ini", "window=100, 80, 1200, 900\n");
	pick = Choose(pack, 1, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.source == "matched" && pick.border && SameRect(pick.border->window, Rect{100, 80, 1200, 900}), "window= replaces the one found");
	WriteText(games + "Sonic The Hedgehog 2 (World).ini", "window=100,80,10,10\nunknown=1\n");
	pick = Choose(pack, 1, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.border && SameRect(pick.border->window, kWinC), "a window= that is too small is ignored, the picture's own window is found");
	WriteText(games + "Sonic The Hedgehog 2 (World).ini", "window=1,2,3\n");
	pick = Choose(pack, 1, Game("sth2", "Sonic The Hedgehog 2 (World)"));
	CHECK(pick.border && SameRect(pick.border->window, kWinC), "a window= with three numbers is ignored");
	WriteText(pack + "/default.ini", "window=50,50,800,600\n");
	pick = Choose(pack, 2, Game("sth2"));
	CHECK(pick.border && SameRect(pick.border->window, Rect{50, 50, 800, 600}), "default.ini gives default.png a window");
	Remove(pack + "/default.ini");
	Remove(games + "Sonic The Hedgehog 2 (World).ini");

	// the RGB PNG has no alpha: no window to find, but a window= punches one out (and with it any rim)
	WriteBytes(games + "plain.png", PngRgb(1920, 1080, [](int x, int y) { return Px{uint8_t(x), uint8_t(y), 9, 255}; }));
	pick = Choose(pack, 1, Game("plain"));
	CHECK(pick.source == "default.png", "an RGB PNG has no clear centre: refused without a window=, default.png");
	WriteText(games + "plain.ini", "window=300,100,1200,800\n");
	pick = Choose(pack, 1, Game("plain"));
	CHECK(pick.source == "own file" && pick.border && SameRect(pick.border->window, Rect{300, 100, 1200, 800}), "with a window= an RGB PNG is a frame");
	CHECK(pick.border && pick.border->rim.empty(), "whose window has no rim");
	CHECK(pick.border && pick.border->backdrop[size_t(400) * 1920 + 900] == 0xff000000u, "and is black inside the window");
	CHECK(pick.border && pick.border->backdrop[size_t(99) * 1920 + 900] != 0xff000000u, "while the picture outside it is kept");
	WriteText(games + "plain.ini", "window=300,100,479,400\n");
	pick = Choose(pack, 1, Game("plain"));
	CHECK(pick.source == "default.png", "a window= under 480 wide is ignored, so the RGB PNG has no window again");
	// an RGBA picture with a window= loses its soft edge: the rectangle is a hole
	WriteBytes(games + "soft.png", png_a);
	WriteText(games + "soft.ini", "window=261,21,1398,1038\n");
	pick = Choose(pack, 1, Game("soft"));
	CHECK(pick.source == "own file" && pick.border && pick.border->rim.empty(), "window= clears the soft edge and the corner inside it: no rim");
	Remove(games + "soft.png");
	Remove(games + "soft.ini");
	Remove(games + "plain.png");
	Remove(games + "plain.ini");

	// upper-case extension, odd characters in the name
	WriteBytes(games + "Dr. Mario & Co! (USA).PNG", png_a);
	pick = Choose(pack, 1, Game("Dr. Mario & Co! (USA)"));
	CHECK(pick.source == "own file" && pick.border && SameRect(pick.border->window, kWinA), "a .PNG file with dots and symbols in its name is found");

	// a system's own default.png comes before the pack's; the systems' borders are kept apart
	const std::string gg = pack + "/GameGear/";
	WriteBytes(gg + "default.png", png_b);
	pick = Choose(pack, 1, Game("Unknown Handheld Game", "", fe::System::Gg));
	CHECK(pick.source == "system default.png" && pick.file == "GameGear/default.png" && pick.border && SameRect(pick.border->window, kWinB),
		"a Game Gear game with no border of its own: GameGear/default.png");
	pick = Choose(pack, 2, Game("Unknown Handheld Game", "", fe::System::Gg));
	CHECK(pick.source == "system default.png", "and Default frame takes it too");
	pick = Choose(pack, 1, Game("Unknown Console Game"));
	CHECK(pick.source == "default.png", "a Mega Drive game, with no MegaDrive/default.png: the pack's default.png");
	pick = Choose(pack, 1, Game("Sonic The Hedgehog 2 (World)", "Sonic The Hedgehog 2 (World)", fe::System::Gg));
	CHECK(pick.source == "system default.png", "a Game Gear game never takes a Mega Drive game's border of the same name");
	pick = Choose(pack, 1, Game("default", "default", fe::System::Gg));
	CHECK(pick.source == "system default.png", "default.png is no game's own border");
	Remove(gg + "default.png");

	// time one choice: a typical file, decoded, searched, built (host figure; the console is slower)
	const auto t0 = std::chrono::steady_clock::now();
	pick = Choose(pack, 1, Game("Dr. Mario & Co! (USA)"));
	const auto t1 = std::chrono::steady_clock::now();
	printf("info: one choice (decode, window, backdrop, rim) took %lld ms on the host\n",
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
	pick = Choose(pack, 1, Game("Dr. Mario & Co! (USA)"), ShapeOf(4.0 / 3.0, 224, ps5video::Scale::Stretch));
	const auto t2 = std::chrono::steady_clock::now();
	printf("info: the same stretched to a 16:9 window took %lld ms\n",
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count());
}

void TestSelect()
{
	const std::string pack = g_root + "/borders";
	const std::string games = pack + "/MegaDrive/";
	ClearPack(pack);
	WriteBytes(games + "Mapped Game.png", ArtPng(kWinB));
	WriteBytes(games + "Mapped Game (USA).png", ArtPng(kWinC));

	fe::Config().border = 0;
	ps5video::g_set_calls = 0;
	fe::border::Select("/data/genplus/roms/Mapped Game.md", fe::border::Shape());
	CHECK(ps5video::g_set_calls == 0 && !ps5video::g_shown, "Border Off: the renderer is not touched");

	fe::Config().border = 1;
	fe::border::Reselect(fe::border::Shape());
	CHECK(ps5video::g_set_calls == 1 && ps5video::g_shown && SameRect(ps5video::g_shown->window, kWinB), "Off -> Auto while a game runs: its own border is set");

	fe::border::Reselect(fe::border::Shape());
	CHECK(ps5video::g_set_calls == 1, "Reselect with the setting unchanged does nothing (every settings change comes through it)");

	fe::Config().border = 2;
	fe::border::Reselect(fe::border::Shape());
	CHECK(ps5video::g_set_calls == 2 && ps5video::g_shown && ps5video::g_shown->window.w >= 480, "Auto -> Default frame: the built-in frame");

	fe::Config().border = 0;
	fe::border::Reselect(fe::border::Shape());
	CHECK(ps5video::g_set_calls == 3 && !ps5video::g_shown, "-> Off: the border is cleared");
	fe::border::Reselect(fe::border::Shape());
	CHECK(ps5video::g_set_calls == 3, "and clearing again does not touch the renderer");

	fe::Config().border = 1;
	fe::border::Select("/data/genplus/roms/Mapped Game.md", fe::border::Shape());
	CHECK(ps5video::g_shown && SameRect(ps5video::g_shown->window, kWinB), "Select sets the border of the loaded game");
	fe::border::Release();
	CHECK(!ps5video::g_shown, "Release clears it");
	const int calls = ps5video::g_set_calls;
	fe::Config().border = 2;
	fe::border::Reselect(fe::border::Shape());
	CHECK(ps5video::g_set_calls == calls, "Reselect with no game does nothing");
	fe::border::Release();
	CHECK(ps5video::g_set_calls == calls, "and Release with nothing shown does not touch the renderer");

	// a corrupt pack never fails Select
	WriteBytes(games + "Mapped Game.png", std::vector<uint8_t>(10, 1));
	WriteBytes(games + "Mapped Game (USA).png", std::vector<uint8_t>(10, 1));
	fe::Config().border = 1;
	fe::border::Select("/data/genplus/roms/Mapped Game.md", fe::border::Shape());
	CHECK(ps5video::g_shown != nullptr, "a pack of corrupt files still ends in the built-in frame");
	fe::border::Release();
}

// ---- the names The Bezel Project has, the downloads, and the border that arrives while a game runs ----
std::string ReadText(const std::string& path)
{
	std::string out;
	if (FILE* f = fopen(path.c_str(), "rb"))
	{
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			out.append(buf, n);
		fclose(f);
	}
	return out;
}

bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}

// The helper's way of saving: the whole file appears at once.
void Land(const std::string& path, const std::vector<uint8_t>& b)
{
	WriteBytes(path + ".part", b);
	rename((path + ".part").c_str(), path.c_str());
}

bool WaitForCalls(int calls, int ms = 4000)
{
	for (int t = 0; t < ms; t += 20)
	{
		if (ps5video::g_set_calls >= calls)
			return true;
		usleep(20 * 1000);
	}
	return ps5video::g_set_calls >= calls;
}

void TestFetch()
{
	using namespace fe::border;
	using fe::WantedCover;
	const std::string pack = g_root + "/borders";
	const std::string games = pack + "/MegaDrive/";
	ClearPack(pack);
	unlink((pack + "/wanted.txt").c_str());
	unsetenv("GENPLUS_BORDER_URL");

	using fe::System;
	const std::vector<std::string>& names = IndexNames(System::Md);
	CHECK(names.size() > 3000 && names.size() < 4000, "the Mega Drive index has the 3286 borders The Bezel Project had in 2026-10");
	CHECK(IndexNames(System::SegaCd).size() > 400 && IndexNames(System::Sms).size() > 1000 && IndexNames(System::Gg).size() > 800 &&
			IndexNames(System::Sg).size() > 200,
		"and every other system has its own list (Sega CD 468, Master System 1065, Game Gear 850, SG-1000 212)");
	CHECK(std::find(names.begin(), names.end(), "Sonic The Hedgehog 2 (World)") != names.end(), "Sonic The Hedgehog 2 (World) is in it");
	CHECK(std::find(names.begin(), names.end(), "Sonic & Knuckles (World)") != names.end(), "so is a name with an ampersand, as the repository spells it");
	const std::vector<std::string>& gg_names = IndexNames(System::Gg);
	CHECK(std::find(gg_names.begin(), gg_names.end(), "Sonic The Hedgehog (Japan, USA) (En)") != gg_names.end() &&
			std::find(names.begin(), names.end(), "Sonic The Hedgehog (Japan, USA) (En)") == names.end(),
		"a Game Gear game is in the Game Gear list, not the Mega Drive's");
	CHECK(std::set<std::string>(names.begin(), names.end()).size() == names.size(), "no name twice");
	bool clean = true;
	for (int sys = 0; sys < int(System::Count); sys++)
		for (const std::string& n : IndexNames(System(sys)))
			clean = clean && !n.empty() && n.find('/') == std::string::npos && n.find('\t') == std::string::npos &&
				n.size() < 200 && (n.size() < 4 || n.compare(n.size() - 4, 4, ".png") != 0);
	CHECK(clean, "every name is a plain file name without the extension");

	CHECK(BezelUrl(System::Md, "Sonic & Knuckles (World)") ==
			"https://raw.githubusercontent.com/thebezelproject/bezelproject-MegaDrive/master/retroarch/overlay/GameBezels/MegaDrive/"
			"Sonic%20%26%20Knuckles%20%28World%29.png",
		"the address is the repository's raw file, every odd character escaped");
	CHECK(BezelUrl(System::Md, "Arcade Legends Street Fighter II' - Special Champion Edition (World)").find("II%27%20-%20") != std::string::npos,
		"an apostrophe is escaped");
	CHECK(BezelUrl(System::Gg, "X").find("/bezelproject-GameGear/master/retroarch/overlay/GameBezels/GameGear/X.png") != std::string::npos &&
			BezelUrl(System::Sms, "X").find("/bezelproject-MasterSystem/master/retroarch/overlay/GameBezels/MasterSystem/") != std::string::npos &&
			BezelUrl(System::Sg, "X").find("/bezelproject-SG-1000/master/retroarch/overlay/GameBezels/SG-1000/") != std::string::npos &&
			BezelUrl(System::SegaCd, "X").find("/bezelproject-SegaCD/master/retroarch/overlay/GameBezels/SegaCD/") != std::string::npos,
		"each system's borders come from its own repository");
	setenv("GENPLUS_BORDER_URL", "http://127.0.0.1:1/b/", 1);
	CHECK(BezelUrl(System::Md, "A (USA)") == "http://127.0.0.1:1/b/A%20%28USA%29.png", "GENPLUS_BORDER_URL replaces the base (the host tests)");
	unsetenv("GENPLUS_BORDER_URL");

	WantedCover w;
	CHECK(BorderWanted(pack, Game("Sonic The Hedgehog 2 (World)", "Sonic The Hedgehog 2 (World)"), &w) &&
			w.file == "MegaDrive/Sonic The Hedgehog 2 (World).png" && w.url == BezelUrl(System::Md, "Sonic The Hedgehog 2 (World)"),
		"a game The Bezel Project has a border for, and the pack lacks, is wanted, into its system's folder");
	CHECK(fe::SafeCoverFile(w.file), "and the helper takes the file name (a system folder, then a plain name)");
	CHECK(!BorderWanted(pack, Game("Not In Any Pack (USA)", "Not In Any Pack (USA)"), &w), "a game it has none for is not");
	CHECK(BorderWanted(pack, Game("Sonic The Hedgehog 2", "Sonic The Hedgehog 2"), &w) == false, "(the stand-in matcher knows only exact names)");
	CHECK(BorderWanted(pack, Game("Sonic The Hedgehog (Japan, USA) (En)", "Sonic The Hedgehog (Japan, USA) (En)", System::Gg), &w) &&
			w.file == "GameGear/Sonic The Hedgehog (Japan, USA) (En).png" && w.url.find("bezelproject-GameGear") != std::string::npos,
		"a Game Gear game is wanted from the Game Gear repository");
	CHECK(!BorderWanted(pack, Game("Sonic The Hedgehog (Japan, USA) (En)", "Sonic The Hedgehog (Japan, USA) (En)", System::Md), &w),
		"and a Mega Drive game of that name has none");

	// the loose translation: matched through the name its CRC has
	fe::GameInfo loose = Game("Loose Translation", "Loose Translation");
	loose.crc_name = "Sonic The Hedgehog 2 (World)";
	CHECK(BezelNameFor(loose) == "Sonic The Hedgehog 2 (World)", "a translation is given its game's border by the CRC name");

	WriteBytes(games + "Sonic The Hedgehog 2 (World).png", ArtPng(kWinA));
	CHECK(!BorderWanted(pack, Game("Sonic The Hedgehog 2 (World)", "Sonic The Hedgehog 2 (World)"), &w), "a border already in the pack is not wanted again");
	unlink((games + "Sonic The Hedgehog 2 (World).png").c_str());

	// a 404 is not asked again for 30 days, and only for the same address
	const std::string marker = games + "Sonic The Hedgehog 2 (World).missing";
	if (FILE* f = fopen(marker.c_str(), "w"))
	{
		fprintf(f, "%s\n", BezelUrl(System::Md, "Sonic The Hedgehog 2 (World)").c_str());
		fclose(f);
	}
	CHECK(!BorderWanted(pack, Game("Sonic The Hedgehog 2 (World)", "Sonic The Hedgehog 2 (World)"), &w), "a recent 404 for the address: not wanted");
	setenv("GENPLUS_BORDER_URL", "http://127.0.0.1:1/b/", 1);
	CHECK(BorderWanted(pack, Game("Sonic The Hedgehog 2 (World)", "Sonic The Hedgehog 2 (World)"), &w), "a 404 for another address does not count");
	unsetenv("GENPLUS_BORDER_URL");
	struct timeval old[2] = {{time(nullptr) - 31L * 24 * 3600, 0}, {time(nullptr) - 31L * 24 * 3600, 0}};
	utimes(marker.c_str(), old);
	CHECK(BorderWanted(pack, Game("Sonic The Hedgehog 2 (World)", "Sonic The Hedgehog 2 (World)"), &w), "a 404 older than 30 days is tried again");
	unlink(marker.c_str());

	// the library: one picture per bezel, none for a ROM with its own border, none for a game without one
	WriteBytes(games + "Columns (USA, Europe).png", ArtPng(kWinA)); // the player's own border for this ROM name
	std::vector<fe::GameInfo> library = {Game("Sonic The Hedgehog 2 (World)", "Sonic The Hedgehog 2 (World)"),
		Game("Sonic The Hedgehog 2 (World)", "Sonic The Hedgehog 2 (World)"), Game("Columns (USA, Europe)", "Columns (USA, Europe)"),
		Game("Gunstar Heroes (USA)", "Gunstar Heroes (USA)"), Game("Not In Any Pack (USA)", "Not In Any Pack (USA)"), loose,
		Game("Sonic The Hedgehog (Japan, USA) (En)", "Sonic The Hedgehog (Japan, USA) (En)", System::Gg)};
	const std::vector<WantedCover> missing = MissingBorders(pack, library);
	CHECK(missing.size() == 3 && missing[0].file == "MegaDrive/Sonic The Hedgehog 2 (World).png" &&
			missing[1].file == "MegaDrive/Gunstar Heroes (USA).png" && missing[2].file == "GameGear/Sonic The Hedgehog (Japan, USA) (En).png",
		"the library list has Sonic 2 (once, though two ROMs and a translation share it), Gunstar Heroes and the Game Gear Sonic");

	WriteWantedBorders(pack, missing);
	WritePriorityBorders(pack, {missing[1]});
	const std::vector<WantedCover> read = fe::ParseWantedList(ReadText(pack + "/wanted.txt"));
	CHECK(read.size() == 3 && read[0].file == missing[0].file && read[0].url == missing[0].url, "borders/wanted.txt is the helper's wanted-list format");
	const std::vector<WantedCover> prio = fe::ParseWantedList(ReadText(pack + "/priority.txt"));
	CHECK(prio.size() == 1 && prio[0].file == "MegaDrive/Gunstar Heroes (USA).png", "borders/priority.txt holds the one given");
	WriteWantedBorders(pack, {});
	CHECK(fe::ParseWantedList(ReadText(pack + "/wanted.txt")).empty(), "and an empty list empties the file");
}

void TestAutoDownload()
{
	const std::string pack = g_root + "/borders";
	const std::string games = pack + "/MegaDrive/";
	const std::string rom = "/data/genplus/roms/Sonic The Hedgehog 2 (World).md";
	ClearPack(pack);
	unlink((pack + "/priority.txt").c_str());
	fe::Config().border = 1;
	fe::Config().border_download = true;

	// Auto, no border for the game yet: the built-in frame at once, the helper asked, the border put up when it lands
	ps5video::g_set_calls = 0;
	fe::border::Select(rom, fe::border::Shape());
	CHECK(ps5video::g_set_calls == 1 && ps5video::g_shown && ps5video::g_shown->window.w >= 480, "no border yet: the game starts with the built-in frame");
	const std::vector<fe::WantedCover> asked = fe::ParseWantedList(ReadText(pack + "/priority.txt"));
	CHECK(asked.size() == 1 && asked[0].file == "MegaDrive/Sonic The Hedgehog 2 (World).png", "and the helper is asked for it, first (borders/priority.txt)");
	usleep(700 * 1000);
	CHECK(ps5video::g_set_calls == 1, "nothing changes while the picture has not arrived");
	Land(games + "Sonic The Hedgehog 2 (World).png", ArtPng(kWinD));
	CHECK(WaitForCalls(2), "the picture lands: the game's border is put up");
	CHECK(ps5video::g_shown && SameRect(ps5video::g_shown->window, kWinD), "it is the downloaded picture");
	fe::border::Release();
	CHECK(!ps5video::g_shown, "Release clears it");

	// the next time the game starts, the file is in the pack: found at once, nothing asked
	unlink((pack + "/priority.txt").c_str());
	ps5video::g_set_calls = 0;
	fe::border::Select(rom, fe::border::Shape());
	CHECK(ps5video::g_set_calls == 1 && SameRect(ps5video::g_shown->window, kWinD), "the next start finds the downloaded border in the pack");
	CHECK(!Exists(pack + "/priority.txt"), "and asks for nothing");
	fe::border::Release();

	// the game's name is not the bezel's: the file is found by the index name, not by the pack's own matcher
	ps5video::g_set_calls = 0;
	fe::border::Select("/data/genplus/roms/Loose Translation.md", fe::border::Shape());
	CHECK(ps5video::g_set_calls == 1 && SameRect(ps5video::g_shown->window, kWinD), "a translation finds the downloaded border by The Bezel Project's name for its game");
	fe::border::Release();

	// the game closes before the picture arrives: it is not put up afterwards, and Release does not wait for the helper
	unlink((games + "Sonic The Hedgehog 2 (World).png").c_str());
	ps5video::g_set_calls = 0;
	fe::border::Select(rom, fe::border::Shape());
	const auto t0 = std::chrono::steady_clock::now();
	fe::border::Release();
	const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
	CHECK(ms < 400, "Release stops the watcher at once");
	const int calls = ps5video::g_set_calls;
	Land(games + "Sonic The Hedgehog 2 (World).png", ArtPng(kWinB));
	usleep(1200 * 1000);
	CHECK(ps5video::g_set_calls == calls, "a picture that arrives after the game closed is not put up");

	// another game starts before it arrives
	unlink((games + "Sonic The Hedgehog 2 (World).png").c_str());
	fe::border::Select(rom, fe::border::Shape());
	fe::border::Select("/data/genplus/roms/Unknown Game.md", fe::border::Shape());
	const int calls2 = ps5video::g_set_calls;
	Land(games + "Sonic The Hedgehog 2 (World).png", ArtPng(kWinB));
	usleep(1200 * 1000);
	CHECK(ps5video::g_set_calls == calls2, "nor when another game has started meanwhile");
	fe::border::Release();

	// the setting changes while it waits
	unlink((games + "Sonic The Hedgehog 2 (World).png").c_str());
	fe::border::Select(rom, fe::border::Shape());
	fe::Config().border = 2;
	fe::border::Reselect(fe::border::Shape());
	const int calls3 = ps5video::g_set_calls;
	Land(games + "Sonic The Hedgehog 2 (World).png", ArtPng(kWinB));
	usleep(1200 * 1000);
	CHECK(ps5video::g_set_calls == calls3, "nor after Auto was switched to Default frame");
	fe::border::Release();

	// a 404: the watcher gives up, the frame stays
	fe::Config().border = 1;
	unlink((games + "Sonic The Hedgehog 2 (World).png").c_str());
	fe::border::Select(rom, fe::border::Shape());
	const int calls4 = ps5video::g_set_calls;
	if (FILE* f = fopen((games + "Sonic The Hedgehog 2 (World).missing").c_str(), "w"))
	{
		fprintf(f, "%s\n", fe::border::BezelUrl(fe::System::Md, "Sonic The Hedgehog 2 (World)").c_str());
		fclose(f);
	}
	usleep(1200 * 1000);
	CHECK(ps5video::g_set_calls == calls4, "a 404 leaves the built-in frame");
	fe::border::Release();
	unlink((games + "Sonic The Hedgehog 2 (World).missing").c_str());

	// downloads off: nothing asked, nothing watched
	fe::Config().border_download = false;
	unlink((pack + "/priority.txt").c_str());
	fe::border::Select(rom, fe::border::Shape());
	CHECK(!Exists(pack + "/priority.txt"), "border_download=0: the helper is not asked");
	fe::border::Release();
	fe::Config().border_download = true;

	// Default frame never asks, a game with its own file never asks
	fe::Config().border = 2;
	fe::border::Select(rom, fe::border::Shape());
	CHECK(!Exists(pack + "/priority.txt"), "Default frame: the helper is not asked");
	fe::border::Release();
	fe::Config().border = 1;
	WriteBytes(games + "Sonic The Hedgehog 2 (World).ini", std::vector<uint8_t>{'o', 'f', 'f', '=', '1', '\n'});
	fe::border::Select(rom, fe::border::Shape());
	CHECK(!Exists(pack + "/priority.txt"), "off=1 in the game's .ini: the helper is not asked");
	fe::border::Release();
}
// ---- the picture's shape reaches the border ----
void TestShape()
{
	using ps5video::Scale;
	const std::string pack = g_root + "/borders";
	const std::string games = pack + "/MegaDrive/";
	const std::string rom = "/data/genplus/roms/Sonic The Hedgehog 2 (World).md";
	const fe::GameInfo game = Game("Sonic The Hedgehog 2 (World)", "Sonic The Hedgehog 2 (World)");
	const double r43 = 4.0 / 3.0;
	ClearPack(pack);
	unlink((pack + "/priority.txt").c_str());
	fe::Config().border_download = false;
	fe::Config().border = 1;

	// the choice: a window of the picture's shape, from the same art (kWinB is 3:2)
	WriteBytes(games + "Sonic The Hedgehog 2 (World).png", ArtPng(kWinB));
	fe::border::Pick pick = fe::border::Choose(pack, 1, game, ShapeOf(r43));
	CHECK(pick.border && pick.source == "own file" && Shaped(pick.border->window, 4, 3), "a 4:3 picture: the game's own border gets a 4:3 window");
	pick = fe::border::Choose(pack, 1, game, ShapeOf(320.0 / 224));
	CHECK(pick.border && Shaped(pick.border->window, 10, 7), "square pixels at 320x224: a 10:7 window");
	pick = fe::border::Choose(pack, 1, game, ShapeOf(r43, 224, Scale::Stretch));
	CHECK(pick.border && Shaped(pick.border->window, 16, 9), "Stretch to 16:9: a 16:9 window");
	pick = fe::border::Choose(pack, 1, game, ShapeOf(r43, 224, Scale::Integer));
	CHECK(pick.border && SameRect(pick.border->window, fe::border::IntegerOpening(kWinB, 224, r43)) && pick.border->window.w == 1194 &&
			pick.border->window.h == 896,
		"Integer scale: the window is the 1194x896 picture, no black around it");
	pick = fe::border::Choose(pack, 1, game);
	CHECK(pick.border && SameRect(pick.border->window, kWinB), "and the window as drawn is what no shape asks for");

	// a Game Gear border (a 4:3 bezel around the handheld's 4:3 picture, 160x144 at 6:5 pixels): it keeps its window at Fit, and
	// at Integer scale the window is 7x the 144-line picture
	const std::string gg = pack + "/GameGear/";
	const fe::GameInfo handheld = Game("Sonic The Hedgehog (Japan, USA) (En)", "Sonic The Hedgehog (Japan, USA) (En)", fe::System::Gg);
	const Rect bezel = {274, 0, 1372, 1080}; // The Bezel Project's Game Gear window: the whole height, 1.27:1
	WriteBytes(gg + "Sonic The Hedgehog (Japan, USA) (En).png", ArtPng(bezel));
	pick = fe::border::Choose(pack, 1, handheld, ShapeOf(160.0 * 6 / 5 / 144, 144));
	CHECK(pick.border && pick.source == "own file" && pick.file == "GameGear/Sonic The Hedgehog (Japan, USA) (En).png" &&
			Shaped(pick.border->window, 4, 3) && pick.border->window.h == 1080,
		"a Game Gear bezel takes the handheld picture's 4:3 shape, full height");
	pick = fe::border::Choose(pack, 1, handheld, ShapeOf(160.0 * 6 / 5 / 144, 144, Scale::Integer));
	CHECK(pick.border && pick.border->window.h == 1008 && pick.border->window.w == 1344, "Integer scale: 7x the 144-line picture, 1344x1008");

	// the built-in frame, which is 1398x1038 (4:3) as drawn
	ClearPack(pack);
	const Rect frame = fe::border::Choose(pack, 2, game).border->window;
	CHECK(SameRect(fe::border::Choose(pack, 2, game, ShapeOf(r43)).border->window, frame), "the built-in frame is already 4:3: unchanged");
	const Rect frame107 = fe::border::Choose(pack, 2, game, ShapeOf(320.0 / 224)).border->window;
	const Rect frame169 = fe::border::Choose(pack, 2, game, ShapeOf(r43, 224, Scale::Stretch)).border->window;
	CHECK(SameRect(frame107, fe::border::FitOpening(frame, 320.0 / 224)) && Shaped(frame107, 10, 7), "the built-in frame: a 10:7 window");
	CHECK(SameRect(frame169, fe::border::FitOpening(frame, 16.0 / 9)) && Shaped(frame169, 16, 9) && frame169.w > frame.w, "and a wider 16:9 one");

	// Reselect: the shape changes while a game runs (a setting, or the game's own screen mode)
	WriteBytes(games + "Sonic The Hedgehog 2 (World).png", ArtPng(kWinB));
	fe::border::Select(rom, fe::border::Shape());
	CHECK(ps5video::g_shown && SameRect(ps5video::g_shown->window, kWinB), "no shape: the picture as drawn");
	int calls = ps5video::g_set_calls;
	fe::border::Reselect(ShapeOf(320.0 / 224));
	CHECK(ps5video::g_set_calls == calls + 1 && Shaped(ps5video::g_shown->window, 10, 7), "-> square pixels while a game runs: the border is fitted again");
	fe::border::Reselect(ShapeOf(320.0 / 224 * 1.002));
	CHECK(ps5video::g_set_calls == calls + 1, "and nothing more while the shape stays (within half a percent)");
	fe::border::Reselect(ShapeOf(r43, 224, Scale::Stretch));
	CHECK(ps5video::g_set_calls == calls + 2 && Shaped(ps5video::g_shown->window, 16, 9), "-> Stretch: a 16:9 window");
	fe::Config().border = 0;
	fe::border::Reselect(ShapeOf(r43, 224, Scale::Stretch));
	CHECK(ps5video::g_set_calls == calls + 3 && !ps5video::g_shown, "Border Off clears it");
	fe::border::Reselect(ShapeOf(r43, 224, Scale::Integer));
	CHECK(ps5video::g_set_calls == calls + 3, "and with Border Off the shape has nothing to fit");
	fe::Config().border = 1;
	fe::border::Reselect(ShapeOf(r43, 224, Scale::Integer));
	CHECK(ps5video::g_set_calls == calls + 4 && SameRect(ps5video::g_shown->window, fe::border::IntegerOpening(kWinB, 224, r43)),
		"Border back to Auto: the picture, fitted to the shape of the moment (Integer scale)");
	fe::border::Reselect(ShapeOf(r43, 240, Scale::Integer));
	CHECK(ps5video::g_set_calls == calls + 5 && SameRect(ps5video::g_shown->window, fe::border::IntegerOpening(kWinB, 240, r43)),
		"a 240-line picture (a PAL game) at Integer scale: the window follows the line count");
	fe::border::Release();

	// a border that is downloaded while the game runs is fitted to the shape of the moment
	unlink((games + "Sonic The Hedgehog 2 (World).png").c_str());
	unlink((pack + "/priority.txt").c_str());
	fe::Config().border_download = true;
	ps5video::g_set_calls = 0;
	fe::border::Select(rom, ShapeOf(r43, 224, Scale::Stretch));
	CHECK(ps5video::g_set_calls == 1 && Shaped(ps5video::g_shown->window, 16, 9), "the frame it starts with is fitted");
	Land(games + "Sonic The Hedgehog 2 (World).png", ArtPng(kWinD));
	CHECK(WaitForCalls(2) && Shaped(ps5video::g_shown->window, 16, 9) && ps5video::g_shown->window.h == kWinD.h, "and so is the downloaded border (kWinD is 10:7)");
	fe::border::Release();
	fe::Config().border_download = true;
}

} // namespace

int main()
{
	char dir[] = "/tmp/border-pack-test-XXXXXX";
	if (!mkdtemp(dir))
	{
		perror("mkdtemp");
		return 2;
	}
	g_root = dir;
	setenv("GENPLUS_PS5_ROOT", dir, 1);
	MakePack(g_root);
	OrbisLogSetEnabled(true);

	TestDecode();
	TestWindow();
	TestBuild();
	TestFit();
	TestChoose();
	TestSelect();
	TestFetch();
	TestAutoDownload();
	TestShape();

	if (FILE* p = popen(("rm -rf '" + g_root + "'").c_str(), "r"))
		pclose(p);
	printf("%d checks, %d failed\n", g_checked, g_failed);
	return g_failed ? 1 : 0;
}

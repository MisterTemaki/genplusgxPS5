// The border in the renderer (ProsperoVideo.cpp): where the picture goes in a window, when the backdrop is restored, what a
// frame reports to Present, and the rim blend, on the CPU (plain picture, smooth picture, a CRT shader) and the surface the GPU
// lays over its picture. Runs the real renderer against the host's stub video output; the GPU renderer itself is stood in for
// below (the host groups run the real one on lavapipe). Ported from Snes9x PS5 2.7's test (pull request #3 by thatboialex).
// SPDX-License-Identifier: MIT

#include "ProsperoGpu.h"
#include "ProsperoVideo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

// ---- the GPU renderer, stood in for: "active" when the test says so, recording what the picture was given ----
namespace ps5gpu
{
bool g_active = false;
bool g_fullscreen = false;
Rect g_game = {0, 0, 0, 0};
bool Init(int, int, bool, int, int)
{
	return g_active;
}
bool Active()
{
	return g_active;
}
void Shutdown() {}
int OutWidth()
{
	return 1920;
}
int OutHeight()
{
	return 1080;
}
const char* DeviceName()
{
	return "test";
}
int ShaderCount()
{
	return 3;
}
const char* ShaderName(int)
{
	return "test shader";
}
bool ShaderFullscreen(int i)
{
	return g_fullscreen && i == 2;
}
void SetGame(const uint32_t*, int, int, Rect dst, int, bool, bool)
{
	g_game = dst;
}
bool Present(const uint32_t*, Rect, bool)
{
	return true;
}
double TakeShaderMs()
{
	return 0;
}
bool Building()
{
	return false;
}
} // namespace ps5gpu

using namespace ps5video;

static int g_fail = 0;
#define CHECK(c)                                                          \
	do                                                                    \
	{                                                                     \
		if (!(c))                                                         \
		{                                                                 \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);           \
			g_fail++;                                                     \
		}                                                                 \
	} while (0)

static bool Same(const Rect& a, const Rect& b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
#define CHECK_RECT(got, ex_x, ex_y, ex_w, ex_h)                                                        \
	do                                                                                                 \
	{                                                                                                  \
		const Rect _g = (got);                                                                         \
		if (!Same(_g, Rect{ex_x, ex_y, ex_w, ex_h}))                                                   \
		{                                                                                              \
			printf("FAIL %s:%d  %s = %d,%d %dx%d, expected %d,%d %dx%d\n", __FILE__, __LINE__, #got,   \
				_g.x, _g.y, _g.w, _g.h, ex_x, ex_y, ex_w, ex_h);                                       \
			g_fail++;                                                                                  \
		}                                                                                              \
	} while (0)

static const double k43 = 4.0 / 3.0;

static void TestComputeRectIn()
{
	// the window of a Bezel Project bezel (1398x1038 at 261,21): 1.347 is within 3% of 4:3, so a 4:3 picture fills it
	const Rect win = {261, 21, 1398, 1038};
	CHECK_RECT(ComputeRectIn(224, k43, Scale::Fit, win), 261, 21, 1398, 1038);
	CHECK_RECT(ComputeRectIn(224, k43, Scale::Stretch, win), 261, 21, 1398, 1038);
	CHECK_RECT(ComputeRectIn(144, 160.0 * 6 / 5 / 144, Scale::Fit, win), 261, 21, 1398, 1038); // the Game Gear's 4:3 too
	CHECK_RECT(ComputeRectIn(224, 320.0 / 224, Scale::Fit, win), 261, 50, 1398, 979); // 10:7 is wider: fitted by width
	CHECK_RECT(ComputeRectIn(224, 8.0 / 7, Scale::Fit, win), 367, 21, 1186, 1038); // 8:7 is narrower: centred
	CHECK_RECT(ComputeRectIn(224, k43, Scale::Integer, win), 363, 92, 1194, 896); // 4x, centred in the window
	CHECK_RECT(ComputeRectIn(144, k43, Scale::Integer, win), 288, 36, 1344, 1008); // a Game Gear's 144 lines: 7x
	// the window Integer scale cuts the art to (IntegerOpening in fe_borderimg.cpp) is exactly 4x: the picture fills it; a picture
	// of another line count is its own whole multiple inside it
	const Rect win4x = {363, 92, 1194, 896};
	CHECK_RECT(ComputeRectIn(224, k43, Scale::Integer, win4x), 363, 92, 1194, 896);
	CHECK_RECT(ComputeRectIn(240, k43, Scale::Integer, win4x), 480, 180, 960, 720);
	CHECK_RECT(ComputeRectIn(224, k43, Scale::Integer, Rect{100, 100, 896, 672}), 100, 100, 896, 672); // 3x window
	// the 3% edge of the fill rule: 1480x1080 is 2.8% off 4:3 and fills, 1490x1080 is 3.5% off and is fitted by height
	CHECK_RECT(ComputeRectIn(224, k43, Scale::Fit, Rect{0, 0, 1480, 1080}), 0, 0, 1480, 1080);
	CHECK_RECT(ComputeRectIn(224, k43, Scale::Fit, Rect{0, 0, 1490, 1080}), 25, 0, 1440, 1080);
	// a window narrower than 4:3: fitted by width
	CHECK_RECT(ComputeRectIn(224, k43, Scale::Fit, Rect{100, 50, 1000, 1000}), 100, 175, 1000, 750);
	// a window smaller than the picture: Integer has no whole multiple that fits and uses the Fit rule; an odd width loses a pixel
	CHECK_RECT(ComputeRectIn(239, k43, Scale::Integer, Rect{100, 100, 320, 180}), 140, 100, 240, 180);
	CHECK_RECT(ComputeRectIn(224, k43, Scale::Stretch, Rect{0, 0, 1001, 1080}), 0, 0, 1000, 1080);
	// nonsense in, a picture inside the window out
	CHECK_RECT(ComputeRectIn(224, NAN, Scale::Fit, win), 261, 21, 1398, 1038);
	CHECK_RECT(ComputeRectIn(0, k43, Scale::Integer, Rect{0, 0, 480, 270}), 60, 0, 360, 270);

	// the smallest window a border may have (480x270, the pack's limit): every scale and picture stays inside it
	const Rect windows[] = {{0, 0, 480, 270}, {1440, 810, 480, 270}, {700, 300, 480, 270}, {261, 21, 1398, 1038}};
	const int lines[] = {144, 192, 224, 240, 448, 480};
	const double aspects[] = {k43, 8.0 / 7, 320.0 / 224, 16.0 / 9, 1.2};
	for (const Rect& w : windows)
		for (int l : lines)
			for (double a : aspects)
				for (int s = 0; s < int(Scale::Count); s++)
				{
					const Rect r = ComputeRectIn(l, a, Scale(s), w);
					const bool inside = r.w > 0 && r.h > 0 && r.w % 2 == 0 && r.x >= w.x && r.y >= w.y && r.x + r.w <= w.x + w.w &&
										r.y + r.h <= w.y + w.h;
					if (!inside)
						printf("FAIL %d lines %.3f scale %d in %d,%d %dx%d: %d,%d %dx%d\n", l, a, s, w.x, w.y, w.w, w.h, r.x, r.y, r.w, r.h);
					CHECK(inside);
				}
	CHECK_RECT(ComputeRectIn(239, k43, Scale::Integer, Rect{0, 0, 480, 270}), 81, 15, 318, 239); // 1x fits
}

// ---- the renderer against a border ------------------------------------------------------------------------------

static const uint32_t kBorderColor = Rgb(10, 20, 200);
static const Rect kWindow = {261, 21, 1398, 1038};

static uint32_t Px(int x, int y)
{
	return Surface()[size_t(y) * kWidth + x];
}

static uint32_t Surf(uint32_t argb) // what DrawFrame makes of a 0xAARRGGBB pixel
{
	return 0xff000000u | ((argb & 0xff) << 16) | (argb & 0xff00) | ((argb >> 16) & 0xff);
}

// The border colour outside the window, black inside it; 'rim' pixels (index order) sit in the window and are on black in
// the backdrop, as fe_border builds it.
static std::shared_ptr<const Border> MakeBorder(uint32_t color, const Rect& window, const std::vector<RimPixel>& rim)
{
	auto b = std::make_shared<Border>();
	b->backdrop.assign(size_t(kWidth) * kHeight, color);
	for (int y = window.y; y < window.y + window.h; y++)
		std::fill(b->backdrop.begin() + size_t(y) * kWidth + window.x, b->backdrop.begin() + size_t(y) * kWidth + window.x + window.w,
			0xff000000u);
	b->window = window;
	b->rim = rim;
	for (const RimPixel& p : rim)
		b->backdrop[p.index] = 0xff000000u | p.pre; // the pixel on black: colour * alpha / 255, which is `pre`
	return b;
}

static RimPixel Rim(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	const uint32_t pr = (uint32_t(r) * a + 127) / 255, pg = (uint32_t(g) * a + 127) / 255, pb = (uint32_t(b) * a + 127) / 255;
	return RimPixel{uint32_t(y) * kWidth + x, (pb << 16) | (pg << 8) | pr, uint32_t(255 - a)};
}

// the blend the renderer has to make, in floating point
static uint32_t Exact(uint32_t pic, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	const double k = a / 255.0;
	auto ch = [&](uint32_t p, int shift, uint8_t c) { return uint32_t(std::lround(((p >> shift) & 0xff) * (1 - k) + c * k)); };
	return 0xff000000u | (ch(pic, 16, b) << 16) | (ch(pic, 8, g) << 8) | ch(pic, 0, r);
}

// the blend as the renderer does it, in integers
static uint32_t Formula(uint32_t pic, const RimPixel& p)
{
	auto ch = [&](int shift) { return ((((pic >> shift) & 0xff) * p.inv + 127) / 255 + ((p.pre >> shift) & 0xff)); };
	return 0xff000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

static bool Near(uint32_t a, uint32_t b)
{
	for (int shift = 0; shift < 24; shift += 8)
		if (std::abs(int((a >> shift) & 0xff) - int((b >> shift) & 0xff)) > 1)
			return false;
	return (a >> 24) == (b >> 24);
}

static Rect Draw(const std::vector<uint32_t>& src, int w, int h, Scale scale, int shader = 0, double aspect = k43, bool smooth = false)
{
	return DrawFrame(src.data(), w, h, h > 300 ? h / 2 : h, aspect, scale, smooth, false, shader);
}

static void TestBorderFrames()
{
	const uint32_t kPic = 0xff7b7d7bu;
	const uint32_t pic = Surf(kPic);
	const std::vector<uint32_t> f224(320 * 224, kPic), f240(320 * 240, kPic), gg(160 * 144, kPic);
	auto border = MakeBorder(kBorderColor, kWindow, {});

	SetBorder(border);
	InvalidateFrame();
	// first frame: the backdrop, then the picture in the window (Integer: 4x, centred, with window around it)
	Rect r = Draw(f224, 320, 224, Scale::Integer);
	CHECK_RECT(r, 0, 0, kWidth, kHeight); // everything changed
	const Rect picture = {363, 92, 1194, 896};
	CHECK(Same(LastFrameRect(), picture));
	CHECK(Px(100, 100) == kBorderColor); // outside the window
	CHECK(Px(300, 300) == 0xff000000u); // in the window, beside the picture
	CHECK(Px(363, 92) == pic && Px(1556, 987) == pic && Px(960, 540) == pic);
	CHECK(Px(362, 92) == 0xff000000u && Px(363, 91) == 0xff000000u && Px(1557, 987) == 0xff000000u);

	// a normal frame changes nothing but the picture: that is what Present is given
	r = Draw(f224, 320, 224, Scale::Integer);
	CHECK_RECT(r, picture.x, picture.y, picture.w, picture.h);
	Surface()[100 * kWidth + 100] = 0xffabcdefu;
	r = Draw(f224, 320, 224, Scale::Integer);
	CHECK(Px(100, 100) == 0xffabcdefu); // the backdrop is not copied every frame

	// a new picture size copies the backdrop again and reports the whole surface
	r = Draw(f240, 320, 240, Scale::Integer);
	CHECK_RECT(r, 0, 0, kWidth, kHeight);
	CHECK(Px(100, 100) == kBorderColor);
	CHECK(Same(LastFrameRect(), ComputeRectIn(240, k43, Scale::Integer, kWindow)));
	r = Draw(f240, 320, 240, Scale::Integer);
	CHECK(Same(r, LastFrameRect()));

	// the app clears the surface around menus and calls InvalidateFrame(): the border comes back with the picture
	FillRect(0, 0, kWidth, kHeight, Rgb(0, 0, 0));
	InvalidateFrame();
	r = Draw(f240, 320, 240, Scale::Integer);
	CHECK_RECT(r, 0, 0, kWidth, kHeight);
	CHECK(Px(100, 100) == kBorderColor && Px(960, 540) == pic);

	// Fit: a 4:3 picture fills this window
	r = Draw(f224, 320, 224, Scale::Fit);
	CHECK_RECT(r, 0, 0, kWidth, kHeight);
	CHECK(Same(LastFrameRect(), kWindow));
	CHECK(Px(261, 21) == pic && Px(1658, 1058) == pic && Px(260, 21) == kBorderColor && Px(1659, 540) == kBorderColor);
	// the Game Gear's 160x144 picture at 4:3 too
	r = Draw(gg, 160, 144, Scale::Fit);
	CHECK(Same(LastFrameRect(), kWindow) && Px(261, 21) == pic && Px(260, 21) == kBorderColor);

	// another border: repainted even though the rectangle is the same
	auto other = MakeBorder(Rgb(200, 10, 10), kWindow, {});
	SetBorder(other);
	r = Draw(f224, 320, 224, Scale::Fit);
	CHECK_RECT(r, 0, 0, kWidth, kHeight);
	CHECK(Px(100, 100) == Rgb(200, 10, 10));

	// no border: black, and the rectangle of the whole screen, as before borders
	SetBorder(nullptr);
	r = Draw(f224, 320, 224, Scale::Fit);
	CHECK_RECT(r, 0, 0, kWidth, kHeight);
	CHECK_RECT(LastFrameRect(), 240, 0, 1440, 1080);
	CHECK(Px(100, 100) == 0xff000000u && Px(300, 300) == pic && Px(239, 540) == 0xff000000u && Px(240, 0) == pic);
	r = Draw(f224, 320, 224, Scale::Fit);
	CHECK_RECT(r, 240, 0, 1440, 1080);
	Draw(f224, 320, 224, Scale::Integer);
	CHECK_RECT(LastFrameRect(), 363, 92, 1194, 896);

	// a border that does not fit the surface is ignored: no border, no crash
	auto bad = std::make_shared<Border>();
	bad->backdrop.assign(1000, kBorderColor);
	bad->window = kWindow;
	SetBorder(bad);
	r = Draw(f224, 320, 224, Scale::Fit);
	CHECK_RECT(LastFrameRect(), 240, 0, 1440, 1080);
	CHECK(Px(100, 100) == 0xff000000u);
	auto bad_window = std::make_shared<Border>(); // a window that runs off the surface
	bad_window->backdrop.assign(size_t(kWidth) * kHeight, kBorderColor);
	bad_window->window = Rect{1000, 500, 1000, 700};
	SetBorder(bad_window);
	r = Draw(f224, 320, 224, Scale::Fit);
	CHECK_RECT(LastFrameRect(), 240, 0, 1440, 1080);
	SetBorder(nullptr);
}

static void TestRim()
{
	const uint32_t kPic = 0xff7b7d7bu;
	const uint32_t pic = Surf(kPic);
	const std::vector<uint32_t> f224(320 * 224, kPic);
	const Rect picture = {363, 92, 1194, 896}; // Integer, 4x

	// in index order: the picture's top left (opaque, a rounded corner), a soft pixel inside, one beside the picture in
	// the window, one at the bottom right of the picture, one on its right edge column
	std::vector<RimPixel> rim = {
		Rim(363, 92, 9, 8, 7, 255),
		Rim(500, 100, 200, 50, 100, 128),
		Rim(1556, 500, 255, 255, 255, 1),
		Rim(1556, 987, 0, 0, 0, 200),
		Rim(300, 300, 200, 50, 100, 128),
	};
	std::sort(rim.begin(), rim.end(), [](const RimPixel& a, const RimPixel& b) { return a.index < b.index; });
	auto border = MakeBorder(kBorderColor, kWindow, rim);
	SetBorder(border);
	InvalidateFrame();
	Draw(f224, 320, 224, Scale::Integer);
	CHECK(Same(LastFrameRect(), picture));

	CHECK(Px(363, 92) == Rgb(9, 8, 7)); // alpha 255: the border's own colour
	CHECK(Px(500, 100) == Formula(pic, Rim(500, 100, 200, 50, 100, 128))); // the integer formula
	CHECK(Near(Px(500, 100), Exact(pic, 200, 50, 100, 128)));
	CHECK(Near(Px(1556, 500), Exact(pic, 255, 255, 255, 1)));
	CHECK(Near(Px(1556, 987), Exact(pic, 0, 0, 0, 200)));
	CHECK(Px(300, 300) == border->backdrop[300 * kWidth + 300]); // outside the picture: only the backdrop
	CHECK(Px(300, 300) != 0xff000000u);
	CHECK(Px(364, 92) == pic && Px(363, 93) == pic && Px(1555, 987) == pic); // untouched neighbours

	// a second frame draws the picture again and blends the rim over the new pixels, not over the first blend
	Draw(f224, 320, 224, Scale::Integer);
	CHECK(Px(363, 92) == Rgb(9, 8, 7));
	CHECK(Near(Px(500, 100), Exact(pic, 200, 50, 100, 128)));
	const std::vector<uint32_t> green(320 * 224, 0xff00ff00u);
	Draw(green, 320, 224, Scale::Integer);
	CHECK(Near(Px(500, 100), Exact(Surf(0xff00ff00u), 200, 50, 100, 128)));

	// the rim over the smooth picture (bilinear: Fit is not a whole multiple) and over a CPU CRT shader's picture: the picture
	// is drawn first, the rim is blended after it
	for (int pass = 0; pass < 2; pass++)
	{
		const int shader = pass == 0 ? 0 : 1;
		std::vector<RimPixel> one = {Rim(700, 300, 255, 0, 0, 100)};
		SetBorder(MakeBorder(kBorderColor, kWindow, {}));
		Draw(f224, 320, 224, Scale::Fit, shader, k43, true);
		const uint32_t drawn = Px(700, 300);
		SetBorder(MakeBorder(kBorderColor, kWindow, one));
		Draw(f224, 320, 224, Scale::Fit, shader, k43, true);
		CHECK(Near(Px(700, 300), Exact(drawn, 255, 0, 0, 100)));
		CHECK(Px(700, 300) != drawn);
		CHECK(Px(100, 100) == kBorderColor);
	}
	SetBorder(nullptr);
}

// The GPU draws the picture under the surface: there the surface must be clear over the picture but for the rim, which is
// its premultiplied colour with its alpha; the border around it is opaque; the GPU is given the picture's rectangle in the
// window. A shader that draws a bezel of its own (the Mega Bezel) gets the whole screen and no border.
static void TestGpuSurface()
{
	if (!ps5video::GpuActive())
	{
		printf("FAIL the GPU stand-in did not start\n");
		g_fail++;
		return;
	}
	const std::vector<uint32_t> f224(320 * 224, 0xff7b7d7bu);
	std::vector<RimPixel> rim = {Rim(363, 92, 9, 8, 7, 255), Rim(500, 100, 200, 50, 100, 128)};
	SetBorder(MakeBorder(kBorderColor, kWindow, rim));
	InvalidateFrame();
	Rect r = Draw(f224, 320, 224, Scale::Integer, 1);
	CHECK_RECT(r, 0, 0, kWidth, kHeight);
	CHECK(Same(Rect{ps5gpu::g_game.x, ps5gpu::g_game.y, ps5gpu::g_game.w, ps5gpu::g_game.h}, Rect{363, 92, 1194, 896}));
	CHECK(Px(100, 100) == kBorderColor); // the border: opaque
	CHECK(Px(300, 300) == 0xff000000u); // the window beside the picture: opaque black
	CHECK(Px(960, 540) == 0u); // over the picture: clear, the GPU's picture shows
	CHECK(Px(363, 92) == (0xff000000u | Rim(363, 92, 9, 8, 7, 255).pre)); // an opaque rim pixel covers it
	CHECK(Px(500, 100) == (Rim(500, 100, 200, 50, 100, 128).pre | (128u << 24))); // a soft one: premultiplied, alpha 128
	r = Draw(f224, 320, 224, Scale::Integer, 1);
	CHECK_RECT(r, 363, 92, 1194, 896); // a normal frame: only the picture rectangle again
	CHECK(Px(500, 100) == (Rim(500, 100, 200, 50, 100, 128).pre | (128u << 24)) && Px(960, 540) == 0u);
	// the pause menu darkens the surface twice: the border and the rim at a quarter, the GPU's picture behind at a quarter too
	DarkenRect(0, 0, kWidth, kHeight);
	DarkenRect(0, 0, kWidth, kHeight);
	CHECK((Px(100, 100) >> 24) == 0xff && (Px(100, 100) & 0xff) == 2 && ((Px(100, 100) >> 16) & 0xff) == 50);
	CHECK((Px(960, 540) >> 24) == 0xc0); // the picture behind shows at a quarter: 1 - 0xc0/255
	// a shader with its own bezel: the whole screen is the GPU's, no border is drawn
	ps5gpu::g_fullscreen = true;
	r = Draw(f224, 320, 224, Scale::Integer, 2);
	CHECK_RECT(r, 0, 0, kWidth, kHeight);
	CHECK(Px(100, 100) == 0u && Px(960, 540) == 0u);
	CHECK(Same(LastFrameRect(), Rect{363, 92, 1194, 896})); // the picture's place on the bare screen (4x happens to be the same)
	ps5gpu::g_fullscreen = false;
	r = Draw(f224, 320, 224, Scale::Integer, 1);
	CHECK(Px(100, 100) == kBorderColor && Px(960, 540) == 0u); // back with a shader that draws the game only
	SetBorder(nullptr);
	r = Draw(f224, 320, 224, Scale::Integer, 1);
	CHECK(Px(100, 100) == 0xff000000u && Px(960, 540) == 0u);
}

int main()
{
	TestComputeRectIn();
	if (!Init())
	{
		printf("FAIL the host video output did not start\n");
		return 1;
	}
	TestBorderFrames();
	TestRim();
	Shutdown();
	// the same renderer with the GPU (stood in for) drawing the picture
	setenv("GENPLUS_HOST_GPU", "1", 1);
	ps5gpu::g_active = true;
	if (!Init())
	{
		printf("FAIL the host video output did not start for the GPU\n");
		return 1;
	}
	TestGpuSurface();
	Shutdown();
	if (g_fail)
		return 1;
	printf("border render: all checks passed\n");
	return 0;
}

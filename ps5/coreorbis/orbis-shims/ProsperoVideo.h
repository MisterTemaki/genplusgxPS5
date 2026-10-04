// Genesis Plus GX PS5: the picture, through libSceVideoOut (no GPU driver needed).
//
// The frontend draws into a linear 1920x1080 A8B8G8R8 surface (0xAABBGGRR in memory order R,G,B,A).
// Present() swizzles the changed part into one of two tiled scan-out buffers and flips it.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace ps5video
{
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

bool Init();
void Shutdown();

uint32_t* Surface(); // kWidth * kHeight pixels, pitch kWidth

// Copies the rectangle (x, y, w, h) of the surface (everything when w or h is 0) to the back buffer and
// flips it. wait_vsync: return once the flip happened (the frame pacing for 60 Hz games); otherwise the
// next Present waits for it instead, before it touches that buffer again.
void Present(int x, int y, int w, int h, bool wait_vsync);

inline uint32_t Rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return 0xff000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
}

// Fills / darkens part of the surface (clipped).
void FillRect(int x, int y, int w, int h, uint32_t color);
void DarkenRect(int x, int y, int w, int h); // halves every channel

// The game picture: the core's frame converted to 0xAARRGGBB (w x h, pitch w), scaled into the surface.
enum class Scale : int
{
	Fit = 0, // as large as fits at the display aspect ratio
	Integer, // whole multiples of the console's line count (base_h), at the same ratio
	Stretch, // 1920x1080
	Count
};
const char* ScaleName(Scale s);

struct Rect
{
	int x, y, w, h;
};
// aspect: display width / height; base_h: the console's line count before any video filter (the integer
// scale and the scanlines follow it). smooth: bilinear when the scale isn't whole; scanlines: darkens the
// last screen row of every console line, CRT style, when each line gets 3 rows or more. Returns the
// rectangle that changed so the caller can Present just that (everything when the geometry changed).
Rect DrawFrame(const uint32_t* argb, int w, int h, int base_h, double aspect, Scale scale, bool smooth, bool scanlines);
// The last rectangle DrawFrame covered (to darken it for the menu, or to clear around it).
Rect LastFrameRect();
// Forget the cached geometry: the next DrawFrame clears the borders.
void InvalidateFrame();

// Blends a w x h 0xAARRGGBB overlay (a HUD) over the whole surface, scaled.
// row_used (h entries, optional) skips rows with nothing drawn.
void BlendOverlay(const uint32_t* argb, int w, int h, const uint8_t* row_used);
} // namespace ps5video

// Genesis Plus GX PS5: the picture, through libSceVideoOut (no GPU driver needed).
//
// The frontend draws into a linear 1920x1080 A8B8G8R8 surface (0xAABBGGRR in memory order R,G,B,A).
// Present() swizzles the changed part into one of two tiled scan-out buffers and flips it.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace ps5video
{
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

bool Init();
void Shutdown();
// The GPU draws the picture (ProsperoGpu.h): the surface is then premultiplied RGBA laid over it.
bool GpuActive();
// The GPU shader picked is still being prepared (compiled on a thread of its own: the plain picture meanwhile).
bool ShaderBuilding();
// The shaders of the renderer in use: the CPU's (ProsperoCrt.h) or the GPU's (slang presets); 0 is "Off".
int ShaderCount();
const char* ShaderName(int i);

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

// Art around the picture (the border, fe_border.h; Snes9x PS5 2.7's design, pull request #3 by thatboialex). The picture
// goes in the border's window; the border is painted into the surface where the surface is cleared (a new geometry, the
// pause menu's redraw), so a normal frame still changes only the picture rectangle. The CPU draws the rim over the picture
// every frame; for the GPU, the surface over the picture is transparent but for the rim, premultiplied, so the GPU's picture
// (any shader) shows through the window and the rim is laid over it. A shader that draws the whole screen itself (the Mega
// Bezel, ps5gpu::ShaderFullscreen) has a bezel of its own: the border is left out while it is in use.
// One pixel of the border that lies inside the window: drawn over the picture every frame.
struct RimPixel
{
	uint32_t index; // y * kWidth + x
	uint32_t pre; // 0xAABBGGRR with A = 0: each colour already multiplied by alpha / 255
	uint32_t inv; // 255 - alpha
};
struct Border
{
	std::vector<uint32_t> backdrop; // kWidth * kHeight, 0xAABBGGRR, opaque. Window pixels 0xff000000, rim pixels already on black.
	Rect window;
	std::vector<RimPixel> rim; // every pixel inside `window` with alpha 1..255, sorted by index
};
// The border DrawFrame uses from the next frame on; null = none (black around the picture). Cheap: swaps a pointer.
void SetBorder(std::shared_ptr<const Border> border);

// Where the picture goes inside 'bounds' (a border's window): centred, width kept even. Fit takes the aspect ratio by the
// limiting side, or fills 'bounds' when it has that ratio within 3%; Integer is the largest whole multiple of base_h lines
// that fits (all of 'bounds' when it is such a multiple already, as a window fitted for Integer scale is); Stretch fills it.
Rect ComputeRectIn(int base_h, double aspect, Scale scale, const Rect& bounds);
// aspect: display width / height; base_h: the console's line count before any video filter (the integer
// scale and the scanlines follow it). smooth: bilinear when the scale isn't whole; scanlines: darkens the
// last screen row of every console line, CRT style, when each line gets 3 rows or more. Returns the
// rectangle that changed so the caller can Present just that (everything when the geometry changed).
// shader: a ps5crt::Shader (0 = none: the plain scaler with smooth / scanlines); with one, the CRT shader draws the
// picture instead (ProsperoCrt.h).
Rect DrawFrame(const uint32_t* argb, int w, int h, int base_h, double aspect, Scale scale, bool smooth, bool scanlines,
	int shader = 0);
// The shader's average drawing time since the last call, in ms (0 when no shader drew).
double TakeShaderMs();
// The last rectangle DrawFrame covered (to darken it for the menu, or to clear around it).
Rect LastFrameRect();
// Forget the cached geometry: the next DrawFrame clears the borders.
void InvalidateFrame();

// Blends a w x h 0xAARRGGBB overlay (a HUD) over the whole surface, scaled.
// row_used (h entries, optional) skips rows with nothing drawn.
void BlendOverlay(const uint32_t* argb, int w, int h, const uint8_t* row_used);
} // namespace ps5video

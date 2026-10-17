// Genesis Plus GX PS5: the GPU renderer -- Vulkan, through RADV (Mesa's AMD driver, ported to the PS5 by
// PS5_Vulkan / PS5_Mesa, linked into the app).
//
// The CPU still draws the menus, the shelf and the HUD into the 1920x1080 surface (ProsperoVideo.cpp); the GPU
// draws the game's picture through a RetroArch slang shader preset (single- or multi-pass, ProsperoSlang.h) at the
// display's resolution (4K on a 4K TV), and lays the surface over it (premultiplied alpha: where the surface is
// transparent, the game shows). On the host (Linux tests) the same code renders off screen with any Vulkan driver
// (lavapipe) and hands each frame to the host's flip dump.
//
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace ps5gpu
{
struct Rect
{
	int x, y, w, h;
};

// ui_w x ui_h: the CPU surface. offscreen: no display (the host tests), out_w x out_h is the target.
bool Init(int ui_w, int ui_h, bool offscreen, int out_w, int out_h);
bool Active();
void Shutdown();
int OutWidth();
int OutHeight();
const char* DeviceName();

// The GPU's shaders: 0 is the plain picture ("Off"); the others are the built-in slang presets.
int ShaderCount();
const char* ShaderName(int i);
int PassCount(int i);
// The shader draws the whole screen (a bezel around the game: the Mega Bezel), not only the game's rectangle.
bool ShaderFullscreen(int i);

// The game picture of the next Present: the core's frame (0xAARRGGBB, w x h), where it goes on the surface (in
// surface pixels), the shader, and for the plain picture the filter and the scanlines.
void SetGame(const uint32_t* argb, int w, int h, Rect dst, int shader, bool smooth, bool scanlines);
// No game picture (the shelf).
void HideGame();
// The surface (0xAABBGGRR, premultiplied alpha), of which `dirty` changed since the last call, over the game
// picture, on screen. wait_vsync: as ProsperoVideo's Present.
bool Present(const uint32_t* ui, Rect dirty, bool wait_vsync);
// The game passes' GPU time per frame since the last call, in ms (0: not measured).
double TakeShaderMs();
} // namespace ps5gpu

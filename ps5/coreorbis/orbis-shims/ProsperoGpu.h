// Genesis Plus GX PS5: the GPU renderer -- Vulkan, through RADV (Mesa's AMD driver, ported to the PS5 by
// PS5_Vulkan / PS5_Mesa, linked into the app).
//
// The GPU draws the game's picture through a RetroArch slang shader preset (single- or multi-pass, ProsperoSlang.h:
// the built-in ones, compiled at build time, and the user's own .slangp files, compiled on the console) at the
// display's resolution (4K on a 4K TV); on the shelf it draws the backdrop and the covers (SetScene). The CPU draws
// the text and the menus into the 1920x1080 surface (ProsperoVideo.cpp), which the GPU lays over everything
// (premultiplied alpha: where the surface is transparent, the game or the shelf shows). A shader is built on a
// thread of its own (the picture goes on meanwhile) and its pipelines kept in a cache file, so the next time it
// loads at once. On the host (Linux tests) the same code renders off screen with any Vulkan driver (lavapipe) and
// hands each frame to the host's flip dump.
//
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <vector>

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
// A shader is being built (in the background; the picture goes on with the one it had).
bool Building();

// A shader's parameters (its #pragma parameter lines), and the values set in the menu (Settings, Shader parameters).
// They are kept in <root>/shader-params.ini: read at Init, written by SaveShaderParams (the menu, on leaving).
struct ParamInfo
{
	const char* name;
	const char* desc;
	float def, min, max, step, value;
};
int ShaderParamCount(int shader);
bool ShaderParam(int shader, int i, ParamInfo* out);
void SetShaderParam(int shader, const char* name, float value);
void ResetShaderParams(int shader);
bool SaveShaderParams();

// The shelf drawn by the GPU (2.0): its backdrop and the covers as textured quads at the output's resolution (4K
// on a 4K screen), mip-mapped, under the surface (which keeps the text). Coordinates are the surface's (1920x1080);
// w is each corner's distance from the eye, so the textures are mapped in perspective.
enum QuadMode
{
	kQuadImage = 0,      // the texture, times bright, at alpha
	kQuadReflection = 1, // the texture fading out from the top edge (the floor's reflection)
	kQuadGlow = 2,       // a glow of colour around rect (no texture), added
};
struct SceneQuad
{
	uint64_t tex = 0; // a SceneTexture key (0 for a glow)
	float x[4] = {}, y[4] = {}, w[4] = {1, 1, 1, 1}; // top-left, top-right, bottom-left, bottom-right
	float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
	float bright = 1, alpha = 1;
	int mode = kQuadImage;
	float rect[4] = {}; // glow: the rectangle it surrounds (l, t, r, b)
	float glow_r = 0;
	float color[3] = {}; // glow colour, 0..1
};
// The scene can be drawn (the GPU is on and its quad shader compiled).
bool SceneAvailable();
// A texture for the scene, kept by `key` (uploaded on the first call, with its mip levels): pixels in the
// surface's format, row-major or column-major (px[x * h + y], as the shelf's covers are). False: it could not be.
bool SceneTexture(uint64_t key, const uint32_t* px, int w, int h, bool column_major);
bool HasSceneTexture(uint64_t key);
// The quads drawn under the surface at every Present from now on (in order), until ClearScene.
void SetScene(const std::vector<SceneQuad>& quads);
void ClearScene();
} // namespace ps5gpu

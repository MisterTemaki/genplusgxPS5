// Genesis Plus GX PS5: RetroArch slang shader presets, compiled at build time (ps5/tools/slang_compile.py) into
// these tables, and run on the GPU by ProsperoGpu.cpp.
//
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ps5slang
{
// a member of a pass's uniform block or push constants: a semantic (MVP, OutputSize...) or a parameter
struct Member
{
	const char* name;
	int offset;
	int kind; // 0 float, 1 int, 2 uint, 3 vec2, 4 vec3, 5 vec4, 6 mat4, 7 other
};

// a texture the pass samples: Source, Original, PassOutput1, an alias, a LUT...
struct Sampler
{
	const char* name;
	int binding;
};

struct Pass
{
	const uint32_t* vs;
	unsigned vs_bytes;
	const uint32_t* fs;
	unsigned fs_bytes;
	const Member* ubo;
	int ubo_count;
	int ubo_size;
	int ubo_binding; // -1: no uniform block
	const Member* push;
	int push_count;
	int push_size;
	const Sampler* samplers;
	int sampler_count;
	const char* alias;
	int framebuffer; // 0 8-bit UNORM, 1 sRGB, 2 16-bit float
	int scale_type_x, scale_type_y; // 0 source, 1 viewport, 2 absolute
	float scale_x, scale_y;
	bool linear;
	int wrap; // 0 border, 1 edge, 2 repeat, 3 mirrored repeat
	int frame_count_mod;
	bool mipmap_input;
};

struct Param
{
	const char* name;
	const char* desc;
	float value, min, max, step;
};

struct Lut
{
	const char* name;
	const uint8_t* png;
	unsigned png_bytes;
	bool linear;
	int wrap;
	bool mipmap;
};

struct Preset
{
	const char* name;
	const Pass* passes;
	int pass_count;
	const Param* params;
	int param_count;
	const Lut* luts;
	int lut_count;
	bool fullscreen; // draws on the whole screen (a bezel around the game): the viewport is the screen, not the game
};

// the presets compiled at build time (tools/slang_compile.py): [0] the plain picture, [1] the menus' surface
extern const Preset kBuiltinPresets[];
extern const int kBuiltinPresetCount;

// All the presets: the built-in ones, then the user's own from <root>/shaders (ProsperoSlangExt.cpp), which are
// compiled on the console the first time they are used (EnsureCompiled: on the GPU renderer's build thread). Until
// then a user preset is its name only (no passes, no parameters).
void ScanExternal();
// GLSL (Vulkan) to SPIR-V with the same compiler: the GPU renderer's own small shaders (the shelf's quads)
bool CompileGlsl(const std::string& src, bool vertex, std::vector<uint32_t>& spv, std::string& err);
std::string ExternalDir();
int PresetCount();
const Preset& PresetAt(int i);
bool IsExternal(int i);
bool EnsureCompiled(int i, std::string* err);
#ifndef __PROSPERO__
void CheckAllExternal(); // host tests only
#endif
} // namespace ps5slang

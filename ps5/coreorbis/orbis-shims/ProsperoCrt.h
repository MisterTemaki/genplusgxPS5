// Genesis Plus GX PS5: CRT shaders, run on the CPU.
//
// The PS5 build has no GPU driver (the picture is drawn by the CPU, ProsperoVideo.cpp), so RetroArch's slang
// shaders can't run as they are. These are CPU versions of the single-pass shaders of libretro's slang-shaders
// collection (crt/) whose licences allow it -- public domain (CC0 / Unlicense) and MIT -- plus an original
// "Easymode style" shader (the default) that gives the look of EasyMode's crt-easymode without its code (that
// one is GPL, which Genesis Plus GX's licence can't take in).
//
// Each one is split the way a CPU likes it: what depends on a source line and a screen column (the horizontal
// filter) is computed once per source line, what depends on a screen row once per row, and only the vertical
// blend, the beam, the mask and the gamma (a table) are done per screen pixel. Curved screens use a per-pixel
// map built once per picture size. The work is split across a few threads.
//
// SPDX-License-Identifier: MIT (this file's own code); the ported shaders keep their licences, see
// ProsperoCrt.cpp and THIRD_PARTY_SHADERS.md.
#pragma once
#include <cstdint>

namespace ps5crt
{
enum class Shader : int
{
	Off = 0,
	EasymodeStyle, // the default: flat CRT, Lanczos horizontal, beam scanlines, aperture grille
	Lottes, // crt-lottes (Timothy Lottes, public domain)
	LottesFast, // crt-lottes-fast (Timothy Lottes, public domain)
	OneTap, // crt-1tap (fishku, CC0)
	TwoTap, // crt-2tap (fishku, CC0)
	HyllianFast, // crt-hyllian-fast (Hyllian, MIT)
	Nobody, // crt-nobody (Hyllian, MIT)
	NewpixieMini, // newpixie-mini (Mattias Gustavsson, MIT / Unlicense)
	BlurPiSharp, // crt-blurPi-sharp (Oriol Ferrer Mesia, MIT)
	BlurPiSoft, // crt-blurPi-soft (Oriol Ferrer Mesia, MIT)
	MonoCrt, // monoCRT (hunterk, public domain)
	Count
};

const char* Name(Shader s); // "CRT Easymode style", "crt-lottes"...

// Draws the w x h 0xAARRGGBB frame through the shader into the (dx, dy, dw, dh) rectangle of a 0xAABBGGRR surface
// (pitch in pixels). frame: a counter of the frames shown (for shaders that move).
void Render(Shader s, const uint32_t* argb, int w, int h, uint32_t* surface, int pitch, int dx, int dy, int dw, int dh,
	uint64_t frame);
} // namespace ps5crt

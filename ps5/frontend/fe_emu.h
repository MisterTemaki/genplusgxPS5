// Genesis Plus GX PS5 frontend: the Genesis Plus GX core behind a small API.
//
// The core is driven through its libretro interface (libretro/libretro.c, the most complete of its ports): the
// frontend is the libretro "frontend" -- it answers the core's environment calls (folders, options, log), gets
// each frame (RGB565) and its sound (44.1 kHz) through callbacks, and reports the pads. One retro_run() is one
// frame, called from the frontend's thread, as Snes9x PS5 runs Snes9x.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace emu
{
// The choices the settings menu offers.
struct Choice
{
	const char* name;
	const char* value; // the core option's value ("" where the frontend handles it)
};
extern const Choice kAspects[]; // the core's TV aspect, square pixels, 4:3, 16:9
extern const int kAspectCount;
extern const Choice kNtscFilters[]; // genesis_plus_gx_blargg_ntsc_filter
extern const int kNtscFilterCount;
extern const Choice kBorders[]; // genesis_plus_gx_overscan
extern const int kBorderCount;
extern const Choice kFmChips[]; // genesis_plus_gx_ym2612
extern const int kFmChipCount;
extern const Choice kRegions[]; // genesis_plus_gx_region_detect
extern const int kRegionCount;

bool InitCore();
void DeinitCore();

// Starts the game. On failure, *error says why (a missing Sega CD BIOS names the files and where they go).
bool LoadGame(const std::string& path, std::string* error);
void CloseGame(); // writes the battery save and ends the game
bool GameLoaded();
std::string GameName(); // the file name without its extension
std::string SystemName(); // "Mega Drive", "Sega CD"... of the running game

// Settings from fe::Config() -> the core's options and the frontend's picture/sound settings.
void ApplySettings();

enum class FrameResult
{
	Continue,
	OpenMenu, // L3 + R3 pressed together
	Stopped, // no game
};
// One frame: the pads and hot keys, retro_run(), the picture (and the messages) on the screen, the sound to
// the audio ring, paced by the display (60 Hz games) or by the sound (50 Hz games). Hot keys: L2 + Up / Down =
// save / load the state slot, L2 + Left / Right = change the slot, R2 held = fast forward, L2 + R2 held =
// rewind.
FrameResult RunFrame();

void Pause(); // nothing runs between frames; kept for the menus' symmetry
void Resume();

bool SaveState(int slot); // 1..10, /data/genplus/states/<game>.state<slot>
bool LoadState(int slot);
bool StateExists(int slot);
void Reset();
void PowerCycle(); // the game loaded again, as switching the console off and on
void Osd(const std::string& text); // a message over the game for a few seconds

// Redraws the last frame into the surface (the pause menu draws over it).
void RedrawLastFrame();
} // namespace emu

// Genesis Plus GX PS5 frontend: settings kept in /data/genplus/genplus-ps5.ini (key=value lines).
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace fe
{
struct Settings
{
	// video
	int scale = 0; // ps5video::Scale: fit, integer, stretch
	int aspect = 0; // emu::kAspects: the core's (TV), square pixels, 4:3, 16:9
	int ntsc = 0; // emu::kNtscFilters: off, composite, S-Video, RGB, monochrome (Blargg's NTSC filter)
	int borders = 0; // emu::kBorders: off, top/bottom, left/right, full (the core's overscan option)
	bool gg_lcd = true; // Game Gear LCD ghosting
	bool smooth = false; // bilinear when the scale isn't whole
	bool scanlines = false;
	bool show_fps = false;
	// audio
	bool audio = true;
	int volume = 100; // 0..100
	int fm_chip = 0; // emu::kFmChips
	bool lowpass = false; // the core's low-pass audio filter
	// emulation
	int ff_speed = 300; // % while R2 is held; 0 = as fast as possible
	bool rewind = true; // L2 + R2 rewinds
	int region = 0; // emu::kRegions: auto, USA, Europe, Japan
	bool no_sprite_limit = false;
	int state_slot = 1; // 1..10
	// controls
	int pad_type = 0; // emu::kPadTypes: auto (the game's header), 3 buttons, 6 buttons (Mega Drive, Sega CD)
	int multitap = 0; // emu::kMultitaps: off, 4 Way Play, Team Player
	// the PS5 button (index in emu::kPs5Buttons) of each console button, in emu::kConsoleButtons' order:
	// A, B, C, X, Y, Z, Start, Mode. By position by default: Square, Cross, Circle, L1, Triangle, R1, OPTIONS, touchpad.
	static constexpr int kButtons = 8;
	static constexpr int kDefaultButtons[kButtons] = {2, 0, 1, 4, 3, 5, 6, 7};
	int buttons[kButtons] = {2, 0, 1, 4, 3, 5, 6, 7};
	void DefaultButtons();
	// library
	bool covers_download = true; // fetch box art from libretro-thumbnails
	int shelf_family = 0; // fe::Family the shelf shows
	std::string last_rom; // the shelf puts the selection on this game

	void Load();
	void Save() const;
};

Settings& Config();
} // namespace fe

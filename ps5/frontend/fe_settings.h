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
	int shader = 1; // ps5crt::Shader: 0 off, 1 "CRT Easymode style" (the default for every game), ...
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
	int add_on = 0; // emu::kAddOns: the CD add-on of a cartridge game with a disc beside it (MD+, MSU-MD)
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
	int language = 0;            // fe::Language: 0 English (the default), 1 Spanish, 2 Portuguese (Brazil), 3 French, 4 German
	int renderer = 1;            // 1 GPU (the default when the build has one), 0 CPU; read by ProsperoVideo at start
	bool perf_log = false;       // logs/perf-<system>-<game>.csv, a row a second in a game (fe_emu.cpp, Perf)
	bool debug_logs = true;      // boot.log and the others in /data/genplus/logs (OrbisLogSetEnabled)
	int shelf_family = 0; // fe::Family the shelf shows
	std::string last_rom; // the shelf puts the selection on this game

	void Load();
	// Writes the settings: the global file, and -- while a game with its own settings runs -- that game's file
	// (the game's keys go there, the global file keeps the global ones).
	void Save() const;

	// The settings parser and writer, for any file. game_keys_only: only the keys a game can have of its own.
	bool LoadFrom(const std::string& path);
	bool SaveTo(const std::string& path, bool game_keys_only) const;
};

Settings& Config();

// Per-game settings (/data/genplus/games/<system>/<game>.ini): the picture, sound, emulation and control settings a
// game can keep of its own; the library, logs, fast forward and rewind stay global.
// BeginGame: when the game has its own file, its settings replace the global ones until EndGame.
void BeginGame(const std::string& game_ini);
void EndGame();
bool GameHasOwnSettings();
// Makes the running game use its own settings (starting from the current ones), or the global ones again (its
// file removed). False when no game runs or the file can't be written.
bool SetGameOwnSettings(bool own);
} // namespace fe

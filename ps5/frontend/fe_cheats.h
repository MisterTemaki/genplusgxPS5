// Genesis Plus GX PS5 frontend: the running game's cheats (Snes9x PS5's cheat manager, by thatboialex, and its
// 2.7 reload when the file changes).
//
// Where they come from: libretro cheat files (.cht, fe_chtfile.h) in /data/genplus/cheats:
//   1. cheats/<system>/<game>.cht (<system> = MegaDrive, SegaCD, MasterSystem, GameGear, SG1000; <game> = the ROM's
//      file name): the game's own file -- the cheat manager writes it when a cheat is switched on or off;
//   2. otherwise the best match by name (fe::cht::Best: the ROM's name, or its No-Intro / Redump name) among the files
//      in cheats/<system>/, cheats/, cheats/<libretro-database folder>/ ("Sega - Mega Drive - Genesis", "Sega -
//      Mega-CD - Sega CD", "Sega - Master System - Mark III", "Sega - Game Gear", "Sega - SG-1000": copy the
//      database's folders as they are) and the ROM's own folder (not on a network share).
// The codes go to Genesis Plus GX through its libretro port (retro_cheat_reset / retro_cheat_set), which decodes
// and applies them: Game Genie and Action Replay codes, ROM patches (taken back when the cheat is switched off) and
// RAM values written every frame, Sega CD PRG-RAM / Word-RAM included. A code the core can't read
// (fe::cheatcode::Decode) makes its cheat unusable: it isn't listed, and stays in the file as it was.
//
// The file is looked at again (a stat, once a second) while the game runs and while the cheat manager is open: a
// cheat added over FTP is applied with no restart. A file still being copied is read once it looks the same twice;
// a file with no usable cheat leaves the running ones alone.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_games.h"

#include <string>

namespace fe
{
namespace cheats
{
// emu::LoadGame, once the core runs the game: finds its cheat file and applies the cheats that are on.
void GameStarted(const std::string& path, const std::string& name, System system);
// emu::CloseGame, before the core unloads the game: a switch that couldn't be saved is saved, the cheats are taken out.
void GameClosing();
// Once a frame: once a second, a changed file replaces the cheats (and says so on the screen).
void Frame();

int Count(); // the usable cheats
std::string Name(int index);
bool Enabled(int index);
bool Toggle(int index); // switches one on or off; false if that couldn't be saved
void SetAll(bool enabled);
// Reads the file again when it changed; true when the list changed. now: at once (the cheat manager just opened);
// otherwise at most once a second. Without cheats, the folders are searched again too.
bool Refresh(bool now);
std::string Status(); // which file was found, or why there are no cheats (in the interface's language)
} // namespace cheats
} // namespace fe

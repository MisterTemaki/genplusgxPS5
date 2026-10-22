// Genesis Plus GX PS5 frontend: the borders downloaded from The Bezel Project, as Snes9x PS5 2.7 does it (pull request #3 by
// thatboialex).
//
// The app ships the names of the borders The Bezel Project has for each system Genesis Plus GX runs (data/border-index.txt,
// tools/make_border_index.py: its MegaDrive, SegaCD, MasterSystem, GameGear and SG-1000 repositories), never the pictures. A
// game's border is the name the cover matcher picks for it among its system's names ("Sonic The Hedgehog (USA, Europe)"); when
// two games fit a name alike, the USA one of two releases of one game is taken. The helper payload downloads the picture, as
// it does the covers (fe_coverworker.h; the sandboxed app cannot): the app writes borders/priority.txt (the game being played)
// or borders/wanted.txt (Settings, Download all borders), and the file lands in borders/<system folder>/ under The Bezel
// Project's own name, so the pack finds it like any hand-copied border (fe_border.h). A 404 leaves <name>.missing.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_coverfetch.h"
#include "fe_games.h"

#include <string>
#include <vector>

namespace fe::border
{
// The names (no extension) of the borders The Bezel Project has for system s, in file order.
const std::vector<std::string>& IndexNames(System s);

// The Bezel Project's name for this game's border (the cover matcher over its system's IndexNames), "" when it has none.
std::string BezelNameFor(const GameInfo& game);

// The picture's download address, in the repository of system s; GENPLUS_BORDER_URL (a folder address ending in "/")
// replaces the base for the host tests.
std::string BezelUrl(System s, const std::string& name);

// The download this game's border needs ("<system folder>/<name>.png" and its address): false when The Bezel Project has
// none for it, the picture is already in `pack` (the borders folder), or a 404 was seen in the last 30 days.
bool BorderWanted(const std::string& pack, const GameInfo& game, WantedCover* out);

// The downloads the library still needs, one per bezel (several ROMs of a game share it); a game with its own
// <system folder>/<ROM name>.png is left out.
std::vector<WantedCover> MissingBorders(const std::string& pack, const std::vector<GameInfo>& games);

// borders/wanted.txt (the library, the helper works through it last) and borders/priority.txt (the game being played, first).
void WriteWantedBorders(const std::string& pack, const std::vector<WantedCover>& wanted);
void WritePriorityBorders(const std::string& pack, const std::vector<WantedCover>& wanted);
} // namespace fe::border

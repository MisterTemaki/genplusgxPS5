// Genesis Plus GX PS5 frontend: the border around the picture (art in /data/genplus/borders, see ps5video::Border).
// Snes9x PS5 2.7's borders (pull request #3 by thatboialex), for the systems Genesis Plus GX runs.
//
// The pack, in /data/genplus/borders:
//   default.png               optional: for a game with no border of its own (any system)
//   default.ini               optional: its settings
//   <system>/default.png      optional: the same, for one system's games (<system>: MegaDrive, SegaCD, MasterSystem,
//                             GameGear, SG1000, the folders of roms/ and covers/); <system>/default.ini its settings
//   <system>/<name>.png       a border for one game; The Bezel Project's bezels (GameBezels/<system>/*.png) work as they are,
//                             and the ones the app downloads land here under The Bezel Project's own name
//   <system>/<name>.ini       optional, same name; keys: window=x,y,w,h (at least 480x270; use this window instead of
//                             finding it, and clear the picture inside it), off=1 (this game has no border even in Auto)
// A PNG is 1920x1080, 8 bits per channel, RGBA or palette with tRNS (or RGB with a window=, as it has no clear area of its
// own), under 8 MiB; its window is the clear area around the centre (a Game Gear bezel's is the handheld's screen).
//
// Setting Auto picks, first hit wins (a file that is there but unusable is logged and skipped):
//   1. <system>/<ROM file name>.ini says off=1                    -> no border
//   2. <system>/<ROM file name>.png
//   3. the cover matcher over the names in <system>/ (renamed and translated ROMs find their game's border)
//   4. The Bezel Project's name for the game, when that file is in <system>/ (a download from before)
//   5. <system>/default.png, then default.png
//   6. the frame built into the app
// Default frame uses 5 and 6 only; Off sets none. Auto with no border of the game's own also asks the helper to download
// The Bezel Project's (fe_borderfetch.h), and puts it up while the game runs once it has landed.
//
// The window follows the picture: the art is refitted so the window has the picture's shape (its aspect ratio; 16:9 for
// Stretch to 16:9) or, for Integer scale, the size of the whole-multiple picture, with the side art scaled evenly and
// cropped, not squeezed (FitToShape in fe_borderimg.h). A Game Gear or Master System picture gets its own shape.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_borderimg.h"

#include "ProsperoVideo.h"

#include <cstdint>
#include <memory>
#include <string>

namespace fe
{
struct GameInfo;
}

namespace fe::border
{
// Picks and loads the border for the game that was just loaded (its ROM path), fitted to `shape`, or clears it when the
// setting is Off. Calls ps5video::SetBorder. Never throws, never fails the game: a problem is one OrbisLog line.
void Select(const std::string& rom_path, const Shape& shape);
// The Border setting or the picture's shape changed while a game is loaded: re-pick (and re-fit) for the same game. A
// no-op with no game, and while the setting and the shape are the ones already applied (fe_emu calls it every frame and
// at every settings change: it compares a few numbers). A shape within half a percent of the last is the same shape.
void Reselect(const Shape& shape);
// The game was closed.
void Release();

// The choice itself, without the renderer, for the tests.
struct Pick
{
	std::shared_ptr<const ps5video::Border> border; // null: none
	std::string source; // "own file", "matched", "bezel name", "system default.png", "default.png", "built-in", "off", ""
	std::string file; // the picture's file name inside the pack ("MegaDrive/Sonic The Hedgehog (USA, Europe).png")
};
// pack: the borders folder; setting: 1 Auto or 2 Default frame (0 is the caller's: no border); game: named as ScanGames does;
// shape: what the window is fitted to (the default, not valid, keeps the art as drawn).
Pick Choose(const std::string& pack, int setting, const GameInfo& game, const Shape& shape = Shape());
} // namespace fe::border

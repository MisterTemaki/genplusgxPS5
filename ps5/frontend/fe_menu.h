// Genesis Plus GX PS5 frontend: the menus, drawn on the 1920x1080 surface.
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace fe
{
enum class PauseAction
{
	Resume,
	BackToList,
	Quit,
};
// The in-game menu (L3 + R3), over the paused picture.
PauseAction PauseMenu();

// The settings screen (Triangle on the shelf, or from the pause menu).
void SettingsMenu();

// Settings > Network shares: the SMB and NFS shares games are read from (fe_netscreen.cpp, from Snes9x PS5).
void NetworkMenu();

// Shows a message box until Cross / Circle is pressed (errors such as a game that won't start).
void MessageBox(const std::string& title, const std::string& text);
} // namespace fe

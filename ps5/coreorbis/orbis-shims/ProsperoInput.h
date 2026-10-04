// Genesis Plus GX PS5: DualSense / DualShock input through libScePad (up to 4 players).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace ps5input
{
constexpr int kMaxPads = 4;

struct PadState
{
	bool connected = false;
	uint32_t buttons = 0; // SCE_PAD_BUTTON_*; the left stick also sets the D-pad bits
	uint32_t raw_buttons = 0; // without the stick
	uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
};

bool Init();
void Shutdown();

// Reads every pad (thread-safe). Player 1 is the foreground user; the other logged-in users follow in the system's
// order. Who is logged in is checked again every couple of seconds.
void Poll();
const PadState& Pad(int player); // the polling thread's view
PadState Snapshot(int player); // a copy, safe from any thread (Poll may run on another one)
int ConnectedCount();

// Player 1's buttons that went down in the last Poll (menus).
uint32_t Pressed();
} // namespace ps5input

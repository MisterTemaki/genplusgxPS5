// Genesis Plus GX PS5 frontend: settings file.
// SPDX-License-Identifier: MIT

#include "fe_settings.h"

#include "OrbisPaths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fe
{
namespace
{
std::string IniPath()
{
	return OrbisRoot() + "/genplus-ps5.ini";
}

int Clamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

const char* const kButtonKeys[Settings::kButtons] = {"btn_a", "btn_b", "btn_c", "btn_x", "btn_y", "btn_z", "btn_start",
	"btn_mode"};
constexpr int kPs5ButtonChoices = 9; // emu::kPs5ButtonCount
} // namespace

constexpr int Settings::kDefaultButtons[Settings::kButtons];

void Settings::DefaultButtons()
{
	for (int i = 0; i < kButtons; i++)
		buttons[i] = kDefaultButtons[i];
}

Settings& Config()
{
	static Settings s;
	return s;
}

void Settings::Load()
{
	FILE* f = fopen(IniPath().c_str(), "r");
	if (!f)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		char* eq = strchr(line, '=');
		if (!eq || line[0] == '#' || line[0] == ';')
			continue;
		*eq = 0;
		const std::string key = line;
		const char* val = eq + 1;
		const int n = atoi(val);
		if (key == "scale")
			scale = Clamp(n, 0, 2);
		else if (key == "aspect")
			aspect = Clamp(n, 0, 3);
		else if (key == "ntsc")
			ntsc = Clamp(n, 0, 4);
		else if (key == "borders")
			borders = Clamp(n, 0, 3);
		else if (key == "gg_lcd")
			gg_lcd = n != 0;
		else if (key == "smooth")
			smooth = n != 0;
		else if (key == "scanlines")
			scanlines = n != 0;
		else if (key == "show_fps")
			show_fps = n != 0;
		else if (key == "audio")
			audio = n != 0;
		else if (key == "volume")
			volume = Clamp(n, 0, 100);
		else if (key == "fm_chip")
			fm_chip = Clamp(n, 0, 3);
		else if (key == "lowpass")
			lowpass = n != 0;
		else if (key == "ff_speed")
			ff_speed = n == 0 ? 0 : Clamp(n, 150, 1000);
		else if (key == "rewind")
			rewind = n != 0;
		else if (key == "region")
			region = Clamp(n, 0, 3);
		else if (key == "no_sprite_limit")
			no_sprite_limit = n != 0;
		else if (key == "pad_type")
			pad_type = Clamp(n, 0, 2);
		else if (key == "multitap")
			multitap = Clamp(n, 0, 2);
		else if (key.compare(0, 4, "btn_") == 0)
		{
			for (int i = 0; i < kButtons; i++)
				if (key == kButtonKeys[i])
					buttons[i] = Clamp(n, 0, kPs5ButtonChoices - 1);
		}
		else if (key == "state_slot")
			state_slot = Clamp(n, 1, 10);
		else if (key == "covers_download")
			covers_download = n != 0;
		else if (key == "shelf_family")
			shelf_family = Clamp(n, 0, 64);
		else if (key == "last_rom")
			last_rom = val;
	}
	fclose(f);
	OrbisLog("[settings] loaded %s", IniPath().c_str());
}

void Settings::Save() const
{
	const std::string tmp = IniPath() + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
	{
		OrbisLog("[settings] can't write %s", tmp.c_str());
		return;
	}
	fprintf(f, "# Genesis Plus GX PS5\n");
	fprintf(f, "scale=%d\n", scale);
	fprintf(f, "aspect=%d\n", aspect);
	fprintf(f, "ntsc=%d\n", ntsc);
	fprintf(f, "borders=%d\n", borders);
	fprintf(f, "gg_lcd=%d\n", gg_lcd ? 1 : 0);
	fprintf(f, "smooth=%d\n", smooth ? 1 : 0);
	fprintf(f, "scanlines=%d\n", scanlines ? 1 : 0);
	fprintf(f, "show_fps=%d\n", show_fps ? 1 : 0);
	fprintf(f, "audio=%d\n", audio ? 1 : 0);
	fprintf(f, "volume=%d\n", volume);
	fprintf(f, "fm_chip=%d\n", fm_chip);
	fprintf(f, "lowpass=%d\n", lowpass ? 1 : 0);
	fprintf(f, "ff_speed=%d\n", ff_speed);
	fprintf(f, "rewind=%d\n", rewind ? 1 : 0);
	fprintf(f, "region=%d\n", region);
	fprintf(f, "no_sprite_limit=%d\n", no_sprite_limit ? 1 : 0);
	fprintf(f, "pad_type=%d\n", pad_type);
	fprintf(f, "multitap=%d\n", multitap);
	for (int i = 0; i < kButtons; i++)
		fprintf(f, "%s=%d\n", kButtonKeys[i], buttons[i]);
	fprintf(f, "state_slot=%d\n", state_slot);
	fprintf(f, "covers_download=%d\n", covers_download ? 1 : 0);
	fprintf(f, "shelf_family=%d\n", shelf_family);
	fprintf(f, "last_rom=%s\n", last_rom.c_str());
	fclose(f);
	rename(tmp.c_str(), IniPath().c_str());
}
} // namespace fe

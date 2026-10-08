// Genesis Plus GX PS5 frontend: the menus.
//
// Everything is drawn on the CPU into the 1920x1080 surface (fe_text: Roboto + PromptFont) and shown with
// ps5video::Present, which waits for vsync, so the menus run at 60 Hz.
//
// SPDX-License-Identifier: MIT

#include "fe_menu.h"

#include "fe_emu.h"
#include "fe_settings.h"
#include "fe_text.h"

#include "OrbisPaths.h"
#include "ProsperoCrt.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "ProsperoVideo.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace fe
{
namespace
{
using ps5video::Rgb;
constexpr int W = ps5video::kWidth;
constexpr int H = ps5video::kHeight;

const uint32_t kBg = Rgb(14, 14, 28);
const uint32_t kPanel = Rgb(26, 24, 48);
const uint32_t kAccent = Rgb(150, 125, 255);
const uint32_t kSel = Rgb(78, 62, 170);
const uint32_t kText = Rgb(235, 235, 245);
const uint32_t kDim = Rgb(150, 150, 175);

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ---- input with key repeat ---------------------------------------------------------------------------
struct Nav
{
	bool up = false, down = false, left = false, right = false;
	bool ok = false, back = false, options = false, menu = false;
};

class NavReader
{
public:
	NavReader()
	{
		ps5input::Poll();
		m_prev = ps5input::Pad(0).buttons; // buttons still held from before don't count
	}
	Nav Read()
	{
		ps5input::Poll();
		const uint32_t cur = ps5input::Pad(0).buttons;
		const uint32_t down = cur & ~m_prev;
		m_prev = cur;
		const double now = Now();
		Nav n;
		n.up = Repeat(cur, down, SCE_PAD_BUTTON_UP, now, 0);
		n.down = Repeat(cur, down, SCE_PAD_BUTTON_DOWN, now, 1);
		n.left = Repeat(cur, down, SCE_PAD_BUTTON_LEFT, now, 2);
		n.right = Repeat(cur, down, SCE_PAD_BUTTON_RIGHT, now, 3);
		n.ok = down & SCE_PAD_BUTTON_CROSS;
		n.back = down & SCE_PAD_BUTTON_CIRCLE;
		n.options = down & SCE_PAD_BUTTON_OPTIONS;
		const uint32_t combo = SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3;
		n.menu = (cur & combo) == combo && (down & combo);
		return n;
	}

private:
	bool Repeat(uint32_t cur, uint32_t down, uint32_t bit, double now, int i)
	{
		if (down & bit)
		{
			m_next[i] = now + 0.40;
			return true;
		}
		if ((cur & bit) && now >= m_next[i])
		{
			m_next[i] = now + 0.07;
			return true;
		}
		return false;
	}
	uint32_t m_prev = 0;
	double m_next[4] = {};
};

void Present()
{
	ps5video::Present(0, 0, 0, 0, true);
}

void Header(const char* subtitle)
{
	ps5video::FillRect(0, 0, W, H, kBg);
	ps5video::FillRect(0, 0, W, 150, kPanel);
	ps5video::FillRect(0, 150, W, 4, kAccent);
	DrawText(80, 34, "Genesis Plus GX PS5", 6, kAccent);
	const char* ver = "Genesis Plus GX " GENPLUS_CORE_VERSION " - PS5 " GENPLUS_PS5_VERSION;
	DrawText(W - 80 - TextWidth(ver, 2), 40, ver, 2, kDim);
	if (subtitle && *subtitle)
		DrawText(84, 104, FitText(subtitle, 3, W - 168).c_str(), 3, kDim);
}

void Footer(const std::string& help)
{
	ps5video::FillRect(0, H - 80, W, 80, kPanel);
	DrawText(80, H - 60, help.c_str(), 3, kDim);
}

// A box of label/value rows (pause menu, settings); a row with header=true is a section title.
struct Row
{
	std::string label;
	std::string value; // "" = an action
	bool enabled = true;
	bool header = false;
};

void DrawOptionBox(const std::string& title, const std::vector<Row>& rows, int sel, bool over_game)
{
	const int scale = 3;
	const int row_h = 46;
	const int visible = std::min(int(rows.size()), 16);
	const int bw = 1180;
	const int bh = 120 + visible * row_h + 30;
	const int bx = (W - bw) / 2;
	const int by = std::max(20, (H - bh) / 2);
	int top = 0;
	if (int(rows.size()) > visible)
		top = std::max(0, std::min(int(rows.size()) - visible, sel - visible / 2));
	if (!over_game)
		ps5video::FillRect(bx - 4, by - 4, bw + 8, bh + 8, kAccent);
	ps5video::FillRect(bx, by, bw, bh, kPanel);
	ps5video::FillRect(bx, by + 86, bw, 3, kAccent);
	DrawText(bx + 40, by + 24, FitText(title, 5, bw - 80).c_str(), 5, kAccent);
	for (int i = 0; i < visible; i++)
	{
		const Row& row = rows[size_t(top + i)];
		const int y = by + 110 + i * row_h;
		if (row.header)
		{
			DrawText(bx + 40, y + 6, row.label.c_str(), 2, kAccent);
			ps5video::FillRect(bx + 40, y + 34, bw - 80, 1, kSel);
			continue;
		}
		if (top + i == sel)
			ps5video::FillRect(bx + 16, y - 6, bw - 32, row_h, kSel);
		const uint32_t col = row.enabled ? kText : kDim;
		DrawText(bx + 40, y, row.label.c_str(), scale, col);
		if (!row.value.empty())
		{
			const std::string v = (top + i == sel ? "< " + row.value + " >" : row.value);
			DrawText(bx + bw - 40 - TextWidth(v.c_str(), scale), y, v.c_str(), scale, top + i == sel ? kText : kAccent);
		}
	}
	if (int(rows.size()) > visible)
	{
		const int track = visible * row_h;
		const int thumb = std::max(30, track * visible / int(rows.size()));
		const int ty = by + 104 + (track - thumb) * top / std::max(1, int(rows.size()) - visible);
		ps5video::FillRect(bx + bw - 12, ty, 6, thumb, kAccent);
	}
}

int Step(int sel, int dir, const std::vector<Row>& rows)
{
	const int n = int(rows.size());
	for (int i = 0; i < n; i++)
	{
		sel = (sel + dir + n) % n;
		if (!rows[size_t(sel)].header)
			return sel;
	}
	return sel;
}

const char* OnOff(bool b)
{
	return b ? "On" : "Off";
}

// ---- settings ----------------------------------------------------------------------------------------
enum SettingRow
{
	H_VIDEO,
	S_SHADER,
	S_SCALE,
	S_ASPECT,
	S_NTSC,
	S_BORDERS,
	S_SMOOTH,
	S_SCANLINES,
	S_GGLCD,
	S_FPS,
	H_AUDIO,
	S_AUDIO,
	S_VOLUME,
	S_FMCHIP,
	S_LOWPASS,
	H_EMU,
	S_FF,
	S_REWIND,
	S_REGION,
	S_SPRITES,
	H_CONTROLS,
	S_PADTYPE,
	S_MULTITAP,
	S_BTN_FIRST,
	S_BTN_LAST = S_BTN_FIRST + emu::kConsoleButtonCount - 1,
	S_BTN_DEFAULT,
	H_LIBRARY,
	S_COVERS,
	H_SYSTEM,
	S_DEBUGLOGS,
	S_COUNT
};

const int kFfSteps[] = {150, 200, 300, 400, 500, 1000, 0};
constexpr int kFfCount = int(sizeof(kFfSteps) / sizeof(kFfSteps[0]));

Row SettingRowFor(int s)
{
	const Settings& c = Config();
	char buf[48];
	switch (s)
	{
		case H_VIDEO: return {"VIDEO", "", true, true};
		case S_SHADER: return {"Shader", ps5crt::Name(ps5crt::Shader(c.shader))};
		case S_SCALE: return {"Screen size", ps5video::ScaleName(ps5video::Scale(c.scale))};
		case S_ASPECT: return {"Aspect ratio", emu::kAspects[c.aspect % emu::kAspectCount].name};
		case S_NTSC: return {"NTSC filter (Blargg)", emu::kNtscFilters[c.ntsc % emu::kNtscFilterCount].name};
		case S_BORDERS: return {"Show the borders (overscan)", emu::kBorders[c.borders % emu::kBorderCount].name};
		case S_SMOOTH: return {"Smooth picture", c.shader ? "(shader)" : OnOff(c.smooth), c.shader == 0};
		case S_SCANLINES: return {"Scanlines (CRT effect)", c.shader ? "(shader)" : OnOff(c.scanlines), c.shader == 0};
		case S_GGLCD: return {"Game Gear LCD ghosting", OnOff(c.gg_lcd)};
		case S_FPS: return {"Show FPS", OnOff(c.show_fps)};
		case H_AUDIO: return {"AUDIO", "", true, true};
		case S_AUDIO: return {"Sound", OnOff(c.audio)};
		case S_VOLUME: snprintf(buf, sizeof(buf), "%d%%", c.volume); return {"Volume", buf};
		case S_FMCHIP: return {"FM sound chip", emu::kFmChips[c.fm_chip % emu::kFmChipCount].name};
		case S_LOWPASS: return {"Low-pass filter", OnOff(c.lowpass)};
		case H_EMU: return {"EMULATION", "", true, true};
		case S_FF:
			if (c.ff_speed == 0)
				return {"Fast forward speed (hold R2)", "Unlimited"};
			snprintf(buf, sizeof(buf), "%d%%", c.ff_speed);
			return {"Fast forward speed (hold R2)", buf};
		case S_REWIND: return {"Rewind (hold L2 + R2)", OnOff(c.rewind)};
		case S_REGION: return {"Console region", emu::kRegions[c.region % emu::kRegionCount].name};
		case S_SPRITES: return {"Remove the sprite limit", OnOff(c.no_sprite_limit)};
		case H_CONTROLS: return {"CONTROLS (Mega Drive pad button  ->  PS5 button)", "", true, true};
		case S_PADTYPE: return {"Mega Drive / Sega CD pad", emu::kPadTypes[c.pad_type % emu::kPadTypeCount].name};
		case S_MULTITAP: return {"4-player adapter", emu::kMultitaps[c.multitap % emu::kMultitapCount].name};
		case S_BTN_DEFAULT: return {"Default button layout", ""};
		case H_LIBRARY: return {"LIBRARY", "", true, true};
		case S_COVERS: return {"Download covers", OnOff(c.covers_download)};
		case H_SYSTEM: return {"SYSTEM", "", true, true};
		case S_DEBUGLOGS: return {"Debug logs", OnOff(c.debug_logs)};
		default:
			if (s >= S_BTN_FIRST && s <= S_BTN_LAST)
			{
				const int i = s - S_BTN_FIRST;
				const int b = c.buttons[i];
				return {std::string("Button ") + emu::kConsoleButtons[i].name,
					emu::kPs5Buttons[b >= 0 && b < emu::kPs5ButtonCount ? b : emu::kPs5ButtonCount - 1].name};
			}
			return {"", ""};
	}
}

int Cycle(int v, int dir, int n)
{
	return ((v + dir) % n + n) % n;
}

void ChangeSetting(int s, int dir)
{
	Settings& c = Config();
	switch (s)
	{
		case S_SHADER: c.shader = Cycle(c.shader, dir, int(ps5crt::Shader::Count)); break;
		case S_SCALE: c.scale = Cycle(c.scale, dir, int(ps5video::Scale::Count)); break;
		case S_ASPECT: c.aspect = Cycle(c.aspect, dir, emu::kAspectCount); break;
		case S_NTSC: c.ntsc = Cycle(c.ntsc, dir, emu::kNtscFilterCount); break;
		case S_BORDERS: c.borders = Cycle(c.borders, dir, emu::kBorderCount); break;
		case S_SMOOTH: c.smooth = !c.smooth; break;
		case S_SCANLINES: c.scanlines = !c.scanlines; break;
		case S_GGLCD: c.gg_lcd = !c.gg_lcd; break;
		case S_FPS: c.show_fps = !c.show_fps; break;
		case S_AUDIO: c.audio = !c.audio; break;
		case S_VOLUME: c.volume = std::max(0, std::min(100, c.volume + dir * 10)); break;
		case S_FMCHIP: c.fm_chip = Cycle(c.fm_chip, dir, emu::kFmChipCount); break;
		case S_LOWPASS: c.lowpass = !c.lowpass; break;
		case S_FF:
		{
			int i = 0;
			while (i < kFfCount - 1 && kFfSteps[i] != c.ff_speed)
				i++;
			c.ff_speed = kFfSteps[Cycle(i, dir, kFfCount)];
			break;
		}
		case S_REWIND: c.rewind = !c.rewind; break;
		case S_REGION: c.region = Cycle(c.region, dir, emu::kRegionCount); break;
		case S_SPRITES: c.no_sprite_limit = !c.no_sprite_limit; break;
		case S_COVERS: c.covers_download = !c.covers_download; break;
		case S_DEBUGLOGS:
			c.debug_logs = !c.debug_logs;
			OrbisLogSetEnabled(c.debug_logs); // at once: the line saying so is the last (or first) one written
			break;
		case S_PADTYPE: c.pad_type = Cycle(c.pad_type, dir, emu::kPadTypeCount); break;
		case S_MULTITAP: c.multitap = Cycle(c.multitap, dir, emu::kMultitapCount); break;
		case S_BTN_DEFAULT: c.DefaultButtons(); break;
		default:
			if (s >= S_BTN_FIRST && s <= S_BTN_LAST)
			{
				int& b = c.buttons[s - S_BTN_FIRST];
				b = Cycle(b, dir, emu::kPs5ButtonCount);
			}
			break;
	}
	emu::ApplySettings();
}

std::vector<Row> SettingRows()
{
	std::vector<Row> rows;
	for (int s = 0; s < S_COUNT; s++)
		rows.push_back(SettingRowFor(s));
	rows.push_back({"Back", ""});
	return rows;
}

void Background(bool over_game)
{
	if (over_game)
	{
		// the paused game, darkened, under the box
		ps5video::FillRect(0, 0, W, H, Rgb(0, 0, 0));
		emu::RedrawLastFrame();
		ps5video::DarkenRect(0, 0, W, H);
		ps5video::DarkenRect(0, 0, W, H);
	}
	else
		Header("Settings");
}

void SettingsScreen(bool over_game)
{
	NavReader nav;
	int sel = S_SHADER;
	for (;;)
	{
		const std::vector<Row> rows = SettingRows();
		Background(over_game);
		DrawOptionBox("Settings", rows, sel, over_game);
		if (!over_game)
			Footer(std::string(icon::Cross) + " Change     " + icon::DpadLeftRight + " Adjust     " + icon::Circle + " Back");
		Present();
		const Nav n = nav.Read();
		if (n.up)
			sel = Step(sel, -1, rows);
		if (n.down)
			sel = Step(sel, 1, rows);
		if (sel < S_COUNT && (n.left || n.right || n.ok))
			ChangeSetting(sel, n.left ? -1 : 1);
		if (n.back || n.options || n.menu || (n.ok && sel == S_COUNT))
		{
			Config().Save();
			return;
		}
	}
}

// Word wrap into lines that fit max_px at `scale`.
std::vector<std::string> Wrap(const std::string& text, int scale, int max_px)
{
	std::vector<std::string> lines;
	std::string cur;
	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t sp = text.find(' ', pos);
		if (sp == std::string::npos)
			sp = text.size();
		const std::string word = text.substr(pos, sp - pos);
		const std::string tryline = cur.empty() ? word : cur + " " + word;
		if (!cur.empty() && TextWidth(tryline.c_str(), scale) > max_px)
		{
			lines.push_back(cur);
			cur = word;
		}
		else
			cur = tryline;
		pos = sp + 1;
	}
	if (!cur.empty())
		lines.push_back(cur);
	return lines;
}
} // namespace

void SettingsMenu()
{
	SettingsScreen(false);
}

PauseAction PauseMenu()
{
	Settings& cfg = Config();
	NavReader nav;
	enum Item
	{
		I_RESUME,
		I_SAVE,
		I_LOAD,
		I_SLOT,
		I_SETTINGS,
		I_RESET,
		I_POWER,
		I_LIST,
		I_QUIT,
		I_COUNT
	};
	int sel = I_RESUME;
	std::string toast;
	double toast_until = 0;

	for (;;)
	{
		char buf[64];
		std::vector<Row> rows(I_COUNT);
		rows[I_RESUME] = {"Resume", ""};
		snprintf(buf, sizeof(buf), "Save state (slot %d)", cfg.state_slot);
		rows[I_SAVE] = {buf, ""};
		snprintf(buf, sizeof(buf), "Load state (slot %d)", cfg.state_slot);
		rows[I_LOAD] = {buf, "", emu::StateExists(cfg.state_slot)};
		snprintf(buf, sizeof(buf), "%d%s", cfg.state_slot, emu::StateExists(cfg.state_slot) ? " (used)" : " (empty)");
		rows[I_SLOT] = {"State slot", buf};
		rows[I_SETTINGS] = {"Settings", ""};
		rows[I_RESET] = {"Reset", ""};
		rows[I_POWER] = {"Power cycle", ""};
		rows[I_LIST] = {"Back to the game list", ""};
		rows[I_QUIT] = {"Quit Genesis Plus GX PS5", ""};

		Background(true);
		const std::string sys = emu::SystemName();
		DrawOptionBox(emu::GameName(), rows, sel, true);
		if (!sys.empty())
			DrawText(W / 2 + 560 - TextWidth(sys.c_str(), 2) - 40, (H - (120 + I_COUNT * 46 + 30)) / 2 + 40, sys.c_str(), 2, kDim);
		if (!toast.empty() && Now() < toast_until)
		{
			const int tw = TextWidth(toast.c_str(), 3);
			ps5video::FillRect((W - tw) / 2 - 30, H - 110, tw + 60, 70, kSel);
			DrawText((W - tw) / 2, H - 90, toast.c_str(), 3, kText);
		}
		Present();

		const Nav n = nav.Read();
		if (n.up)
			sel = Step(sel, -1, rows);
		if (n.down)
			sel = Step(sel, 1, rows);
		if (n.back || n.options || n.menu)
			return PauseAction::Resume;

		if (sel == I_SLOT && (n.left || n.right || n.ok))
		{
			cfg.state_slot = n.left ? (cfg.state_slot + 8) % 10 + 1 : cfg.state_slot % 10 + 1;
			cfg.Save();
		}
		else if (n.ok)
		{
			switch (sel)
			{
				case I_RESUME: return PauseAction::Resume;
				case I_SAVE:
					toast = emu::SaveState(cfg.state_slot) ? "State saved." : "Could not save the state.";
					toast_until = Now() + 2.0;
					cfg.Save();
					break;
				case I_LOAD:
					if (emu::LoadState(cfg.state_slot))
						return PauseAction::Resume;
					toast = "This slot is empty.";
					toast_until = Now() + 2.0;
					break;
				case I_SETTINGS:
					SettingsScreen(true);
					nav = NavReader();
					break;
				case I_RESET:
					emu::Reset();
					return PauseAction::Resume;
				case I_POWER:
					emu::PowerCycle();
					return PauseAction::Resume;
				case I_LIST: return PauseAction::BackToList;
				case I_QUIT: return PauseAction::Quit;
				default: break;
			}
		}
	}
}

void MessageBox(const std::string& title, const std::string& text)
{
	NavReader nav;
	const int bw = 1400;
	const std::vector<std::string> lines = Wrap(text, 3, bw - 80);
	const int bh = 220 + int(lines.size()) * 44;
	for (;;)
	{
		Header("");
		const int bx = (W - bw) / 2, by = (H - bh) / 2;
		ps5video::FillRect(bx - 4, by - 4, bw + 8, bh + 8, kAccent);
		ps5video::FillRect(bx, by, bw, bh, kPanel);
		DrawText(bx + 40, by + 30, FitText(title, 5, bw - 80).c_str(), 5, kAccent);
		int y = by + 120;
		for (const std::string& l : lines)
		{
			DrawText(bx + 40, y, l.c_str(), 3, kText);
			y += 44;
		}
		DrawText(bx + 40, by + bh - 70, (std::string(icon::Cross) + " OK").c_str(), 3, kDim);
		Present();
		const Nav n = nav.Read();
		if (n.ok || n.back || n.options)
			return;
	}
}
} // namespace fe

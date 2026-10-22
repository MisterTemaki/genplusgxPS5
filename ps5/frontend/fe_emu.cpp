// Genesis Plus GX PS5 frontend: the core and its libretro callbacks (fe_emu.h).
//
// Frame pacing, as in Snes9x PS5:
//   - 60 Hz games (NTSC, 59.92 Hz): Present waits for the flip, so the console's vsync paces the emulation, and
//     the resampler (44.1 -> 48 kHz) stretches the sound by up to 0.5% to keep about 60 ms queued.
//   - 50 Hz games (PAL): the audio clock paces instead (wait while more than 60 ms is queued); the flips don't
//     wait (each frame still waits, before writing, for the buffer it last flipped).
//   - In any case, if more than 120 ms is queued (vsync not blocking for some reason), wait on audio.
//   - Fast forward (hold R2): several frames per flip, no sound.
//
// SPDX-License-Identifier: MIT

#include "fe_emu.h"

#include "fe_bram.h"
#include "fe_border.h"
#include "ProsperoNotify.h"
#include "fe_games.h"
#include "fe_i18n.h"
#include "fe_settings.h"
#include "fe_text.h"

#include "OrbisPaths.h"
#include "ProsperoAudio.h"
#include "ProsperoCrash.h"
#include "ProsperoCrt.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "ProsperoVideo.h"

#include "libretro.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <vector>

#include <zlib.h>

namespace emu
{
const Choice kAspects[] = {
	{"TV (the core's)", ""},
	{"Square pixels", ""},
	{"4:3", ""},
	{"16:9", ""},
};
const int kAspectCount = int(sizeof(kAspects) / sizeof(kAspects[0]));

const Choice kNtscFilters[] = {
	{"Off", "disabled"},
	{"Composite", "composite"},
	{"S-Video", "svideo"},
	{"RGB", "rgb"},
	{"Monochrome", "monochrome"},
};
const int kNtscFilterCount = int(sizeof(kNtscFilters) / sizeof(kNtscFilters[0]));

const Choice kBorders[] = {
	{"Off", "disabled"},
	{"Top and bottom", "top/bottom"},
	{"Left and right", "left/right"},
	{"All", "full"},
};
const int kBorderCount = int(sizeof(kBorders) / sizeof(kBorders[0]));

const Choice kFmChips[] = {
	{"MAME YM2612", "mame (ym2612)"},
	{"MAME YM3438", "mame (asic ym3438)"},
	{"Nuked YM2612 (accurate)", "nuked (ym2612)"},
	{"Nuked YM3438 (accurate)", "nuked (ym3438)"},
};
const int kFmChipCount = int(sizeof(kFmChips) / sizeof(kFmChips[0]));

const Choice kRegions[] = {
	{"Auto", "auto"},
	{"USA (NTSC)", "ntsc-u"},
	{"Europe (PAL)", "pal"},
	{"Japan (NTSC)", "ntsc-j"},
};
const int kRegionCount = int(sizeof(kRegions) / sizeof(kRegions[0]));

const Choice kAddOns[] = {
	{"Auto", "auto"},
	{"MegaSD (MD+)", "megasd"},
	{"Sega CD (MSU-MD)", "sega/mega cd"},
	{"None", "none"},
};
const int kAddOnCount = int(sizeof(kAddOns) / sizeof(kAddOns[0]));

const Choice kPadTypes[] = {
	{"Auto (the game's)", ""},
	{"3 buttons", ""},
	{"6 buttons", ""},
};
const int kPadTypeCount = int(sizeof(kPadTypes) / sizeof(kPadTypes[0]));

const Choice kMultitaps[] = {
	{"Off", ""},
	{"4 Way Play (EA)", ""},
	{"Team Player (Sega)", ""},
};
const int kMultitapCount = int(sizeof(kMultitaps) / sizeof(kMultitaps[0]));

const PadButton kPs5Buttons[] = {
	{"Cross", SCE_PAD_BUTTON_CROSS},
	{"Circle", SCE_PAD_BUTTON_CIRCLE},
	{"Square", SCE_PAD_BUTTON_SQUARE},
	{"Triangle", SCE_PAD_BUTTON_TRIANGLE},
	{"L1", SCE_PAD_BUTTON_L1},
	{"R1", SCE_PAD_BUTTON_R1},
	{"OPTIONS", SCE_PAD_BUTTON_OPTIONS},
	{"Touchpad", SCE_PAD_BUTTON_TOUCH_PAD},
	{"None", 0},
};
const int kPs5ButtonCount = int(sizeof(kPs5Buttons) / sizeof(kPs5Buttons[0]));

// Genesis Plus GX's RetroPad layout (libretro.c): B/A/Y = Mega Drive B/C/A, L/X/R = X/Y/Z, Select = Mode;
// Master System 1/2 = B/A, Start = Pause.
const ConsoleButton kConsoleButtons[kConsoleButtonCount] = {
	{"A", "btn_a", RETRO_DEVICE_ID_JOYPAD_Y, 2},
	{"B  (Master System 1)", "btn_b", RETRO_DEVICE_ID_JOYPAD_B, 0},
	{"C  (Master System 2)", "btn_c", RETRO_DEVICE_ID_JOYPAD_A, 1},
	{"X", "btn_x", RETRO_DEVICE_ID_JOYPAD_L, 4},
	{"Y", "btn_y", RETRO_DEVICE_ID_JOYPAD_X, 3},
	{"Z", "btn_z", RETRO_DEVICE_ID_JOYPAD_R, 5},
	{"Start  (Pause)", "btn_start", RETRO_DEVICE_ID_JOYPAD_START, 6},
	{"Mode", "btn_mode", RETRO_DEVICE_ID_JOYPAD_SELECT, 7},
};
void Osd(const std::string& text);
} // namespace emu

namespace
{
using fe::T;

// printf into a std::string (the translated formats)
std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}
constexpr int kTargetQueued = ps5audio::kRate * 60 / 1000; // 60 ms of sound
constexpr int kRewindEvery = 3; // frames between rewind snapshots
constexpr size_t kRewindBytes = 192u * 1024 * 1024;

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = char(tolower(uint8_t(c)));
	return s;
}

std::string ExtOf(const std::string& p)
{
	const size_t dot = p.find_last_of('.');
	const size_t slash = p.find_last_of('/');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
		return "";
	return Lower(p.substr(dot));
}

bool FileExists(const std::string& p)
{
	struct stat st = {};
	return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.insert(out.end(), buf, buf + n);
	fclose(f);
	return true;
}

// data -> path.part, flushed to the disk (fsync) and closed without error, read back and compared, then renamed
// over path: the old file or the whole new one, never a cut or wrong one (a full disk or an I/O error shows up at
// fflush / fsync / fclose; a write the storage got wrong, in the comparison).
bool WriteFileAtomic(const std::string& path, const void* data, size_t size)
{
	const std::string tmp = path + ".part";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	bool ok = fwrite(data, 1, size, f) == size;
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (ok)
	{
		std::vector<uint8_t> back;
		ok = ReadFile(tmp, back) && back.size() == size && (size == 0 || memcmp(back.data(), data, size) == 0);
		if (!ok)
			OrbisLog("[emu] %s: what was written doesn't read back the same: not used", tmp.c_str());
	}
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
	{
		remove(tmp.c_str());
		return false;
	}
	return true;
}

uint64_t Fnv(const uint8_t* p, size_t n)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

struct State
{
	bool inited = false;
	bool loaded = false;
	std::string path, name, dir, ext; // the game; ext without the dot
	fe::System system = fe::System::Md;
	std::vector<uint8_t> rom; // a ROM read from a .zip
	retro_game_info_ext info_ext = {};
	std::string sys_dir, save_dir;

	// the core's options, as libretro variables
	std::map<std::string, std::string> opts;
	bool opts_changed = false;

	// video
	unsigned pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
	std::vector<uint32_t> frame;
	int fw = 0, fh = 0;
	int av_w = 0, av_h = 0; // the core's picture size when the game was loaded (before the first frame gives fw, fh)
	bool new_frame = false;
	double fps = 60.0, core_aspect = 4.0 / 3.0;
	double sample_rate = 44100.0;

	// sound: the core's samples of this frame, and the resampler's state
	std::vector<int16_t> in;
	bool mute = false; // fast forward, rewind: drop the sound
	double frac = 0;
	int16_t hist[3][2] = {}; // the three frames before this call's first one (the cubic filter reads 4)
	// the rate control and the ring, measured between two log lines
	double adj_sum = 0, adj_min = 2, adj_max = 0;
	uint64_t adj_n = 0;
	int queued_min = 1 << 30, queued_max = 0;
	std::vector<int16_t> out;
	std::vector<int16_t> silence; // the zeros that refill a dry ring
	int pads_applied = -1; // the pad type / adapter last given to the core

	// pads
	ps5input::PadState pads[ps5input::kMaxPads];
	uint32_t prev_p1 = 0;
	bool wait_release = true;

	// fast forward, rewind
	bool turbo = false, rewinding = false;
	std::deque<std::vector<uint8_t>> rewind;
	size_t rewind_bytes = 0;
	int rewind_tick = 0;

	// saves
	uint64_t sram_hash = 0;
	size_t sram_size = 0; // the battery RAM's full size, taken before the core runs (then it reports a trimmed size)
	double next_sram_check = 0;

	// messages and the FPS counter
	std::string osd;
	double osd_until = 0;
	bool overlay_was_drawn = false;
	int fps_frames = 0;
	double fps_since = 0, fps_shown = 0;
	uint64_t frames = 0;
	double next_stats = 0;
	int stats_reports = 0;
	// the shader's cost, measured every 2 s; a warning once per game when it can't keep up
	double next_shader_check = 0;
	double shader_ms_sum = 0;
	int shader_ms_n = 0;
	bool shader_warned = false;
	// saves: the backup copy of the battery save is made once per game, before its first write
	bool sram_backed_up = false;
	bool sram_backup_warned = false;
	double last_shader_ms = 0;
	bool bram_backup_warned = false;
};
State g;

// ---- libretro callbacks -------------------------------------------------------------------------------------
void CoreLog(enum retro_log_level level, const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	size_t n = strlen(buf);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = 0;
	static const char* const kLevel[] = {"debug", "info", "warn", "error"};
	if (level == RETRO_LOG_DEBUG)
		return;
	OrbisLog("[core] %s%s%s", level == RETRO_LOG_INFO ? "" : kLevel[level < 4 ? level : 3],
		level == RETRO_LOG_INFO ? "" : ": ", buf);
}

bool Environment(unsigned cmd, void* data)
{
	switch (cmd)
	{
		case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
			*static_cast<const char**>(data) = g.sys_dir.c_str();
			return true;
		case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
			*static_cast<const char**>(data) = g.save_dir.c_str();
			return true;
		case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		{
			const unsigned f = *static_cast<const unsigned*>(data);
			if (f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_0RGB1555 && f != RETRO_PIXEL_FORMAT_XRGB8888)
				return false;
			g.pixel_format = f;
			return true;
		}
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
			static_cast<retro_log_callback*>(data)->log = CoreLog;
			return true;
		case RETRO_ENVIRONMENT_GET_VARIABLE:
		{
			retro_variable* v = static_cast<retro_variable*>(data);
			auto it = v->key ? g.opts.find(v->key) : g.opts.end();
			if (it == g.opts.end())
			{
				v->value = nullptr;
				return false;
			}
			v->value = it->second.c_str();
			return true;
		}
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
			*static_cast<bool*>(data) = g.opts_changed;
			g.opts_changed = false;
			return true;
		case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
			*static_cast<unsigned*>(data) = 0; // the core then uses SET_VARIABLES (accepted below)
			return true;
		case RETRO_ENVIRONMENT_GET_CAN_DUPE:
			*static_cast<bool*>(data) = true;
			return true;
		case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
			return true;
		case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
			if (!g.loaded && g.path.empty())
				return false;
			*static_cast<const retro_game_info_ext**>(data) = &g.info_ext;
			return true;
		case RETRO_ENVIRONMENT_SET_GEOMETRY:
		{
			const retro_game_geometry* geo = static_cast<const retro_game_geometry*>(data);
			if (geo->aspect_ratio > 0.1f)
				g.core_aspect = geo->aspect_ratio;
			return true;
		}
		case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		{
			const retro_system_av_info* av = static_cast<const retro_system_av_info*>(data);
			if (av->geometry.aspect_ratio > 0.1f)
				g.core_aspect = av->geometry.aspect_ratio;
			if (av->timing.fps > 1)
				g.fps = av->timing.fps;
			if (av->timing.sample_rate > 1000)
				g.sample_rate = av->timing.sample_rate;
			OrbisLog("[emu] av info: %.3f fps, %.0f Hz", g.fps, g.sample_rate);
			return true;
		}
		case RETRO_ENVIRONMENT_SET_MESSAGE:
		{
			const retro_message* m = static_cast<const retro_message*>(data);
			if (m && m->msg)
				emu::Osd(m->msg);
			return true;
		}
		// accepted and ignored
		case RETRO_ENVIRONMENT_SET_VARIABLES:
		case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
		case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
		case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
		case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
		case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
		case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
		case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
		case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
		case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
			return true;
		default:
			return false;
	}
}

inline uint32_t Expand565(uint16_t c)
{
	const uint32_t r = (c >> 11) & 31, gg = (c >> 5) & 63, b = c & 31;
	return 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((gg << 2) | (gg >> 4)) << 8) | ((b << 3) | (b >> 2));
}

inline uint32_t Expand555(uint16_t c)
{
	const uint32_t r = (c >> 10) & 31, gg = (c >> 5) & 31, b = c & 31;
	return 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((gg << 3) | (gg >> 2)) << 8) | ((b << 3) | (b >> 2));
}

void Video(const void* data, unsigned w, unsigned h, size_t pitch)
{
	if (!data || w == 0 || h == 0)
		return; // a duplicated frame: keep the last one
	g.fw = int(w);
	g.fh = int(h);
	g.frame.resize(size_t(w) * h);
	const uint8_t* src = static_cast<const uint8_t*>(data);
	for (unsigned y = 0; y < h; y++)
	{
		uint32_t* out = g.frame.data() + size_t(y) * w;
		if (g.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888)
		{
			const uint32_t* in = reinterpret_cast<const uint32_t*>(src + y * pitch);
			for (unsigned x = 0; x < w; x++)
				out[x] = in[x] | 0xff000000u;
		}
		else
		{
			const uint16_t* in = reinterpret_cast<const uint16_t*>(src + y * pitch);
			if (g.pixel_format == RETRO_PIXEL_FORMAT_RGB565)
				for (unsigned x = 0; x < w; x++)
					out[x] = Expand565(in[x]);
			else
				for (unsigned x = 0; x < w; x++)
					out[x] = Expand555(in[x]);
		}
	}
	g.new_frame = true;
}

void AudioSample(int16_t l, int16_t r)
{
	if (g.mute)
		return;
	g.in.push_back(l);
	g.in.push_back(r);
}

size_t AudioBatch(const int16_t* data, size_t frames)
{
	if (!g.mute)
		g.in.insert(g.in.end(), data, data + frames * 2);
	return frames;
}

void InputPoll()
{
	ps5input::Poll();
	for (int p = 0; p < ps5input::kMaxPads; p++)
		g.pads[p] = ps5input::Snapshot(p);
}

// The RetroPad Genesis Plus GX reads, from the player's button layout (Settings, CONTROLS; by position by
// default: Square/Cross/Circle = A/B/C, L1/Triangle/R1 = X/Y/Z, OPTIONS = Start, touchpad = Mode).
uint16_t RetroPadBits(const ps5input::PadState& p, bool dpad_off)
{
	if (!p.connected)
		return 0;
	const fe::Settings& cfg = fe::Config();
	uint16_t bits = 0;
	for (int i = 0; i < emu::kConsoleButtonCount; i++)
	{
		const int b = cfg.buttons[i];
		const unsigned bit = b >= 0 && b < emu::kPs5ButtonCount ? emu::kPs5Buttons[b].bit : 0;
		if (bit && (p.raw_buttons & bit))
			bits |= uint16_t(1u << emu::kConsoleButtons[i].retro_id);
	}
	struct Dir
	{
		unsigned id;
		uint32_t bit;
	};
	static const Dir kDirs[] = {
		{RETRO_DEVICE_ID_JOYPAD_UP, SCE_PAD_BUTTON_UP},
		{RETRO_DEVICE_ID_JOYPAD_DOWN, SCE_PAD_BUTTON_DOWN},
		{RETRO_DEVICE_ID_JOYPAD_LEFT, SCE_PAD_BUTTON_LEFT},
		{RETRO_DEVICE_ID_JOYPAD_RIGHT, SCE_PAD_BUTTON_RIGHT},
	};
	if (!dpad_off)
		for (const Dir& d : kDirs)
			if (p.buttons & d.bit) // the D-pad includes the left stick
				bits |= uint16_t(1u << d.id);
	return bits;
}

int16_t InputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
	if (port >= unsigned(ps5input::kMaxPads) || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD || index != 0)
		return 0;
	// L2 is the hot-key modifier for player 1: the D-pad stays with the hot keys while it is held.
	// While the buttons that closed a menu (Cross, Circle) or opened it (L3 + R3) are still held, player 1's pad
	// is not passed on: Resume with Cross must not press B in the game.
	if (port == 0 && g.wait_release)
		return 0;
	const bool l2 = port == 0 && (g.pads[0].raw_buttons & SCE_PAD_BUTTON_L2) != 0;
	const uint16_t bits = RetroPadBits(g.pads[port], l2);
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return int16_t(bits);
	return id < 16 ? int16_t((bits >> id) & 1) : 0;
}

// ---- sound: the core's 44.1 kHz -> 48 kHz, the rate steered by the ring's fill ---------------------------------
// ---- the performance log (Settings, SYSTEM, Performance log): logs/perf-<system>-<game>.csv, a row a second -----
// What a real game does to the frame budget, for checking Sega CD FMV, CD audio and long sessions on the console:
// the core's time per frame (average and worst), the presentation's (the shader on the CPU, or the hand-over to
// the GPU), the GPU shader's own time, the frames that came late, the sound queued and the underruns, the rate
// control, the CD drive (CD audio playing, data being read) and the PCM chip, and the drift between the sound
// the core made and the frames it ran (it should stay flat over hours: a growing one is sound running ahead of,
// or behind, the picture).
struct Perf
{
	FILE* f = nullptr;
	double start = 0, next = 0, last_frame = 0;
	uint64_t frames = 0, rows = 0;
	double core_sum = 0, core_max = 0, present_sum = 0, frame_max = 0;
	int late = 0, cdda = 0, cdread = 0, pcm = 0;
	uint64_t underruns0 = 0;
	// over the whole session
	double audio_frames = 0, emu_frames = 0;
	double total_core = 0, worst_core = 0;
	uint64_t total_frames = 0, total_late = 0;
	double drift_min = 1e9, drift_max = -1e9;
};
Perf g_perf;

double PerfDrift()
{
	return g.sample_rate > 0 && g.fps > 0 ? (g_perf.audio_frames / g.sample_rate - g_perf.emu_frames / g.fps) * 1000.0 : 0;
}

void PerfStart()
{
	g_perf = Perf();
	if (!fe::Config().perf_log)
		return;
	std::string name = fe::Info(g.system).folder + std::string("-") + g.name;
	for (char& c : name)
		if (c == '/' || c == '\\' || c == ':' || uint8_t(c) < 32)
			c = '_';
	const std::string path = OrbisDir("logs") + "/perf-" + name + ".csv";
	g_perf.f = fopen(path.c_str(), "w");
	if (!g_perf.f)
	{
		OrbisLog("[perf] can't write %s", path.c_str());
		return;
	}
	fprintf(g_perf.f, "# Genesis Plus GX PS5 %s performance log: %s (%s), %.3f fps, renderer %s, shader %s\n",
		GENPLUS_PS5_VERSION, g.name.c_str(), emu::SystemName().c_str(), g.fps, ps5video::GpuActive() ? "GPU" : "CPU",
		ps5video::ShaderName(fe::Config().shader));
	fprintf(g_perf.f, "time_s,frames,fps,core_ms_avg,core_ms_max,present_ms_avg,frame_ms_max,late_frames,shader_ms,"
					  "audio_queued_ms,underruns,rate,cd_audio_pct,cd_read_pct,pcm_pct,av_drift_ms\n");
	fflush(g_perf.f);
	g_perf.start = g_perf.next = g_perf.last_frame = Now();
	g_perf.next += 1.0;
	g_perf.underruns0 = ps5audio::Underruns();
	OrbisLog("[perf] logging to %s", path.c_str());
}

void PerfFrame(double core_ms, double present_ms, double shader_ms)
{
	if (!g_perf.f)
		return;
	const double now = Now();
	const double frame_ms = (now - g_perf.last_frame) * 1000.0;
	g_perf.last_frame = now;
	g_perf.frames++;
	g_perf.core_sum += core_ms;
	g_perf.core_max = std::max(g_perf.core_max, core_ms);
	g_perf.present_sum += present_ms;
	g_perf.frame_max = std::max(g_perf.frame_max, frame_ms);
	if (!g.turbo && !g.rewinding && frame_ms > 1500.0 / std::max(1.0, g.fps))
		g_perf.late++;
	const int cd = fe_cd_activity();
	g_perf.cdda += (cd & 1) != 0;
	g_perf.pcm += (cd & 2) != 0;
	g_perf.cdread += (cd & 4) != 0;
	g_perf.total_core += core_ms;
	g_perf.worst_core = std::max(g_perf.worst_core, core_ms);
	g_perf.total_frames++;
	if (now < g_perf.next)
		return;
	const double span = now - (g_perf.next - 1.0);
	const double n = double(std::max<uint64_t>(1, g_perf.frames));
	const uint64_t under = ps5audio::Underruns();
	const double drift = PerfDrift();
	g_perf.drift_min = std::min(g_perf.drift_min, drift);
	g_perf.drift_max = std::max(g_perf.drift_max, drift);
	g_perf.total_late += uint64_t(g_perf.late);
	fprintf(g_perf.f, "%.1f,%llu,%.2f,%.3f,%.3f,%.3f,%.2f,%d,%.3f,%.1f,%llu,%.4f,%.0f,%.0f,%.0f,%.2f\n",
		now - g_perf.start, (unsigned long long)g_perf.frames, g_perf.frames / std::max(1e-3, span), g_perf.core_sum / n,
		g_perf.core_max, g_perf.present_sum / n, g_perf.frame_max, g_perf.late, shader_ms,
		ps5audio::Queued() * 1000.0 / ps5audio::kRate, (unsigned long long)(under - g_perf.underruns0),
		g.adj_n ? g.adj_sum / double(g.adj_n) : 1.0, g_perf.cdda * 100.0 / n, g_perf.cdread * 100.0 / n,
		g_perf.pcm * 100.0 / n, drift);
	fflush(g_perf.f);
	g_perf.rows++;
	g_perf.underruns0 = under;
	g_perf.frames = 0;
	g_perf.core_sum = g_perf.core_max = g_perf.present_sum = g_perf.frame_max = 0;
	g_perf.late = g_perf.cdda = g_perf.cdread = g_perf.pcm = 0;
	g_perf.next = now + 1.0;
}

extern "C" void fe_chdcache_take(unsigned long long* hits, unsigned long long* misses);

void PerfStop()
{
	if (!g_perf.f)
		return;
	unsigned long long hits = 0, misses = 0;
	fe_chdcache_take(&hits, &misses);
	if (hits + misses)
		OrbisLog("[perf] CHD: %llu hunk(s) read, %llu decompressed ahead on the worker, %llu on the emulation thread",
			hits + misses, hits, misses);
	const double secs = Now() - g_perf.start;
	OrbisLog("[perf] %.0f s, %llu frames: core %.2f ms a frame on average, %.2f ms at worst; %llu late frames; "
			 "sound/picture drift %.1f to %.1f ms",
		secs, (unsigned long long)g_perf.total_frames,
		g_perf.total_frames ? g_perf.total_core / double(g_perf.total_frames) : 0.0, g_perf.worst_core,
		(unsigned long long)g_perf.total_late, g_perf.rows ? g_perf.drift_min : 0.0, g_perf.rows ? g_perf.drift_max : 0.0);
	fclose(g_perf.f);
	g_perf.f = nullptr;
}

void FlushAudio()
{
	const size_t frames = g.in.size() / 2;
	if (frames == 0)
		return;
	if (!g.turbo && !g.rewinding)
		g_perf.audio_frames += double(frames); // what the core made, for the sound/picture drift
	const fe::Settings& cfg = fe::Config();
	if (!cfg.audio || g.mute || !ps5audio::Available())
	{
		g.in.clear();
		return;
	}
	int queued = ps5audio::Queued();
	if (queued < kTargetQueued / 4)
	{
		// the ring ran (nearly) dry: a game just started, or came back from a menu, or the frame was late. Fill
		// it with silence up to the target at once; the rate control alone (0.5%) would take ~10 s to get there,
		// and play crackling meanwhile.
		g.silence.assign(size_t(kTargetQueued - queued) * 2, 0);
		ps5audio::Push(g.silence.data(), kTargetQueued - queued);
		queued = kTargetQueued;
	}
	double adj = double(kTargetQueued - queued) / kTargetQueued;
	adj = 1.0 + std::max(-1.0, std::min(1.0, adj)) * 0.005;
	const double step = g.sample_rate / (ps5audio::kRate * adj); // input frames per output frame
	g.adj_sum += adj;
	g.adj_n++;
	g.adj_min = std::min(g.adj_min, adj);
	g.adj_max = std::max(g.adj_max, adj);
	g.queued_min = std::min(g.queued_min, queued);
	g.queued_max = std::max(g.queued_max, queued);
	const double vol = cfg.volume / 100.0;
	g.out.clear();
	// 4-point, 3rd-order Hermite interpolation (Catmull-Rom) between the frames around each output frame: flat to
	// well above 10 kHz and with little aliasing, where linear interpolation dulled the treble and an 8-bit
	// fraction added noise. The frames before this call's first one are kept in g.hist.
	auto at = [&](long k, int c) -> float {
		return k < 0 ? float(g.hist[3 + k][c]) : float(g.in[size_t(k) * 2 + c]);
	};
	double pos = g.frac; // the output frame lies between input frames (pos - 2) and (pos - 1): see below
	while (pos < double(frames))
	{
		const long i = long(pos); // between frames i - 2 and i - 1 (both already here), with i, i - 3 around
		const float t = float(pos - double(i));
		for (int c = 0; c < 2; c++)
		{
			const float y0 = at(i - 3, c), y1 = at(i - 2, c), y2 = at(i - 1, c), y3 = at(i, c);
			const float c1 = 0.5f * (y2 - y0);
			const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
			const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
			const float v = float((((c3 * t + c2) * t + c1) * t + y1) * vol);
			g.out.push_back(int16_t(std::max(-32768.0f, std::min(32767.0f, std::nearbyint(v)))));
		}
		pos += step;
	}
	g.frac = pos - double(frames);
	// the last three frames, for the next call (read before g.hist is written: a short call reads g.hist too)
	int16_t last[3][2];
	for (int k = 0; k < 3; k++)
		for (int c = 0; c < 2; c++)
			last[k][c] = int16_t(at(long(frames) - 3 + k, c));
	memcpy(g.hist, last, sizeof(last));
	g.in.clear();
	ps5audio::Push(g.out.data(), int(g.out.size() / 2));
}

// ---- saves ------------------------------------------------------------------------------------------------
// Saves are kept per system (saves/MegaDrive/<game>.srm, states/MasterSystem/<game>.state3...): No-Intro gives the
// same name to games of different systems ("Sonic The Hedgehog (USA, Europe)" is a Mega Drive and a Master System
// game), and one game must never load the other's battery RAM or state.
std::string SaveDir(const char* kind)
{
	return OrbisDir(kind) + "/" + fe::Info(g.system).folder;
}

std::string SramPath()
{
	return SaveDir("saves") + "/" + g.name + ".srm";
}

// The game's own settings, when it has them: games/<system>/<game>.ini
std::string GameIniPath()
{
	return OrbisDir("games") + "/" + fe::Info(g.system).folder + "/" + g.name + ".ini";
}

std::string StatePath(int slot)
{
	return SaveDir("states") + "/" + g.name + ".state" + std::to_string(slot);
}

// Saves made by 1.0's first builds sit directly in saves/ and states/: the first game of that name to start takes
// them into its system's folder (as before, when that game was the one using them).
void MigrateSaves()
{
	OrbisMkdirs(SaveDir("saves"));
	OrbisMkdirs(SaveDir("states"));
	auto move = [](const std::string& from, const std::string& to) {
		if (FileExists(from) && !FileExists(to))
		{
			const bool ok = rename(from.c_str(), to.c_str()) == 0;
			OrbisLog("[emu] moved %s -> %s (%s)", from.c_str(), to.c_str(), ok ? "ok" : "failed");
		}
	};
	move(OrbisDir("saves") + "/" + g.name + ".srm", SramPath());
	for (int slot = 1; slot <= 10; slot++)
		move(OrbisDir("states") + "/" + g.name + ".state" + std::to_string(slot), StatePath(slot));
}

void LoadSram()
{
	uint8_t* mem = static_cast<uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
	// before the first retro_run the core reports the full size (0x10000); once running, only up to the last byte
	// that isn't 0xFF -- 0 for a save the game erased, which would then never be written. Keep the full size.
	const size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	g.sram_size = mem ? size : 0;
	g.sram_hash = mem && size ? Fnv(mem, size) : 0;
	if (!mem || !size)
		return;
	std::vector<uint8_t> data;
	if (ReadFile(SramPath(), data) && !data.empty())
	{
		memcpy(mem, data.data(), std::min(size, data.size()));
		g.sram_hash = Fnv(mem, size);
		OrbisLog("[emu] battery save loaded: %s (%zu bytes)", SramPath().c_str(), data.size());
	}
}

// Before a save file is written for the first time while a game runs, the file as it was when the game started is
// kept beside it as <file>.bak (replacing the last one): a game that wrecks its own save, or a bad write, can be
// undone from there.
bool CopyFile(const std::string& from, const std::string& to)
{
	std::vector<uint8_t> data;
	return ReadFile(from, data) && WriteFileAtomic(to, data.data(), data.size());
}

// *done is set only once the copy is really there (or there was nothing to copy): a failed copy is tried again at
// the next write, and said on the screen once. Returns whether the file may be written over: false while the copy
// of it couldn't be made (the caller then writes beside it, never over it).
bool BackupOnce(const std::string& path, bool* done)
{
	if (*done)
		return true;
	if (!FileExists(path))
	{
		*done = true;
		return true;
	}
	const bool ok = CopyFile(path, path + ".bak");
	OrbisLog("[emu] kept the save as it was: %s.bak (%s)", path.c_str(), ok ? "ok" : "failed");
	if (ok)
		*done = true;
	else if (!g.sram_backup_warned)
	{
		g.sram_backup_warned = true;
		emu::Osd("Could not back up the save: the new one is kept beside it (.srm.new)");
	}
	return ok;
}

// Writes the battery save when it changed (or when `force`).
void SaveSram(bool force)
{
	uint8_t* mem = static_cast<uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
	const size_t size = g.sram_size;
	if (!mem || !size)
		return;
	const uint64_t h = Fnv(mem, size);
	if (h == g.sram_hash && !force)
		return;
	if (h == g.sram_hash && !FileExists(SramPath()))
	{
		// nothing written by the game yet: don't create a file of zeros
		bool blank = true;
		for (size_t i = 0; i < size && blank; i++)
			blank = mem[i] == 0x00 || mem[i] == 0xff;
		if (blank)
			return;
	}
	// over the file only once its backup is there; otherwise beside it, the file left as it was
	const std::string target = BackupOnce(SramPath(), &g.sram_backed_up) ? SramPath() : SramPath() + ".new";
	const bool ok = WriteFileAtomic(target, mem, size);
	if (ok)
		g.sram_hash = h; // a failed write is tried again 3 s later and when the game closes
	OrbisLog("[emu] battery save -> %s (%s)", target.c_str(), ok ? "ok" : "failed");
}

// The Sega CD's backup RAM and RAM cartridge, every 3 s when they changed (the core writes them only on unload).
void SaveBram()
{
	const int r = fe_bram_flush();
	if (r)
		OrbisLog("[emu] Sega CD backup RAM -> %s%s%s%s", (r & 1) ? "internal " : "", (r & 2) ? "cartridge " : "",
			(r & 4) ? "(a write failed)" : "(ok)", (r & 8) ? ", beside the file (.new): its backup failed" : "");
	if ((r & 8) && !g.bram_backup_warned)
	{
		g.bram_backup_warned = true;
		emu::Osd("Could not back up the Sega CD saves: the new ones are kept beside them (.brm.new)");
	}
}

// The Sega CD BIOS, checked before loading a disc (Genesis Plus GX picks the one of the disc's region).
std::string MissingCdBios()
{
	static const char* const kNames[] = {"bios_CD_U.bin", "bios_CD_E.bin", "bios_CD_J.bin"};
	for (const char* n : kNames)
		if (FileExists(g.sys_dir + "/" + n))
			return "";
	return "bios_CD_U.bin (USA), bios_CD_E.bin (Europe) or bios_CD_J.bin (Japan)";
}

void ShowOverlays(ps5video::Rect* damage)
{
	const fe::Settings& cfg = fe::Config();
	const double now = Now();
	bool drawn = false;
	if (cfg.show_fps)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.1f fps", g.fps_shown);
		const int w = fe::TextWidth(buf, 3);
		ps5video::FillRect(ps5video::kWidth - w - 60, 20, w + 40, 50, ps5video::Rgb(0, 0, 0));
		fe::DrawText(ps5video::kWidth - w - 40, 28, buf, 3, ps5video::Rgb(255, 255, 255));
		drawn = true;
	}
	if (!g.osd.empty() && now < g.osd_until)
	{
		const std::string text = fe::FitText(g.osd, 3, ps5video::kWidth - 160);
		const int w = fe::TextWidth(text.c_str(), 3);
		ps5video::DarkenRect(40, ps5video::kHeight - 100, w + 40, 60);
		fe::DrawText(60, ps5video::kHeight - 90, text.c_str(), 3, ps5video::Rgb(255, 255, 255));
		drawn = true;
	}
	if (drawn || g.overlay_was_drawn)
		*damage = ps5video::Rect{0, 0, ps5video::kWidth, ps5video::kHeight};
	if (!drawn && g.overlay_was_drawn)
		ps5video::InvalidateFrame(); // clear what the overlay left outside the picture
	g.overlay_was_drawn = drawn;
}

double FrameAspect()
{
	const fe::Settings& cfg = fe::Config();
	switch (cfg.aspect)
	{
		case 1: return g.fh > 0 ? double(g.fw) / (g.fh > 300 ? g.fh / 2 : g.fh) : 4.0 / 3.0;
		case 2: return 4.0 / 3.0;
		case 3: return 16.0 / 9.0;
		default: return g.core_aspect;
	}
}

// The picture the border's window is fitted to (fe_border.h): FrameAspect's ratio and the line grid DrawLast uses, from the
// last frame, or before the first one from the size the core gave at load.
fe::border::Shape BorderShape()
{
	const fe::Settings& cfg = fe::Config();
	fe::border::Shape shape;
	shape.valid = true;
	const int w = g.fw > 0 ? g.fw : g.av_w, h = g.fh > 0 ? g.fh : g.av_h;
	const int lines = h > 300 ? h / 2 : h;
	shape.base_h = lines > 0 ? lines : 224;
	switch (cfg.aspect)
	{
		case 1: shape.aspect = w > 0 && lines > 0 ? double(w) / lines : 4.0 / 3.0; break;
		case 2: shape.aspect = 4.0 / 3.0; break;
		case 3: shape.aspect = 16.0 / 9.0; break;
		default: shape.aspect = g.core_aspect; break;
	}
	shape.scale = ps5video::Scale(cfg.scale);
	return shape;
}

ps5video::Rect DrawLast()
{
	if (g.frame.empty())
		return ps5video::Rect{0, 0, 0, 0};
	const fe::Settings& cfg = fe::Config();
	const int base_h = g.fh > 300 ? g.fh / 2 : g.fh; // interlaced (double field) pictures: the 224-line grid
	return ps5video::DrawFrame(g.frame.data(), g.fw, g.fh, base_h, FrameAspect(), ps5video::Scale(cfg.scale), cfg.smooth,
		cfg.scanlines, cfg.shader);
}

void RewindPush()
{
	const size_t size = retro_serialize_size();
	if (size == 0)
		return;
	std::vector<uint8_t> s(size);
	if (!retro_serialize(s.data(), size))
		return;
	g.rewind_bytes += size;
	g.rewind.push_back(std::move(s));
	while (g.rewind_bytes > kRewindBytes && !g.rewind.empty())
	{
		g.rewind_bytes -= g.rewind.front().size();
		g.rewind.pop_front();
	}
}

void RewindClear()
{
	g.rewind.clear();
	g.rewind_bytes = 0;
	g.rewind_tick = 0;
}
} // namespace

namespace emu
{
bool InitCore()
{
	if (g.inited)
		return true;
	GENPLUS_STAGE(Emu, "init");
	g.sys_dir = OrbisDir("bios");
	g.save_dir = OrbisDir("saves");
	ApplySettings();
	retro_set_environment(Environment);
	retro_set_video_refresh(Video);
	retro_set_audio_sample(AudioSample);
	retro_set_audio_sample_batch(AudioBatch);
	retro_set_input_poll(InputPoll);
	retro_set_input_state(InputState);
	retro_init();
	retro_system_info info = {};
	retro_get_system_info(&info);
	OrbisLog("[emu] %s %s ready (libretro API %u; BIOS folder %s)", info.library_name, info.library_version,
		retro_api_version(), g.sys_dir.c_str());
	g.inited = true;
	return true;
}

void DeinitCore()
{
	if (!g.inited)
		return;
	CloseGame();
	retro_deinit();
	g.inited = false;
}

// The pads plugged into the console's two ports (libretro.c's device types): for Mega Drive and Sega CD games the
// pad type and multiplayer adapter from the settings; the 8-bit systems always get their own 2-button pad.
void ApplyControllers()
{
	const fe::Settings& s = fe::Config();
	const bool md = g.system == fe::System::Md || g.system == fe::System::SegaCd;
	const int pad = md ? s.pad_type % kPadTypeCount : 0;
	const int tap = md ? s.multitap % kMultitapCount : 0;
	const int key = pad * 10 + tap;
	if (key == g.pads_applied)
		return;
	g.pads_applied = key;
	constexpr unsigned kJoypad = RETRO_DEVICE_JOYPAD;
	constexpr unsigned k3B = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0), k6B = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1);
	constexpr unsigned kWay3B = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 3), kWay6B = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 4);
	constexpr unsigned kTeam3B = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 5), kTeam6B = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 6);
	const unsigned single = pad == 1 ? k3B : (pad == 2 ? k6B : kJoypad);
	// an adapter needs a fixed pad type: "auto" gives it 3-button pads, as most 4-player games expect
	if (tap == 1)
		retro_set_controller_port_device(0, pad == 2 ? kWay6B : kWay3B); // takes both ports
	else
	{
		retro_set_controller_port_device(0, tap == 2 ? (pad == 2 ? kTeam6B : kTeam3B) : single);
		retro_set_controller_port_device(1, single);
	}
	OrbisLog("[emu] controllers: %s pad%s%s", kPadTypes[pad].name, tap ? ", " : "", tap ? kMultitaps[tap].name : "");
}

void ApplySettings()
{
	const fe::Settings& s = fe::Config();
	auto set = [](const char* key, const std::string& value) {
		auto it = g.opts.find(key);
		if (it == g.opts.end() || it->second != value)
		{
			g.opts[key] = value;
			g.opts_changed = true;
		}
	};
	set("genesis_plus_gx_region_detect", kRegions[s.region % kRegionCount].value);
	set("genesis_plus_gx_add_on", kAddOns[s.add_on % kAddOnCount].value);
	set("genesis_plus_gx_ym2612", kFmChips[s.fm_chip % kFmChipCount].value);
	set("genesis_plus_gx_blargg_ntsc_filter", kNtscFilters[s.ntsc % kNtscFilterCount].value);
	set("genesis_plus_gx_overscan", kBorders[s.borders % kBorderCount].value);
	set("genesis_plus_gx_lcd_filter", s.gg_lcd ? "enabled" : "disabled");
	set("genesis_plus_gx_no_sprite_limit", s.no_sprite_limit ? "enabled" : "disabled");
	set("genesis_plus_gx_audio_filter", s.lowpass ? "low-pass" : "disabled");
	set("genesis_plus_gx_sound_output", "stereo");
	set("genesis_plus_gx_aspect_ratio", "auto");
	set("genesis_plus_gx_render", "single field");
	if (!s.rewind)
		RewindClear();
	if (g.loaded)
	{
		ApplyControllers();
		fe::border::Reselect(BorderShape()); // the Border setting, or a picture setting the window follows, changed
	}
}

bool LoadGame(const std::string& path, std::string* error)
{
	if (!g.inited)
		return false;
	GENPLUS_STAGE(Emu, "load game");
	CloseGame();
	const std::string file = path.substr(path.find_last_of('/') + 1);
	if (path.size() >= 255)
	{
		// the core keeps the game's path and the paths it builds from it in 256-byte buffers
		OrbisLog("[emu] could not start %s: the path is %zu characters long", path.c_str(), path.size());
		if (error)
			*error = Fmt(T("The path of %s is too long (over 254 characters): move it to a shorter folder."), file.c_str());
		return false;
	}
	std::string ext = ExtOf(path);
	g.path = path;
	g.dir = path.substr(0, path.find_last_of('/'));
	g.name = file.substr(0, file.size() - ext.size());
	g.rom.clear();
	g.info_ext = retro_game_info_ext();

	std::string inner;
	if (ext == ".zip")
	{
		if (!fe::ReadFromZip(path, &inner, &g.rom))
		{
			OrbisLog("[emu] could not start %s: no game inside the zip", path.c_str());
			if (error)
				*error = Fmt(T("There is no game Genesis Plus GX PS5 knows inside %s."), file.c_str());
			g.path.clear();
			return false;
		}
		ext = ExtOf(inner);
	}
	if (!fe::SystemForExt(ext, &g.system))
		g.system = fe::System::Md;
	g.ext = ext.empty() ? "" : ext.substr(1);

	if (g.system == fe::System::SegaCd)
	{
		const std::string bios = MissingCdBios();
		if (!bios.empty())
		{
			OrbisLog("[emu] missing firmware: Sega CD BIOS (checked before loading)");
			OrbisLog("[emu] could not start %s: missing the Sega CD BIOS", path.c_str());
			if (error)
				*error = Fmt(T("Sega CD games need the BIOS: %s. Copy it to %s/ and try again."), bios.c_str(), g.sys_dir.c_str());
			g.path.clear();
			return false;
		}
	}

	g.info_ext.full_path = g.rom.empty() ? g.path.c_str() : nullptr;
	g.info_ext.archive_path = g.rom.empty() ? nullptr : g.path.c_str();
	g.info_ext.dir = g.dir.c_str();
	g.info_ext.name = g.name.c_str();
	g.info_ext.ext = g.ext.c_str();
	g.info_ext.data = g.rom.empty() ? nullptr : g.rom.data();
	g.info_ext.size = g.rom.size();
	g.info_ext.file_in_archive = !g.rom.empty();
	g.info_ext.persistent_data = false;

	retro_game_info gi = {};
	gi.path = g.path.c_str();
	gi.data = g.rom.empty() ? nullptr : g.rom.data();
	gi.size = g.rom.size();

	OrbisLog("[emu] loading %s%s%s", path.c_str(), inner.empty() ? "" : " -> ", inner.c_str());
	fe::BeginGame(GameIniPath());
	ApplySettings();
	g.opts_changed = true;
	g.frame.clear();
	g.fw = g.fh = 0;
	g.in.clear();
	g.mute = false;
	if (!retro_load_game(&gi))
	{
		OrbisLog("[emu] could not start %s", path.c_str());
		if (error)
		{
			if (g.system == fe::System::SegaCd)
				*error = Fmt(T("Genesis Plus GX could not start %s. Sega CD games need the BIOS of their region in %s: "
							   "bios_CD_U.bin (USA), bios_CD_E.bin (Europe), bios_CD_J.bin (Japan)."),
					file.c_str(), g.sys_dir.c_str());
			else
				*error = Fmt(T("Genesis Plus GX could not load %s."), file.c_str());
		}
		g.path.clear();
		g.rom.clear();
		fe::EndGame();
		ApplySettings();
		return false;
	}
	g.pads_applied = -1;
	ApplyControllers();
	retro_system_av_info av = {};
	retro_get_system_av_info(&av);
	g.fps = av.timing.fps > 1 ? av.timing.fps : 60.0;
	g.sample_rate = av.timing.sample_rate > 1000 ? av.timing.sample_rate : 44100.0;
	g.core_aspect = av.geometry.aspect_ratio > 0.1f ? av.geometry.aspect_ratio : 4.0 / 3.0;
	g.av_w = int(av.geometry.base_width);
	g.av_h = int(av.geometry.base_height);
	g.loaded = true;
	g.wait_release = true;
	g.prev_p1 = 0;
	g.turbo = g.rewinding = false;
	g.frac = 0;
	memset(g.hist, 0, sizeof(g.hist));
	g.adj_sum = 0;
	g.adj_n = 0;
	g.adj_min = 2;
	g.adj_max = 0;
	g.queued_min = 1 << 30;
	g.queued_max = 0;
	g.next_shader_check = Now() + 2.0;
	g.shader_ms_sum = 0;
	g.shader_ms_n = 0;
	g.shader_warned = false;
	g.sram_backed_up = false;
	g.sram_backup_warned = false;
	g.bram_backup_warned = false;
	g.frames = 0;
	g.fps_frames = 0;
	g.fps_since = Now();
	g.next_stats = Now() + 1.0;
	g.stats_reports = 0;
	g.next_sram_check = Now() + 3.0;
	g.overlay_was_drawn = false;
	RewindClear();
	ps5video::InvalidateFrame();
	MigrateSaves();
	LoadSram();
	fe_bram_start();
	PerfStart();
	OrbisLog("[emu] running \"%s\" (%s), %.3f fps, %.0f Hz, aspect %.4f, state size %zu", g.name.c_str(),
		SystemName().c_str(), g.fps, g.sample_rate, g.core_aspect, retro_serialize_size());
	fe::border::Select(path, BorderShape()); // reads /data: before the game's first frame
	return true;
}

void CloseGame()
{
	if (!g.loaded)
		return;
	GENPLUS_STAGE(Emu, "close game");
	PerfStop();
	SaveSram(false);
	SaveBram();
	fe_bram_stop();
	retro_unload_game(); // (its own Sega CD backup RAM write is off: fe_bram_stop)
	g.loaded = false;
	g.frame.clear();
	g.rom.clear();
	RewindClear();
	fe::border::Release();
	fe::EndGame();
	ApplySettings();
	OrbisLog("[emu] game closed");
}

bool GameLoaded()
{
	return g.loaded;
}

std::string GameName()
{
	return g.name;
}

std::string SystemName()
{
	return g.loaded || !g.path.empty() ? fe::Info(g.system).name : "";
}

void Pause() {}

void Resume()
{
	ApplySettings();
	g.wait_release = true;
	ps5video::InvalidateFrame();
	g.new_frame = true;
}

void Osd(const std::string& text)
{
	g.osd = T(text.c_str());
	g.osd_until = Now() + 2.5;
}

// A state file is the core's state followed by a 16-byte trailer: "GPXS", the trailer's version (1), the state's
// size and its CRC-32 (zlib's), little-endian. A load checks all four, so a state cut short, padded, or changed
// anywhere inside is refused, not only one of the wrong size. States saved before 1.9 have no trailer: they are
// still loaded when their size is exactly the core's (as 1.8 did), and said to be unchecked in the log.
constexpr uint32_t kStateMagic = 0x53585047; // "GPXS"
constexpr size_t kStateTrailer = 16;

void AddTrailer(std::vector<uint8_t>& data)
{
	const uint32_t size = uint32_t(data.size());
	const uint32_t crc = uint32_t(crc32(0L, data.data(), uInt(data.size())));
	const uint32_t t[4] = {kStateMagic, 1, size, crc};
	for (uint32_t v : t)
		for (int i = 0; i < 4; i++)
			data.push_back(uint8_t(v >> (8 * i)));
}

uint32_t Le32(const uint8_t* p)
{
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

enum class StateCheck
{
	Good, // the trailer and its CRC match
	Legacy, // no trailer, the core's size: a state of 1.8 or older
	Damaged, // a trailer that doesn't match, or no trailer and the wrong size
};

// Checks `data` as a state file for a core whose states are `want` bytes; on success it is cut to the state alone.
StateCheck CheckState(std::vector<uint8_t>& data, size_t want, std::string& why)
{
	if (data.size() == want + kStateTrailer)
	{
		const uint8_t* t = data.data() + want;
		if (Le32(t) != kStateMagic || Le32(t + 4) != 1 || Le32(t + 8) != want)
		{
			why = "its trailer is not a Genesis Plus GX PS5 one";
			return StateCheck::Damaged;
		}
		const uint32_t crc = uint32_t(crc32(0L, data.data(), uInt(want)));
		if (crc != Le32(t + 12))
		{
			char buf[96];
			snprintf(buf, sizeof(buf), "CRC-32 %08x, the file says %08x", crc, Le32(t + 12));
			why = buf;
			return StateCheck::Damaged;
		}
		data.resize(want);
		return StateCheck::Good;
	}
	if (data.size() == want)
	{
		// a trailer-less file of the core's size: an older state, unless it ends like a trailer of another size
		return StateCheck::Legacy;
	}
	char buf[96];
	snprintf(buf, sizeof(buf), "%zu bytes, the core's states are %zu (+%zu)", data.size(), want, kStateTrailer);
	why = buf;
	return StateCheck::Damaged;
}

// rename(), with the host tests' fault injection: GENPLUS_HOST_FAIL_RENAME=<item>[,<item>...] makes every rename whose
// source or target contains an item's text fail (EIO), to check the save paths against a storage that refuses a
// step ("from=<text>" / "to=<text>" look at one side only).
int Rename(const std::string& from, const std::string& to)
{
#ifndef __PROSPERO__
	if (const char* fail = getenv("GENPLUS_HOST_FAIL_RENAME"))
	{
		std::string list = fail;
		size_t pos = 0;
		while (pos <= list.size())
		{
			size_t end = list.find(',', pos);
			if (end == std::string::npos)
				end = list.size();
			std::string item = list.substr(pos, end - pos);
			// "from=<text>" matches the source only, "to=<text>" the target only, "<text>" either
			bool src = true, dst = true;
			if (item.compare(0, 5, "from=") == 0)
			{
				item = item.substr(5);
				dst = false;
			}
			else if (item.compare(0, 3, "to=") == 0)
			{
				item = item.substr(3);
				src = false;
			}
			if (!item.empty() && ((src && from.find(item) != std::string::npos) || (dst && to.find(item) != std::string::npos)))
			{
				errno = EIO;
				return -1;
			}
			pos = end + 1;
		}
	}
#endif
	return rename(from.c_str(), to.c_str());
}

// A state written over another one keeps the other as <state>.prev: the new state is written in full first
// (<state>.part, flushed and read back by WriteFileAtomic), then the old one becomes .prev, then the new one takes
// its name. If the old one can't be kept, nothing is replaced. If the new one can't take its name, the old one is
// put back; if even that fails, it is still whole in <state>.prev (the pause menu's "Load the previous state"), and
// the log and the screen say so -- no step loses the state the slot had.
bool WriteState(const std::string& path, std::vector<uint8_t> data)
{
	AddTrailer(data);
	const std::string part = path + ".part";
	const std::string prev = path + ".prev";
	if (!WriteFileAtomic(part, data.data(), data.size())) // flushed and read back
		return false;
	const bool had_old = FileExists(path);
	if (had_old && Rename(path, prev) != 0)
	{
		OrbisLog("[emu] could not keep the previous state of %s (errno %d): the slot is left as it was", path.c_str(),
			errno);
		remove(part.c_str());
		return false;
	}
	if (Rename(part, path) != 0)
	{
		const int err = errno;
		remove(part.c_str());
		if (!had_old)
		{
			OrbisLog("[emu] could not put the new state in place (errno %d): the slot stays empty", err);
			return false;
		}
		if (Rename(prev, path) == 0)
		{
			OrbisLog("[emu] could not put the new state in place (errno %d): the old one is back, the slot is left as it was",
				err);
			return false;
		}
		OrbisLog("[emu] could not put the new state in place (errno %d), nor the old one back (errno %d): the slot's "
				 "state is whole in %s",
			err, errno, prev.c_str());
		emu::Osd("The slot's state is kept as its previous state: load it from the pause menu");
		return false;
	}
	return true;
}

bool LoadStateFile(const std::string& path, const char* what, int slot)
{
	std::vector<uint8_t> data;
	if (!ReadFile(path, data) || data.empty())
	{
		Osd(Fmt(T(strcmp(what, "The state") ? "The previous state of slot %d is empty" : "The state of slot %d is empty"), slot));
		return false;
	}
	// a damaged state, or one from another core version, is refused: the game goes on as it was
	const size_t want = retro_serialize_size();
	std::string why;
	const StateCheck check = CheckState(data, want, why);
	if (check == StateCheck::Damaged)
	{
		OrbisLog("[emu] load state %d (%s): %s: refused", slot, path.c_str(), why.c_str());
		Osd("This state is damaged or from another version: not loaded");
		return false;
	}
	// the game as it is now, to go back to if the core refuses the state half-way
	std::vector<uint8_t> before(want);
	const bool have_before = retro_serialize(before.data(), want);
	bool ok = retro_unserialize(data.data(), data.size());
	if (!ok && have_before)
		retro_unserialize(before.data(), want);
	OrbisLog("[emu] load state %d%s: %s%s", slot, strstr(path.c_str(), ".prev") ? " (previous)" : "", ok ? "ok" : "failed",
		check == StateCheck::Legacy ? " (an older state without a checksum)" : " (checksum ok)");
	if (ok)
		Osd(Fmt(T(strcmp(what, "The state") ? "The previous state of slot %d loaded" : "The state of slot %d loaded"), slot));
	else
		Osd("Could not load the state");
	if (ok)
		RewindClear();
	return ok;
}

bool SaveState(int slot)
{
	if (!g.loaded)
		return false;
	const size_t size = retro_serialize_size();
	std::vector<uint8_t> data(size);
	bool ok = size > 0 && retro_serialize(data.data(), size) && WriteState(StatePath(slot), std::move(data));
	OrbisLog("[emu] save state %d: %s (%zu bytes)", slot, ok ? "ok" : "failed", size);
	if (ok)
		Osd(Fmt(T("State saved to slot %d"), slot));
	else
		Osd("Could not save the state");
	return ok;
}

bool LoadState(int slot)
{
	return g.loaded && LoadStateFile(StatePath(slot), "The state", slot);
}

bool LoadPreviousState(int slot)
{
	return g.loaded && LoadStateFile(StatePath(slot) + ".prev", "The previous state", slot);
}

bool StateExists(int slot)
{
	return g.loaded && FileExists(StatePath(slot));
}

bool PreviousStateExists(int slot)
{
	return g.loaded && FileExists(StatePath(slot) + ".prev");
}

void Reset()
{
	if (!g.loaded)
		return;
	retro_reset();
	RewindClear();
	OrbisLog("[emu] reset");
}

void PowerCycle()
{
	if (!g.loaded)
		return;
	const std::string path = g.path;
	CloseGame();
	std::string error;
	if (!LoadGame(path, &error))
	{
		OrbisLog("[emu] power cycle: %s", error.c_str());
		ProsperoNotify(T("Genesis Plus GX PS5: the game could not be started again. %s"), error.c_str());
	}
}

void RedrawLastFrame()
{
	ps5video::InvalidateFrame(); // what is around the picture is drawn again too (black, or the border)
	DrawLast();
}

FrameResult RunFrame()
{
	if (!g.loaded)
		return FrameResult::Stopped;
	fe::Settings& cfg = fe::Config();

	// -- hot keys (player 1)
	ps5input::Poll();
	const uint32_t raw = ps5input::Pad(0).raw_buttons;
	if (g.wait_release)
	{
		g.wait_release = (raw & (SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3 | SCE_PAD_BUTTON_CROSS | SCE_PAD_BUTTON_CIRCLE)) != 0;
		g.prev_p1 = raw;
	}
	const uint32_t pressed = raw & ~g.prev_p1;
	g.prev_p1 = raw;
	const uint32_t menu_combo = SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3;
	if ((raw & menu_combo) == menu_combo && (pressed & menu_combo))
		return FrameResult::OpenMenu;

	const bool l2 = (raw & SCE_PAD_BUTTON_L2) != 0;
	const bool r2 = (raw & SCE_PAD_BUTTON_R2) != 0;
	if (l2 && !r2)
	{
		if (pressed & SCE_PAD_BUTTON_UP)
			SaveState(cfg.state_slot);
		else if (pressed & SCE_PAD_BUTTON_DOWN)
			LoadState(cfg.state_slot);
		else if (pressed & (SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_RIGHT))
		{
			cfg.state_slot = (pressed & SCE_PAD_BUTTON_RIGHT) ? cfg.state_slot % 10 + 1 : (cfg.state_slot + 8) % 10 + 1;
			Osd(Fmt(T(StateExists(cfg.state_slot) ? "State slot %d (used)" : "State slot %d (empty)"), cfg.state_slot));
			OrbisLog("[emu] state slot %d", cfg.state_slot);
			cfg.Save();
		}
	}
	const bool want_rewind = cfg.rewind && l2 && r2;
	if (want_rewind != g.rewinding)
	{
		g.rewinding = want_rewind;
		OrbisLog("[emu] rewind %s (%zu snapshots, %zu MB)", want_rewind ? "on" : "off", g.rewind.size(),
			g.rewind_bytes >> 20);
	}
	const bool want_turbo = r2 && !l2;
	if (want_turbo != g.turbo)
	{
		g.turbo = want_turbo;
		OrbisLog("[emu] fast forward %s", want_turbo ? "on" : "off");
	}

	// -- emulate
	GENPLUS_STAGE(Emu, "retro_run");
	double core_ms = 0, present_ms = 0;
	if (g.rewinding)
	{
		if (!g.rewind.empty())
		{
			const std::vector<uint8_t>& s = g.rewind.back();
			retro_unserialize(s.data(), s.size());
			g.rewind_bytes -= s.size();
			g.rewind.pop_back();
		}
		g.mute = true;
		retro_run();
		g.mute = false;
		g.in.clear();
	}
	else
	{
		const int runs = g.turbo ? (cfg.ff_speed == 0 ? 8 : std::max(2, (cfg.ff_speed + 50) / 100)) : 1;
		for (int i = 0; i < runs; i++)
		{
			g.mute = g.turbo;
			const double t0 = Now();
			retro_run();
			core_ms += (Now() - t0) * 1000.0;
			if (!g.turbo)
				g_perf.emu_frames += 1.0;
			g.frames++;
			g.fps_frames++;
			if (cfg.rewind && ++g.rewind_tick >= kRewindEvery)
			{
				g.rewind_tick = 0;
				RewindPush();
			}
		}
		g.mute = false;
	}
	FlushAudio();

	// -- picture
	const double now = Now();
	if (now - g.fps_since >= 1.0)
	{
		g.fps_shown = g.fps_frames / (now - g.fps_since);
		g.fps_frames = 0;
		g.fps_since = now;
	}
	const bool pal = g.fps < 55.0;
	if (g.new_frame || (!g.osd.empty() && now < g.osd_until + 0.1) || cfg.show_fps)
	{
		g.new_frame = false;
		const double t_pic = Now();
		fe::border::Reselect(BorderShape()); // a few comparisons: a new picture shape (rare) fits the border again
		ps5video::Rect r = DrawLast();
		ShowOverlays(&r);
		present_ms = (Now() - t_pic) * 1000.0; // the picture's preparation (a CPU shader included), not the vsync wait
		// 60 Hz games wait for the flip: the display paces them
		ps5video::Present(r.x, r.y, r.w, r.h, !pal && !g.turbo);
	}

	// -- pacing on the sound: 50 Hz games, or a ring filling up
	if (!g.turbo && !g.rewinding)
	{
		if (cfg.audio && ps5audio::Available())
		{
			// 50 Hz: the sound is the clock; 60 Hz: vsync is, and this only catches a display that doesn't block
			const int limit = pal ? kTargetQueued : kTargetQueued * 2;
			const double give_up = Now() + 0.1;
			while (ps5audio::Queued() > limit && Now() < give_up)
				usleep(1000);
		}
		else if (pal)
		{
			static double next = 0;
			if (next < now - 0.1)
				next = now;
			next += 1.0 / g.fps;
			if (next > Now())
				usleep(useconds_t((next - Now()) * 1e6));
		}
	}

	// -- battery save, every 3 s when it changed; the log, now and then
	if (now >= g.next_sram_check)
	{
		g.next_sram_check = now + 3.0;
		SaveSram(false);
		SaveBram();
	}
	// a GPU shader being compiled (the first time only: then it comes from the pipeline cache): said on screen
	if (ps5video::ShaderBuilding())
	{
		g.osd = Fmt(T("Preparing the shader %s..."), ps5video::ShaderName(cfg.shader));
		g.osd_until = now + 0.5;
		g.next_shader_check = now + 2.0; // the plain picture's time is not the shader's
		ps5video::TakeShaderMs();
	}
	if (now >= g.next_shader_check)
	{
		// the shader's time per frame, every 2 s; when the game also runs slow, say once which setting helps
		g.next_shader_check = now + 2.0;
		const double ms = ps5video::TakeShaderMs();
		g.last_shader_ms = ms;
		if (ms > 0)
		{
			g.shader_ms_sum += ms;
			g.shader_ms_n++;
		}
		const double budget = 1000.0 / std::max(50.0, g.fps);
		if (!g.shader_warned && !g.turbo && !g.rewinding && cfg.shader != 0 && ms > budget * 0.6 &&
			g.fps_shown > 0 && g.fps_shown < g.fps * 0.95)
		{
			g.shader_warned = true;
			OrbisLog("[emu] the shader %s takes %.1f ms a frame and the game runs at %.1f fps: suggested a lighter one",
				ps5video::ShaderName(cfg.shader), ms, g.fps_shown);
			Osd(Fmt(T("%s is too heavy here: choose a lighter shader (Settings, Shader)"), ps5video::ShaderName(cfg.shader)));
		}
	}
	if (now >= g.next_stats)
	{
		g.stats_reports++;
		g.next_stats = now + (g.stats_reports < 3 ? 5.0 : 60.0);
		const double shader_ms = g.shader_ms_n ? g.shader_ms_sum / g.shader_ms_n : 0.0;
		g.shader_ms_sum = 0;
		g.shader_ms_n = 0;
		const double rate = g.adj_n ? g.adj_sum / double(g.adj_n) : 1.0;
		OrbisLog("[emu] frame %llu, %.1f fps, audio queued %d (%.0f ms; %.0f-%.0f ms), rate x%.4f (%.4f-%.4f), "
				 "underruns %llu, shader %.1f ms",
			(unsigned long long)g.frames, g.fps_shown, ps5audio::Queued(), ps5audio::Queued() * 1000.0 / ps5audio::kRate,
			g.adj_n ? g.queued_min * 1000.0 / ps5audio::kRate : 0.0, g.adj_n ? g.queued_max * 1000.0 / ps5audio::kRate : 0.0,
			rate, g.adj_n ? g.adj_min : 1.0, g.adj_n ? g.adj_max : 1.0, (unsigned long long)ps5audio::Underruns(), shader_ms);
		g.adj_sum = 0;
		g.adj_n = 0;
		g.adj_min = 2;
		g.adj_max = 0;
		g.queued_min = 1 << 30;
		g.queued_max = 0;
	}
	PerfFrame(core_ms, present_ms, g.last_shader_ms);
	return FrameResult::Continue;
}
} // namespace emu

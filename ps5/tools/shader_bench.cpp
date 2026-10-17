// Genesis Plus GX PS5: the CRT shaders' drawing time, measured on this computer (make shader-bench).
//
// The PS5 build draws the picture on the CPU (no GPU driver for homebrew), so a shader's cost is CPU time. This
// runs each shader of coreorbis/orbis-shims/ProsperoCrt.cpp as the app does -- built with the PS5's flags
// (-O3 -march=znver2), with the same thread pool -- on a Mega Drive picture (320x224, a test pattern that changes
// every frame) drawn 4:3 into a 1920x1080 or 1280x720 surface, and prints the average and the slowest frame.
// The frame budget is 16.7 ms at 60 Hz, and the shader shares it with the emulator: the app warns when a shader
// takes more than 60% of it while the game runs slow. A PC is not a PS5 (the PS5's Zen 2 runs at 3.5 GHz with
// eight cores, the app's pool uses up to six threads), so read the numbers as an order: which shaders are cheap,
// which are heavy.
//
//   shader-bench [frames] [720]
//
// SPDX-License-Identifier: MIT
#include "ProsperoCrash.h"
#include "ProsperoCrt.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// the crash log's per-thread hooks (ProsperoCrash.cpp, not linked here): nothing to arm on a PC
namespace crashlog
{
void* ArmThread()
{
	return nullptr;
}
void DisarmThread(void*) {}
} // namespace crashlog

int main(int argc, char** argv)
{
	const int frames = argc > 1 ? std::max(10, atoi(argv[1])) : 300;
	const bool hd720 = argc > 2 && !strcmp(argv[2], "720");
	const int sw = hd720 ? 1280 : 1920, sh = hd720 ? 720 : 1080;
	const int w = 320, h = 224;
	const int dh = sh, dw = dh * 4 / 3, dx = (sw - dw) / 2;
	std::vector<uint32_t> src(size_t(w) * h), surf(size_t(sw) * sh);
	printf("CRT shaders: a %dx%d picture into %dx%d at %dx%d, %d frames, %u hardware threads\n", w, h, dw, dh, sw, sh,
		frames, std::thread::hardware_concurrency());
	printf("%-20s %10s %10s %8s\n", "shader", "avg ms", "max ms", "budget");
	for (int s = 0; s < int(ps5crt::Shader::Count); s++)
	{
		double total = 0, worst = 0;
		for (int f = -10; f < frames; f++) // 10 frames to warm up (tables, maps, the pool)
		{
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
				{
					const uint32_t v = uint32_t(x * 7 + y * 3 + f * 5);
					src[size_t(y) * w + x] = 0xFF000000u | ((v & 0xFF) << 16) | (((x ^ y) & 0xFF) << 8) | ((y * 9 + f) & 0xFF);
				}
			const auto t0 = std::chrono::steady_clock::now();
			ps5crt::Render(ps5crt::Shader(s), src.data(), w, h, surf.data() + dx, sw, dx, 0, dw, dh, uint64_t(f + 10));
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			if (f >= 0)
			{
				total += ms;
				worst = std::max(worst, ms);
			}
		}
		const double avg = total / frames;
		printf("%-20s %10.2f %10.2f %7.0f%%\n", ps5crt::Name(ps5crt::Shader(s)), avg, worst, avg / 16.683 * 100);
	}
	return 0;
}

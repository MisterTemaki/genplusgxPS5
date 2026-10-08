// Genesis Plus GX PS5: sound through libSceAudioOut (S16 stereo, 48 kHz, 256-frame grains, own thread), as in
// PS5SX2's ProsperoAudio. The frontend resamples the core's 44.1 kHz to 48 kHz (steering the rate by the
// ring's fill), so this is only a ring buffer.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace ps5audio
{
constexpr int kRate = 48000;
constexpr int kGrain = 256; // frames per sceAudioOutOutput
constexpr int kCapacity = 16384; // ring size in frames (~340 ms; the frontend keeps ~60 ms queued)

bool Init();
void Shutdown();

// Producer side (the emulation thread). Frames that don't fit are dropped. Returns frames written.
int Push(const int16_t* stereo, int frames);
int Free(); // frames that fit right now
int Queued(); // frames waiting to be played
// Underruns since the start (the audio thread played silence because the ring was empty).
uint64_t Underruns();
// True when the audio port opened (the output thread runs and plays the ring). Without it, nothing drains the ring,
// so the frontend must not use it as a clock.
bool Available();
} // namespace ps5audio

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sound-card samples that the platform records and pushes in (Android: AudioRecord on a thread of the app), for the sound-card source
// (source_audio.cpp, device serial "push"). One input for the whole process; thread-safe; frames are interleaved stereo float, -1..1.
// A mono recording is pushed with both channels equal. Nothing here opens a microphone: the app does, only while a session uses it.
#pragma once
#include <cstddef>
#include <cstdint>

namespace dect2 {

// The rate the app records at (Hz) and a fresh, empty buffer of about `seconds` of audio. Call before the session starts.
void audioPushConfigure(int sampleRate, double seconds = 1.0);
int audioPushRate();                       // 0 until configured
// Appends frames; when the buffer is full the oldest frames are dropped (counted in audioPushDropped). Returns the frames kept.
size_t audioPushWrite(const float* stereo, size_t frames);
// Up to maxFrames frames that have arrived, oldest first; 0 when none.
size_t audioPushRead(float* stereo, size_t maxFrames);
size_t audioPushAvailable();
uint64_t audioPushDropped();
void audioPushReset();                     // empties the buffer (the rate stays)

} // namespace dect2

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/source/audio_push.h"
#include <algorithm>
#include <mutex>
#include <vector>

namespace dect2 {
namespace {
struct Push {
    std::mutex mu;
    std::vector<float> buf;     // ring of stereo frames
    size_t cap = 0, head = 0, count = 0;   // in frames
    int rate = 0;
    uint64_t dropped = 0;
};
Push& P() { static Push p; return p; }
}

void audioPushConfigure(int sampleRate, double seconds) {
    Push& p = P();
    std::lock_guard<std::mutex> lk(p.mu);
    p.rate = sampleRate > 0 ? sampleRate : 0;
    p.cap = (size_t)std::max(1024.0, (double)std::max(sampleRate, 8000) * std::max(0.1, seconds));
    p.buf.assign(p.cap * 2, 0.f);
    p.head = p.count = 0;
    p.dropped = 0;
}

int audioPushRate() { Push& p = P(); std::lock_guard<std::mutex> lk(p.mu); return p.rate; }

size_t audioPushWrite(const float* stereo, size_t frames) {
    Push& p = P();
    std::lock_guard<std::mutex> lk(p.mu);
    if (!p.cap || !stereo) return 0;
    if (frames > p.cap) { p.dropped += frames - p.cap; stereo += (frames - p.cap) * 2; frames = p.cap; }
    const size_t room = p.cap - p.count;
    if (frames > room) {   // drop the oldest
        const size_t d = frames - room;
        p.head = (p.head + d) % p.cap; p.count -= d; p.dropped += d;
    }
    size_t tail = (p.head + p.count) % p.cap;
    for (size_t i = 0; i < frames; i++) {
        p.buf[2 * tail] = stereo[2 * i]; p.buf[2 * tail + 1] = stereo[2 * i + 1];
        if (++tail == p.cap) tail = 0;
    }
    p.count += frames;
    return frames;
}

size_t audioPushRead(float* stereo, size_t maxFrames) {
    Push& p = P();
    std::lock_guard<std::mutex> lk(p.mu);
    const size_t n = std::min(maxFrames, p.count);
    for (size_t i = 0; i < n; i++) {
        stereo[2 * i] = p.buf[2 * p.head]; stereo[2 * i + 1] = p.buf[2 * p.head + 1];
        if (++p.head == p.cap) p.head = 0;
    }
    p.count -= n;
    return n;
}

size_t audioPushAvailable() { Push& p = P(); std::lock_guard<std::mutex> lk(p.mu); return p.count; }
uint64_t audioPushDropped() { Push& p = P(); std::lock_guard<std::mutex> lk(p.mu); return p.dropped; }
void audioPushReset() { Push& p = P(); std::lock_guard<std::mutex> lk(p.mu); p.head = p.count = 0; }

} // namespace dect2

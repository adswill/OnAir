// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/audio/clip_ring.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

namespace dect2 {

namespace {
int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

struct AudioClipRing::Impl {
    std::vector<int16_t> buf;
    size_t cap = 0;
    std::atomic<uint64_t> w{0};          // frames written since the ring (re)started
    std::atomic<uint64_t> start{0};      // first frame of the contiguous part (w - start <= cap)
    std::atomic<int> rate{0};
    std::atomic<const void*> owner{nullptr};
    std::atomic<int64_t> lastMs{0};
    std::atomic<int> inPush{0};
    std::mutex cfgMu;
};

AudioClipRing::AudioClipRing() : p_(new Impl) {}
AudioClipRing::~AudioClipRing() { configure(0); }
AudioClipRing& AudioClipRing::instance() { static AudioClipRing r; return r; }

void AudioClipRing::configure(double seconds) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.cfgMu);
    const size_t want = seconds > 0 ? (size_t)std::lround(std::min(600.0, seconds) * 48000.0) : 0;
    if (want == s.cap && (want > 0) == on_.load()) return;
    on_.store(false, std::memory_order_release);
    while (s.inPush.load(std::memory_order_acquire) > 0) std::this_thread::yield();   // a block in flight finishes first
    s.w = 0; s.start = 0; s.rate = 0; s.owner = nullptr;
    if (want == 0) { std::vector<int16_t>().swap(s.buf); s.cap = 0; return; }
    s.buf.assign(want, 0);
    s.cap = want;
    on_.store(true, std::memory_order_release);
}

size_t AudioClipRing::bytes() const { return p_->buf.size() * sizeof(int16_t); }
int AudioClipRing::rate() const { return p_->rate.load(); }
double AudioClipRing::availableSeconds() const {
    const int r = p_->rate.load();
    if (r <= 0) return 0;
    const uint64_t n = std::min<uint64_t>(p_->w.load() - p_->start.load(), p_->cap);
    return (double)n / r;
}

void AudioClipRing::push(const void* src, const float* stereo, int frames, int rate) {
    Impl& s = *p_;
    s.inPush.fetch_add(1, std::memory_order_acq_rel);
    if (!on_.load(std::memory_order_acquire) || frames <= 0 || rate <= 0) { s.inPush.fetch_sub(1, std::memory_order_release); return; }
    const int64_t now = nowMs();
    const void* o = s.owner.load(std::memory_order_relaxed);
    bool mine = o == src;
    if (!mine) {
        if (o && now - s.lastMs.load(std::memory_order_relaxed) < 2000) { s.inPush.fetch_sub(1, std::memory_order_release); return; }   // another output is the source
        mine = s.owner.compare_exchange_strong(o, src);
    }
    if (mine) {
        const int64_t gap = now - s.lastMs.load(std::memory_order_relaxed);
        const uint64_t w = s.w.load(std::memory_order_relaxed);
        if (s.rate.load(std::memory_order_relaxed) != rate || (s.lastMs.load(std::memory_order_relaxed) != 0 && gap > 1000)) {
            s.start.store(w, std::memory_order_release);   // new rate or a pause: start the contiguous part here
            s.rate.store(rate, std::memory_order_release);
        }
        s.lastMs.store(now, std::memory_order_relaxed);
        const size_t cap = s.cap;
        size_t k = (size_t)(w % cap);
        int16_t* b = s.buf.data();
        for (int i = 0; i < frames; i++) {
            const float m = 0.5f * (stereo[2 * i] + stereo[2 * i + 1]);
            b[k] = (int16_t)(m >= 1.f ? 32767 : m <= -1.f ? -32767 : (int)(m * 32767.f));
            if (++k == cap) k = 0;
        }
        s.w.store(w + (uint64_t)frames, std::memory_order_release);
    }
    s.inPush.fetch_sub(1, std::memory_order_release);
}

bool AudioClipRing::snapshot(double back0, double back1, std::vector<int16_t>& out, int& rate) const {
    const Impl& s = *p_;
    out.clear();
    if (!on_.load(std::memory_order_acquire)) return false;
    rate = s.rate.load(std::memory_order_acquire);
    if (rate <= 0 || back0 < back1 || back1 < 0) return false;
    const uint64_t w0 = s.w.load(std::memory_order_acquire), st = s.start.load(std::memory_order_acquire);
    const uint64_t have = std::min<uint64_t>(w0 - st, s.cap);
    const uint64_t b0 = (uint64_t)std::llround(back0 * rate), b1 = (uint64_t)std::llround(back1 * rate);
    if (b1 >= have) return false;
    const uint64_t from = w0 - std::min(b0, have), to = w0 - b1;   // frames [from, to)
    if (to <= from) return false;
    out.resize((size_t)(to - from));
    for (uint64_t i = from; i < to; i++) out[(size_t)(i - from)] = s.buf[(size_t)(i % s.cap)];
    // the writer may have lapped the oldest part while we copied: drop what could have been overwritten
    const uint64_t w1 = s.w.load(std::memory_order_acquire);
    if (w1 > w0 && w1 - from > s.cap) {
        const uint64_t bad = w1 - from - s.cap;   // frames from the start of the copy that were overwritten
        if (bad >= out.size()) { out.clear(); return false; }
        out.erase(out.begin(), out.begin() + (long)bad);
    }
    return !out.empty();
}

}  // namespace dect2

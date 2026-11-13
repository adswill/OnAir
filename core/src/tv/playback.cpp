// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/tv/playback.h"
#include "dect2/tv/ts_record.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace dect2 {

using Clock = std::chrono::steady_clock;
static double wallNow() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

// ---- TsFileSource ------------------------------------------------------------------------------------------------------------------------------
bool TsFileSource::open(const std::string& path) {
    close();
    FILE* f = u8fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
#ifdef _WIN32
    const long long sz = _ftelli64(f);
#else
    const long long sz = ftello(f);
#endif
    fseek(f, 0, SEEK_SET);
    PcrTimeline tl;
    uint64_t n = sz > 0 ? (uint64_t)sz / 188 : 0;
    std::vector<uint8_t> buf(188 * 4096);
    uint64_t idx = 0;
    int pcrPid = -1;
    size_t got;
    while ((got = fread(buf.data(), 188, 4096, f)) > 0) {
        for (size_t i = 0; i < got; i++, idx++) {
            const uint8_t* p = &buf[i * 188];
            if (p[0] != 0x47) continue;
            double pcr;
            const int pid = ((p[1] & 0x1F) << 8) | p[2];
            if ((pcrPid < 0 || pid == pcrPid) && tsPcrSeconds(p, pcr)) { pcrPid = pid; tl.add(idx, pcr); }
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    f_ = f; n_ = n; tl_ = std::move(tl);
    return true;
}
void TsFileSource::close() {
    std::lock_guard<std::mutex> lk(mu_);
    if (f_) { fclose(f_); f_ = nullptr; }
    n_ = 0; tl_.clear();
}
double TsFileSource::duration() const { std::lock_guard<std::mutex> lk(mu_); return tl_.empty() ? 0 : tl_.lastTime() - tl_.firstTime(); }
size_t TsFileSource::read(uint64_t idx, uint8_t* out, size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!f_ || idx >= n_) return 0;
    n = (size_t)std::min<uint64_t>(n, n_ - idx);
#ifdef _WIN32
    if (_fseeki64(f_, (long long)idx * 188, SEEK_SET) != 0) return 0;
#else
    if (fseeko(f_, (off_t)idx * 188, SEEK_SET) != 0) return 0;
#endif
    return fread(out, 188, n, f_);
}
bool TsFileSource::timeAt(uint64_t idx, double& t) const { std::lock_guard<std::mutex> lk(mu_); if (!tl_.timeAt(idx, t)) { if (tl_.empty()) return false; t = tl_.firstTime(); } return true; }
bool TsFileSource::indexAt(double t, uint64_t& idx) const { std::lock_guard<std::mutex> lk(mu_); return tl_.indexAt(t, idx); }

// ---- PlaybackFeeder ----------------------------------------------------------------------------------------------------------------------------
void PlaybackFeeder::start(PacketSource* src, uint64_t startIdx, Sink sink) {
    stop();
    src_ = src; sink_ = std::move(sink);
    pos_ = std::max(startIdx, src->first()); sent_ = 0;
    paused_ = false; atEnd_ = false; seek_ = false;
    run_ = true;
    th_ = dect2::namedThread("onair-tvplay", [this] { loop(); });
}
void PlaybackFeeder::stop() {
    run_ = false;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}
void PlaybackFeeder::pause(bool on) { paused_ = on; cv_.notify_all(); }
void PlaybackFeeder::seekIndex(uint64_t idx) { { std::lock_guard<std::mutex> lk(mu_); seek_ = true; seekTo_ = idx; } atEnd_ = false; cv_.notify_all(); }
void PlaybackFeeder::seekSeconds(double t) {
    uint64_t idx;
    if (src_ && src_->indexAt(t, idx)) seekIndex(idx);
}
void PlaybackFeeder::skip(double dt) {
    double t;
    if (src_ && src_->timeAt(pos_, t)) seekSeconds(t + dt);
}
void PlaybackFeeder::goLive() { if (src_) seekIndex(src_->end()); }
double PlaybackFeeder::positionSeconds() const { double t = 0; if (src_) src_->timeAt(pos_, t); return t; }
double PlaybackFeeder::behindLiveSeconds() const {
    if (!src_ || !src_->growing()) return 0;
    double a, b;
    if (!src_->timeAt(pos_, a) || !src_->timeAt(src_->end() ? src_->end() - 1 : 0, b)) return 0;
    return std::max(0.0, b - a);
}

void PlaybackFeeder::loop() {
    TsPacer pacer;
    bool anchored = false;
    constexpr size_t kChunk = 64;
    std::vector<uint8_t> buf(188 * kChunk);
    while (run_) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (seek_) { pos_ = std::min(std::max(seekTo_, src_->first()), src_->end()); seek_ = false; anchored = false; }
            if (paused_) { cv_.wait_for(lk, std::chrono::milliseconds(50)); anchored = false; continue; }
        }
        if (pos_ < src_->first()) pos_ = src_->first();   // the buffer dropped what we were about to play
        const size_t got = src_->read(pos_, buf.data(), kChunk);
        if (got == 0) {
            if (!src_->growing()) atEnd_ = true;
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(20));
            continue;
        }
        // pace on the PCRs inside the chunk
        for (size_t i = 0; i < got && run_; i++) {
            double pcr, t;
            if (!tsPcrSeconds(&buf[i * 188], pcr)) continue;
            if (!src_->timeAt(pos_ + i, t)) t = pcr;
            if (!anchored) { pacer.reset(t, wallNow()); anchored = true; }
            double d = pacer.delay(t, wallNow(), speed_);
            while (d > 0 && run_ && !paused_) {
                std::unique_lock<std::mutex> lk(mu_);
                if (seek_) break;
                cv_.wait_for(lk, std::chrono::duration<double>(std::min(d, 0.05)));
                d = pacer.delay(t, wallNow(), speed_);
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (seek_ || !run_) continue;
        }
        if (paused_) continue;   // paused while waiting: nothing is lost, the chunk is read again on resume
        sink_(buf.data(), got);
        pos_ += got;
        sent_ += got;
    }
}

} // namespace dect2

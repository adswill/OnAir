// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Automatic standard detection: the capture from the engine and the worker thread (autodet.h).
#include "dect2/autodet/autodet.h"
#include "dect2/common/thread_name.h"
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace dect2 {
namespace autodet {

struct Capture::Impl {
    mutable std::mutex mu;
    std::condition_variable cv;
    std::thread th;
    bool quit = false, job = false;
    int kind = 0, state = Idle;
    double rate = 0, rf = 0;
    size_t want = 0;
    std::vector<cf32> buf;
    TvOptions tvo;
    TvResult tv; VoiceResult voice;
    uint64_t seq = 0;
    void run() {
        setThreadName("autodetect");
        std::unique_lock<std::mutex> lk(mu);
        for (;;) {
            cv.wait(lk, [&] { return quit || job; });
            if (quit) return;
            job = false;
            std::vector<cf32> x;
            x.swap(buf);
            const int k = kind; const double r = rate, f = rf; const TvOptions o = tvo;
            lk.unlock();
            TvResult t; VoiceResult v;
            if (k == KindTv) t = detectTv(x.data(), x.size(), r, o);
            else { VoiceOptions vo; vo.rfHz = f; v = detectVoice(x.data(), x.size(), r, vo); }
            lk.lock();
            if (state != Working) continue;   // cancelled meanwhile
            if (k == KindTv) tv = std::move(t); else voice = std::move(v);
            seq++;
            state = Done;
        }
    }
};

Capture::Capture() : p_(new Impl) { p_->th = std::thread([this] { p_->run(); }); }
Capture::~Capture() {
    armed_ = false;
    { std::lock_guard<std::mutex> lk(p_->mu); p_->quit = true; }
    p_->cv.notify_all();
    if (p_->th.joinable()) p_->th.join();
}

void Capture::start(int kind, double rateHz, double rfHz, double seconds, const TvOptions& tvo) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->kind = kind; p_->rate = rateHz; p_->rf = rfHz; p_->tvo = tvo;
    if (p_->tvo.rfHz == 0) p_->tvo.rfHz = rfHz;
    p_->want = (size_t)std::min(6e6, std::max(1.0, rateHz * seconds));   // at most 6 M samples (48 MB)
    p_->buf.clear();
    p_->buf.reserve(p_->want);
    p_->state = rateHz > 0 ? Collecting : Idle;
    armed_ = rateHz > 0;
}

void Capture::cancel() {
    armed_ = false;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->state = Idle; p_->buf.clear(); p_->buf.shrink_to_fit();
}

void Capture::push(const cf32* x, size_t n) {
    if (!armed_.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->state != Collecting) return;
    const size_t k = std::min(n, p_->want - p_->buf.size());
    p_->buf.insert(p_->buf.end(), x, x + k);
    if (p_->buf.size() >= p_->want) {
        armed_ = false;
        p_->state = Working; p_->job = true;
        p_->cv.notify_all();
    }
}

int Capture::state() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->state; }
int Capture::kind() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->kind; }
double Capture::progress() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->state == Collecting && p_->want ? (double)p_->buf.size() / (double)p_->want : p_->state >= Working ? 1 : 0;
}

std::string Capture::status() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    const bool tv = p_->kind == KindTv;
    switch (p_->state) {
    case Collecting: return tv ? "listening to the channel..." : "listening for a digital voice signal...";
    case Working: return tv ? "working out the TV standard..." : "looking for the frame syncs of DMR, P25, NXDN, D-STAR, dPMR, Fusion, TETRA, M17...";
    case Done: return tv ? tvSummary(p_->tv) : voiceSummary(p_->voice);
    default: return "";
    }
}

bool Capture::tvResult(TvResult& out, uint64_t& seq) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->kind != KindTv || p_->state != Done || p_->seq == seq) return false;
    out = p_->tv; seq = p_->seq;
    return true;
}
bool Capture::voiceResult(VoiceResult& out, uint64_t& seq) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->kind != KindVoice || p_->state != Done || p_->seq == seq) return false;
    out = p_->voice; seq = p_->seq;
    return true;
}

} // namespace autodet
} // namespace dect2

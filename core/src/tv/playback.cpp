// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/tv/playback.h"
#include "dect2/tv/ts_record.h"
#include "dect2/common/u8file.h"
#include "dect2/t2/t2mi_in.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include "dect2/common/diag.h"

namespace dect2 {

using Clock = std::chrono::steady_clock;
static double wallNow() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

// ---- TsFileSource ------------------------------------------------------------------------------------------------------------------------------
bool TsFileSource::open(const std::string& path) {
    close();
    FILE* f = u8fopen(path, "rb");
    if (!f) { DIAG_ERROR(153077, "The transport stream file %s could not be opened for playback, so nothing is played.", path.c_str()); return false; }
    {   // T2-MI: a modulator feed in the first 64k packets (12 MB) is played as the transport stream of one of its PLPs
        T2miIngest probe;
        std::vector<uint8_t> pb(188 * 4096);
        uint64_t seen = 0;
        size_t got;
        while (seen < 65536 && (got = fread(pb.data(), 188, 4096, f)) > 0) {
            for (size_t i = 0; i < got; i++) probe.feed(&pb[i * 188]);
            seen += got;
        }
        if (probe.found()) {
            fclose(f);
            int best = -1;
            uint64_t most = 0;
            for (int p : probe.plps()) if (best < 0 || probe.packets(p) > most) { best = p; most = probe.packets(p); }
            {
                std::lock_guard<std::mutex> lk(mu_);
                path_ = path; t2miPid_ = probe.pid(); plps_ = probe.plps();
            }
            if (best >= 0 && buildInner(best)) return true;
            DIAG_ERROR(502085, "The T2-MI file %s has no PLP that could be extracted into a transport stream, so it cannot be played.", path.c_str());
            close();
            return false;
        }
        fseek(f, 0, SEEK_SET);
    }
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
    if (!inner_.empty()) { std::error_code ec; std::filesystem::remove(u8file(inner_), ec); inner_.clear(); }
    path_.clear(); t2miPid_ = t2miPlp_ = -1; plps_.clear();
}

bool TsFileSource::selectT2miPlp(int plp) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (t2miPid_ < 0) return false;
        if (plp == t2miPlp_) return true;
    }
    return buildInner(plp);
}

// One pass over the outer file: the T2-MI packets of the PID found by open() become the transport stream of `plp`, written to a temporary file
// with its own PCR time map. The file in use is swapped only when the new one is complete.
bool TsFileSource::buildInner(int plp) {
    std::string src, dir;
    {
        std::lock_guard<std::mutex> lk(mu_);
        src = path_; dir = scratch_;
    }
    FILE* in = u8fopen(src, "rb");
    if (!in) { DIAG_DEBUG(863626, "The T2-MI file %s could not be reopened to extract PLP %d, so the PLP is not played.", src.c_str(), plp); return false; }
    std::error_code ec;
    const std::filesystem::path d = dir.empty() ? std::filesystem::temp_directory_path(ec) : u8file(dir);
    static std::atomic<unsigned> seq{0};
    const unsigned long long stamp = (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "onair_t2mi_" + std::to_string(stamp % 1000000007ULL) + "_" + std::to_string(seq++) + "_plp" + std::to_string(plp) + ".ts";
    const auto u8p = (d / name).u8string();
    const std::string outPath(u8p.begin(), u8p.end());
    FILE* out = u8fopen(outPath, "wb+");
    if (!out) { DIAG_ERROR(160427, "The temporary file %s for T2-MI PLP %d could not be created, so the PLP is not played.", outPath.c_str(), plp); fclose(in); return false; }
    T2miIngest ing;
    PcrTimeline tl;
    uint64_t idx = 0;
    int pcrPid = -1;
    bool werr = false;
    ing.setSink([&](int p, const uint8_t* pk) {
        if (p != plp || werr) return;
        double pcr;
        const int pid = ((pk[1] & 0x1F) << 8) | pk[2];
        if ((pcrPid < 0 || pid == pcrPid) && tsPcrSeconds(pk, pcr)) { pcrPid = pid; tl.add(idx, pcr); }
        if (fwrite(pk, 188, 1, out) != 1) werr = true;
        idx++;
    });
    std::vector<uint8_t> buf(188 * 4096);
    size_t got;
    while ((got = fread(buf.data(), 188, 4096, in)) > 0) for (size_t i = 0; i < got; i++) ing.feed(&buf[i * 188]);
    fclose(in);
    if (werr || idx == 0 || fflush(out) != 0) { DIAG_ERROR(316224, "Extracting T2-MI PLP %d failed (%s), so the PLP is not played.", plp, werr ? "writing the temporary file failed" : idx == 0 ? "the PLP has no packets" : "the temporary file could not be flushed"); fclose(out); std::filesystem::remove(u8file(outPath), ec); return false; }
    fseek(out, 0, SEEK_SET);
    std::lock_guard<std::mutex> lk(mu_);
    if (f_) fclose(f_);
    if (!inner_.empty()) std::filesystem::remove(u8file(inner_), ec);
    f_ = out; n_ = idx; tl_ = std::move(tl); inner_ = outPath; t2miPlp_ = plp; plps_ = ing.plps();
    return true;
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

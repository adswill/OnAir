// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/umts/umts_rx.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/engine.h"
#include "dect2/dsp/resampler.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace dect2 {

ModeTuning umtsTuning() {
    ModeTuning t;
    t.stdMode = 482; t.id = "umts"; t.name = "UMTS cells";
    t.minMhz = 700; t.maxMhz = 2700; t.defMhz = 2112.8;
    t.sampleRate = 7680000; t.basebandHz = 5000000;
    t.bandwidthMhz = 5;
    t.minSampleRate = 3840000;
    return t;
}

struct UmtsReceiver::Impl {
    double fs = 0, offset = 0;
    RationalResampler rs; bool rsOk = false;
    std::vector<cf32> buf, temp; double mixPh = 0;
    const size_t kBlock = 38400 * 2 * 8;              // 80 ms at 7.68 Msps
    int skip = 0;
    std::mutex mu; UmtsTelemetry tel; uint64_t seq = 0;
    std::function<void(const std::string&)> log;
    std::thread th; std::mutex jm; std::condition_variable jc; std::vector<cf32> job; bool have = false, stop = false;
    uint64_t scanNo = 0;
    Impl() { th = std::thread([this] { run(); }); }
    ~Impl() { { std::lock_guard<std::mutex> k(jm); stop = true; } jc.notify_all(); th.join(); }
    void run() {
        for (;;) {
            std::vector<cf32> b;
            { std::unique_lock<std::mutex> k(jm); jc.wait(k, [&] { return have || stop; }); if (stop) return; b.swap(job); have = false; }
            double ms = 0;
            auto found = umtsCellSearch(b.data(), b.size(), 7500, 8, &ms);
            std::lock_guard<std::mutex> k(mu);
            tel.scans = ++scanNo; tel.lastScanMs = ms; tel.state = 1;
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            for (auto& c : found) {
                UmtsCellRow* r = nullptr;
                for (auto& e : tel.cells) if (e.psc == c.scramblingCode) r = &e;
                if (!r) { tel.cells.push_back({}); r = &tel.cells.back(); r->psc = c.scramblingCode; r->group = c.group; if (log) { char m[96]; snprintf(m, sizeof m, "UMTS: cell PSC %d (Ec/Io %.1f dB)", c.scramblingCode, c.ecIoDb); log(m); } }
                r->cfoHz = c.cfoHz; r->ecIoDb = c.ecIoDb; r->rscpDbfs = c.rscpDbfs; r->frameOffset = c.frameOffsetChips; r->blocks++; r->lastSeen = now;
                if (c.mcc >= 0) { r->mcc = c.mcc; r->mnc = c.mnc; r->mncDigits = c.mncDigits; }
                if (c.cellIdentity >= 0) r->cellIdentity = c.cellIdentity;
                r->bchBlocks += (uint64_t)c.bchBlocks;
            }
            std::sort(tel.cells.begin(), tel.cells.end(), [](const UmtsCellRow& a, const UmtsCellRow& b2) { return a.ecIoDb > b2.ecIoDb; });
            tel.dataValid = !tel.cells.empty(); tel.blocksOk = tel.cells.size(); tel.blocksBad = 0; tel.cfoHz = tel.cells.empty() ? 0 : tel.cells[0].cfoHz; tel.snrDb = tel.cells.empty() ? 0.f : (float)tel.cells[0].ecIoDb;
            tel.seq = ++seq;
        }
    }
    void push(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            buf.push_back(x[i]);
            if (buf.size() == kBlock) {
                std::lock_guard<std::mutex> k(jm);
                if (!have) { job.swap(buf); have = true; jc.notify_one(); }
                buf.clear();
                skip = (int)(0.9 * 7.68e6);
            }
        }
    }
};

UmtsReceiver::UmtsReceiver() : p_(new Impl) {}
UmtsReceiver::~UmtsReceiver() = default;
void UmtsReceiver::configure(double fs) { p_->fs = fs; p_->rsOk = p_->rs.configure(fs, 7.68e6); p_->buf.clear(); p_->skip = 0; }
bool UmtsReceiver::ready() const { return p_->fs >= umtsTuning().minSampleRate - 1 && p_->rsOk; }
void UmtsReceiver::setSignalOffset(double hz) { p_->offset = hz; }
void UmtsReceiver::reset() { p_->buf.clear(); p_->mixPh = 0; if (p_->rsOk) p_->rs.reset(); }
void UmtsReceiver::clearResults() { std::lock_guard<std::mutex> k(p_->mu); p_->tel.cells.clear(); p_->tel.seq = ++p_->seq; }
void UmtsReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> k(p_->mu); p_->log = std::move(cb); }
void UmtsReceiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    if (!I.rsOk) return;
    I.temp.assign(x, x + n);
    if (I.offset != 0) { const double w = -2 * 3.14159265358979323846 * I.offset / I.fs; for (auto& v : I.temp) { v *= cf32((float)std::cos(I.mixPh), (float)std::sin(I.mixPh)); I.mixPh = std::fmod(I.mixPh + w, 2 * 3.14159265358979323846); } }
    std::vector<cf32> o; o.reserve(n + 64);
    I.rs.process(I.temp.data(), I.temp.size(), o);
    if (I.skip > 0) { const size_t d = std::min<size_t>((size_t)I.skip, o.size()); I.skip -= (int)d; o.erase(o.begin(), o.begin() + (ptrdiff_t)d); }
    I.push(o.data(), o.size());
}
bool UmtsReceiver::telemetry(UmtsTelemetry& out, uint64_t lastSeq) { std::lock_guard<std::mutex> k(p_->mu); if (p_->tel.seq == lastSeq) return false; out = p_->tel; return true; }

namespace {
class UmtsSynth : public ModeSynth {
public:
    UmtsSynth(const SynthConfig& c, double rate) : rate_(rate > 0 ? rate : 7.68e6), s_(umtsGenerate({{17, c.cfoHz, 1, 500, 0.4, 1, 1, 2, 0x123456}, {280, 2300 + c.cfoHz, 0.7, 9000, 0.3, 1, 1, 2, 0x234567}}, 38400 * 2 * 4, c.snrDb)) { step_ = 7.68e6 / rate_; }
    double sampleRate() const override { return rate_; }
    // the signal is made at 7.68 Msps (2 samples a chip); at another rate it is read at that spacing, 4-point Lagrange (3.9 labelled the 7.68 Msps
    // samples with the new rate, so any other rate was a time-scaled signal no receiver could find). Fine from about 7.68 Msps up.
    void generate(cf32* out, size_t n) override {
        const size_t N = s_.size();
        for (size_t i = 0; i < n; i++) {
            size_t k = (size_t)pos_;
            const float f = (float)(pos_ - (double)k);
            if (f == 0.f) out[i] = s_[k % N] * 0.2f;
            else {
                const cf32 a = s_[(k + N - 1) % N], b = s_[k % N], c = s_[(k + 1) % N], d = s_[(k + 2) % N];
                const float wa = -f * (f - 1) * (f - 2) / 6, wb = (f + 1) * (f - 1) * (f - 2) / 2, wc = -(f + 1) * f * (f - 2) / 2, wd = (f + 1) * f * (f - 1) / 6;
                out[i] = (a * wa + b * wb + c * wc + d * wd) * 0.2f;
            }
            pos_ += step_;
            if (pos_ >= (double)N) pos_ -= (double)N;
        }
    }
private:
    double rate_, step_ = 1, pos_ = 0; std::vector<cf32> s_;
};
}  // namespace

std::unique_ptr<ModeSynth> makeUmtsSynth(const SynthConfig& c, double sampleRate) { return std::make_unique<UmtsSynth>(c, sampleRate); }

} // namespace dect2

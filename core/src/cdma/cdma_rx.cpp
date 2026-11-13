// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// cdma2000 / IS-95 base stations receiver (cdma_rx.h).
#include "dect2/cdma/cdma_rx.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/source/source.h"
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

ModeTuning cdmaTuning() {
    ModeTuning t;
    t.stdMode = 662; t.id = "cdma2000"; t.name = "CDMA2000 cells";
    t.minMhz = 400; t.maxMhz = 2200; t.defMhz = 878.49;   // band class 0, channel 283 (the usual first carrier)
    t.sampleRate = 2457600; t.basebandHz = 1750000;
    t.bandwidthMhz = 1.25;
    t.minSampleRate = 1600000;
    return t;
}

struct CdmaReceiver::Impl {
    double fs = 0, offset = 0, center = 0;
    RationalResampler rs; bool rsOk = false;
    std::vector<cf32> buf, temp; double mixPh = 0;
    const size_t kBlock = (size_t)(cdma::kFs * 0.98);   // 36 PN periods and a little
    std::mutex mu; CdmaTelemetry tel; uint64_t seq = 0;
    std::function<void(const std::string&)> log;
    std::thread th; std::mutex jm; std::condition_variable jc; std::vector<cf32> job; bool have = false, stop = false;
    std::atomic<bool> working{false};
    uint64_t scanNo = 0;
    Impl() { th = std::thread([this] { run(); }); }
    ~Impl() { { std::lock_guard<std::mutex> k(jm); stop = true; } jc.notify_all(); th.join(); }
    void run() {
        for (;;) {
            std::vector<cf32> b;
            { std::unique_lock<std::mutex> k(jm); jc.wait(k, [&] { return have || stop; }); if (stop) return; b.swap(job); have = false; working = true; }
            double ms = 0;
            auto found = cdma::search(b.data(), b.size(), {}, &ms);
            std::lock_guard<std::mutex> k(mu);
            tel.scans = ++scanNo; tel.lastScanMs = ms; tel.state = 1;
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            for (auto& c : found) {
                CdmaCellRow* r = nullptr;
                if (c.hasMsg) for (auto& e : tel.cells) if (e.pilotPn == c.msg.pilotPn) r = &e;
                if (!r && !c.hasMsg) for (auto& e : tel.cells) if (!e.hasMsg && e.relOffset == c.relOffset) r = &e;
                if (!r) {
                    tel.cells.push_back({}); r = &tel.cells.back();
                    if (log) {
                        char m[160];
                        if (c.hasMsg) snprintf(m, sizeof m, "CDMA2000: PN %d, SID %d, NID %d (Ec/Io %.1f dB)", c.msg.pilotPn, c.msg.sid, c.msg.nid, c.ecIoDb);
                        else snprintf(m, sizeof m, "CDMA2000: pilot at +%d x 64 chips (Ec/Io %.1f dB)", c.relOffset, c.ecIoDb);
                        log(m);
                    }
                }
                if (c.hasMsg) { r->hasMsg = true; r->msg = c.msg; r->pilotPn = c.msg.pilotPn; }
                // a pilot first heard without its message and later identified: drop the anonymous row
                r->relOffset = c.relOffset; r->ecIoDb = c.ecIoDb; r->pilotDbfs = c.pilotDbfs; r->cfoHz = c.cfoHz; r->clockPpm = c.clockPpm;
                r->blocks++; r->msgs += (uint64_t)c.msgs; r->crcFails += (uint64_t)c.crcFails; r->lastSeen = now;
            }
            if (found.size()) {
                // anonymous rows whose offset an identified row of this block now has
                tel.cells.erase(std::remove_if(tel.cells.begin(), tel.cells.end(), [&](const CdmaCellRow& e) {
                    if (e.hasMsg) return false;
                    for (auto& c : found) if (c.hasMsg && c.relOffset == e.relOffset) return true;
                    return false; }), tel.cells.end());
            }
            std::sort(tel.cells.begin(), tel.cells.end(), [](const CdmaCellRow& a, const CdmaCellRow& b2) { return a.ecIoDb > b2.ecIoDb; });
            uint64_t ok = 0, bad = 0; for (auto& c : tel.cells) { ok += c.msgs; bad += c.crcFails; }
            tel.dataValid = !tel.cells.empty(); tel.blocksOk = ok; tel.blocksBad = bad;
            tel.cfoHz = tel.cells.empty() ? 0 : tel.cells[0].cfoHz; tel.snrDb = tel.cells.empty() ? 0.f : (float)tel.cells[0].ecIoDb;
            tel.centerHz = center; tel.bandClass = center > 0 ? cdma::cdmaBandClassAt(center) : -1;
            tel.seq = ++seq;
            working = false;
        }
    }
    void push(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            buf.push_back(x[i]);
            if (buf.size() == kBlock) {
                std::lock_guard<std::mutex> k(jm);
                if (!have) { job.swap(buf); have = true; jc.notify_one(); }
                buf.clear();
            }
        }
    }
};

CdmaReceiver::CdmaReceiver() : p_(new Impl) {}
CdmaReceiver::~CdmaReceiver() = default;
void CdmaReceiver::configure(double fs) { p_->fs = fs; p_->rsOk = p_->rs.configure(fs, cdma::kFs, nullptr, 1.3e6); p_->buf.clear(); }
bool CdmaReceiver::ready() const { return p_->fs >= cdmaTuning().minSampleRate - 1 && p_->rsOk; }
void CdmaReceiver::setSignalOffset(double hz) { p_->offset = hz; }
void CdmaReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> k(p_->mu); p_->center = hz; }
void CdmaReceiver::reset() { p_->buf.clear(); p_->mixPh = 0; if (p_->rsOk) p_->rs.reset(); }
void CdmaReceiver::clearResults() { std::lock_guard<std::mutex> k(p_->mu); p_->tel.cells.clear(); p_->tel.seq = ++p_->seq; }
void CdmaReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> k(p_->mu); p_->log = std::move(cb); }
bool CdmaReceiver::busy() const { std::lock_guard<std::mutex> k(p_->jm); return p_->have || p_->working; }
void CdmaReceiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    if (!I.rsOk) return;
    I.temp.assign(x, x + n);
    if (I.offset != 0) {
        const double w = -2 * M_PI * I.offset / I.fs;
        for (auto& v : I.temp) { v *= cf32((float)std::cos(I.mixPh), (float)std::sin(I.mixPh)); I.mixPh = std::fmod(I.mixPh + w, 2 * M_PI); }
    }
    std::vector<cf32> o; o.reserve(n + 64);
    I.rs.process(I.temp.data(), I.temp.size(), o);
    I.push(o.data(), o.size());
}
bool CdmaReceiver::telemetry(CdmaTelemetry& out, uint64_t lastSeq) { std::lock_guard<std::mutex> k(p_->mu); if (p_->tel.seq == lastSeq) return false; out = p_->tel; return true; }

namespace {
// The built-in test signal: two base stations (PN 168 and PN 12) of one system, about 2 s long and looped (the loop restarts the frames,
// which the receiver sees as a new block).
class CdmaSynth : public ModeSynth {
public:
    CdmaSynth(const SynthConfig& c, double rate) : rate_(rate > 0 ? rate : cdma::kFs) {
        std::vector<cdma::TxCell> cells(2);
        cells[0].msg.sid = 4139; cells[0].msg.nid = 1; cells[0].msg.pilotPn = 168; cells[0].msg.cdmaFreq = 283; cells[0].msg.extCdmaFreq = 283;
        cells[0].msg.sysTime = 18484000000ULL;   // late 2026, 80 ms units cells[0].msg.lpSec = 18; cells[0].msg.ltmOff = 4; cells[0].msg.lcState = 0x123456789ULL; cells[0].msg.minPRev = 6;
        cells[1] = cells[0]; cells[1].msg.pilotPn = 12; cells[1].amp = 0.6; cells[1].cfoHz = 150;
        cells[0].cfoHz = c.cfoHz; cells[1].cfoHz += c.cfoHz;
        std::vector<cf32> s = cdma::generate(cells, (size_t)(cdma::kFs * 2), c.snrDb, 5000);
        if (std::fabs(rate_ - cdma::kFs) > 1) {
            RationalResampler r;
            if (r.configure(cdma::kFs, rate_)) { std::vector<cf32> o; r.process(s.data(), s.size(), o); s.swap(o); }
            else rate_ = cdma::kFs;
        }
        double p = 0; for (auto& v : s) p += std::norm(v);
        const float g = (float)(0.22 / std::sqrt(p / std::max<size_t>(1, s.size())));
        for (auto& v : s) v *= g;
        s_.swap(s);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { for (size_t i = 0; i < n; i++) { out[i] = s_[pos_]; pos_ = (pos_ + 1) % s_.size(); } }
private:
    double rate_; std::vector<cf32> s_; size_t pos_ = 0;
};
}  // namespace

std::unique_ptr<ModeSynth> makeCdmaSynth(const SynthConfig& c, double sampleRate) { return std::make_unique<CdmaSynth>(c, sampleRate); }

} // namespace dect2

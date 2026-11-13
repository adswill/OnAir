// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP transmit generator: see kapp_txgen.h.
#include "dect2/kapp/kapp_txgen.h"
#include "dect2/kapp/kapp_arq.h"
#include "dect2/kapp/kapp_frame.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace dect2 {

namespace {

constexpr double kBurstMs = 1000;       // airtime of one data burst: the longest the receiver accepts (kapp_rx.cpp)
constexpr double kGapSec = 0.03;        // silence between two bursts
constexpr double kPassGapSec = 0.4;     // silence between two passes
constexpr float kLevel = 0.35f;         // output amplitude: keeps the peaks of the high QAM profiles clear of the limiter

size_t blocksPerBurst(const kapp::Profile& P) { return (size_t)std::max(1, std::min(kapp::kMaxBlocks, kapp::maxBlocksForMs(P, kBurstMs))); }
size_t dataCap(const kapp::Profile& P) { return std::min<size_t>((size_t)P.payloadBytes - kapp::kArqBlockHeader, 255); }

class KappGen : public TxGenerator {
public:
    KappGen(double rate, std::vector<uint8_t> msg, std::string call, int profile, int passes)
        : rate_(rate), msg_(std::move(msg)), call_(std::move(call)), profile_(profile), passes_(passes) {}
    double sampleRate() const override { return rate_; }
    bool finished() const override { return done_ && pos_ >= cur_.size(); }
    void generate(cf32* out, size_t n) override {
        size_t have = 0;
        while (have < n) {
            if (pos_ >= cur_.size()) {
                if (done_) { std::fill(out + have, out + n, cf32(0.f, 0.f)); return; }
                nextSegment();
                continue;
            }
            const size_t k = std::min(n - have, cur_.size() - pos_);
            std::copy(cur_.begin() + (long)pos_, cur_.begin() + (long)(pos_ + k), out + have);
            pos_ += k;
            have += k;
        }
    }

private:
    void silence(double sec) {
        cur_.assign((size_t)std::max(1.0, sec * rate_), cf32(0.f, 0.f));
        pos_ = 0;
    }
    void nextSegment() {
        const kapp::Profile* P = kapp::profileById(profile_);
        if (offset_ >= msg_.size()) {   // the pass is over
            pass_++;
            offset_ = 0;
            if (pass_ >= passes_) done_ = true;
            silence(kPassGapSec);
            return;
        }
        const size_t cap = (size_t)P->payloadBytes, dc = dataCap(*P);
        kapp::BurstSpec spec;
        spec.callsign = call_;
        spec.profileId = profile_;
        bool hasLast = false;
        for (size_t b = 0; b < blocksPerBurst(*P) && offset_ < msg_.size(); b++) {
            const size_t len = std::min(dc, msg_.size() - offset_);
            const bool last = offset_ + len == msg_.size();
            const size_t at = spec.payload.size();
            spec.payload.resize(at + cap);
            kapp::packArqBlock((uint32_t)offset_, last, msg_.data() + offset_, len, spec.payload.data() + at, cap);
            offset_ += len;
            hasLast = hasLast || last;
        }
        spec.flags = hasLast ? kapp::kFlagFinal : 0;   // one-way: no ACK is requested
        seq_ = (seq_ + 1) & 63;
        spec.seq = seq_;
        std::vector<kapp::cf> iq = kapp::buildBurst(spec);
        if (std::fabs(rate_ - kapp::kInternalRate) > 0.5) iq = kapp::resample(iq, kapp::kInternalRate, rate_);
        const size_t gap = (size_t)(kGapSec * rate_);
        cur_.assign(gap + iq.size() + gap, cf32(0.f, 0.f));
        for (size_t i = 0; i < iq.size(); i++) cur_[gap + i] = iq[i] * kLevel;
        pos_ = 0;
    }

    double rate_;
    std::vector<uint8_t> msg_;
    std::string call_;
    int profile_, passes_;
    int pass_ = 0, seq_ = 0;
    size_t offset_ = 0;
    bool done_ = false;
    std::vector<cf32> cur_;
    size_t pos_ = 0;
};

} // namespace

bool kappFromRequest(const TxRequest& r, std::vector<uint8_t>& msg, std::string& callsign, int& profile, int& passes, std::string& err) {
    callsign.clear();
    for (char c : r.callsign) callsign += (char)std::toupper((unsigned char)c);
    while (!callsign.empty() && callsign.back() == ' ') callsign.pop_back();
    if (callsign.empty()) callsign = "ONAIR";
    if (callsign.size() > 7) { err = "KAPP: the callsign is at most 7 characters"; return false; }
    for (char c : callsign) {
        if (!(c == '/' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) { err = "KAPP: the callsign may hold only letters, digits and /"; return false; }
    }
    profile = (int)std::lround(r.o("kapp.profile", 2));
    if (profile < 0 || profile > kapp::kMaxProfileId) { err = "KAPP: the profile is 0 to 8"; return false; }
    passes = (int)std::lround(r.o("kapp.repeat", 2));
    if (passes < 1 || passes > 9) { err = "KAPP: the number of passes is 1 to 9"; return false; }
    msg.clear();
    if (!r.file.empty()) {
        std::ifstream in(r.file, std::ios::binary);
        if (!in) { err = "KAPP: the file cannot be opened: " + r.file; return false; }
        msg.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (msg.empty()) { err = "KAPP: the file is empty"; return false; }
        if (msg.size() > kapp::kArqMaxMessage) { err = "KAPP: a message is at most 8 MiB"; return false; }
    } else {
        const std::string t = r.text.empty() ? std::string("OnAir KAPP test message: packet data in bursts with selective repeat.") : r.text;
        msg.assign(t.begin(), t.end());
        if (msg.size() > kapp::kArqMaxMessage) { err = "KAPP: a message is at most 8 MiB"; return false; }
    }
    return true;
}

double kappAirtimeSeconds(size_t messageBytes, int profile, int passes) {
    const kapp::Profile* P = kapp::profileById(profile);
    if (!P || messageBytes == 0) return 0;
    const size_t dc = dataCap(*P), nBlocks = (messageBytes + dc - 1) / dc, per = blocksPerBurst(*P);
    const size_t nBursts = (nBlocks + per - 1) / per;
    double syms = 0;
    for (size_t left = nBlocks; left > 0;) {
        const size_t b = std::min(left, per);
        syms += kapp::burstSymbols(*P, (int)b);
        left -= b;
    }
    const double one = syms / kapp::kSymRate + (double)nBursts * 2 * kGapSec + kPassGapSec;
    return one * std::max(1, passes);
}

std::unique_ptr<TxGenerator> makeKappTxGenerator(const TxRequest& r, double rateHz, std::string& err) {
    if (rateHz < 125000.0 - 1) { err = "KAPP needs a transmit rate of at least 125 kHz"; return nullptr; }
    std::vector<uint8_t> msg;
    std::string call;
    int profile = 2, passes = 2;
    if (!kappFromRequest(r, msg, call, profile, passes, err)) return nullptr;
    return std::make_unique<KappGen>(rateHz, std::move(msg), call, profile, passes);
}

} // namespace dect2

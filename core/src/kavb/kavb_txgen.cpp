// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB transmit generator: see kavb_txgen.h.
#include "dect2/kavb/kavb_txgen.h"
#include "dect2/kavb/kavb_av.h"
#include "dect2/kavb/kavb_phy.h"
#include "dect2/dsp/exact_resampler.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iterator>

namespace dect2 {

namespace {

constexpr float kLevel = 0.25f;                 // output amplitude: keeps the peaks of 64-QAM and the sweep clear of the limiter
constexpr int kEdgeSyms = 8;                    // lead and tail of a chunk, overlapped with its neighbours (the matched filter rings 6 symbols)
constexpr int kLeadSilenceSyms = 96;            // silence before the first and after the last frame
constexpr size_t kMaxFile = (size_t)64 << 20;
constexpr size_t kFilePacketBytes = 1200;

class KavbGen : public TxGenerator {
public:
    KavbGen(double rate, const KavbTxPlan& plan, double rampSec)
        : rate_(rate), plan_(plan), p_(kavb::profileOf(plan.profile, false)), tx_(plan.profile, false, true), pk_(p_, plan.fec) {
        // the output stage fades the key-up in over out.rampMs (2 ms by default): the lead silence must cover it with room to spare,
        // otherwise the fade lands on the first frame (one group of a file lost)
        leadSyms_ = std::max(kLeadSilenceSyms, (int)std::ceil((std::max(0.0, rampSec) + 0.5e-3) * p_.symRate));
        rs_.configure(p_.symRate * kavb::kSps, rate);
        maxPay_ = std::min(kFilePacketBytes, pk_.maxPayload());
        idEveryFrames_ = std::max<long>(1, (long)std::lround(0.5 / kavb::frameSeconds(p_)));
        if (plan.file.empty()) {
            const kavb::BitBudget bb = kavb::bitBudget(p_, plan.fec);
            syn_ = std::make_unique<kavb::SyntheticAv>(std::max(50e3, 0.8 * bb.forVideo), 30.0, 96000.0, maxPay_);
        }
    }
    double sampleRate() const override { return rate_; }
    bool finished() const override { return done_ && pos_ >= q_.size(); }
    void generate(cf32* out, size_t n) override {
        size_t have = 0;
        while (have < n) {
            if (pos_ >= q_.size()) {
                if (done_) { std::fill(out + have, out + n, cf32(0.f, 0.f)); return; }
                q_.clear();
                pos_ = 0;
                nextChunk();
                continue;
            }
            const size_t k = std::min(n - have, q_.size() - pos_);
            std::copy(q_.begin() + (long)pos_, q_.begin() + (long)(pos_ + k), out + have);
            pos_ += k;
            have += k;
        }
    }

private:
    bool nextPacket(kavb::AvPacket& a) {
        if (syn_) return syn_->timeSec() < plan_.seconds && syn_->next(a);
        if (off_ >= plan_.file.size()) return false;
        const size_t len = std::min(maxPay_, plan_.file.size() - off_);
        a = kavb::AvPacket();
        a.type = kavb::PktType::Data;
        a.stream = 1;
        a.seq = fileSeq_++;
        a.payload.assign(plan_.file.begin() + (long)off_, plan_.file.begin() + (long)(off_ + len));
        off_ += len;
        return true;
    }

    void emit(const std::vector<kavb::cf>& s, bool last) {
        // s is a part of the 4 samples per symbol stream; the resampler brings it to the radio rate
        std::vector<cf32> o;
        rs_.process(s.data(), s.size(), o);
        if (last) {   // flush the filter with silence
            std::vector<cf32> z((size_t)(kLeadSilenceSyms * kavb::kSps), cf32(0.f, 0.f));
            rs_.process(z.data(), z.size(), o);
        }
        for (cf32& c : o) c *= kLevel;
        q_.insert(q_.end(), o.begin(), o.end());
    }

    void nextChunk() {
        std::vector<std::vector<uint8_t>> frames;
        bool last = false;
        while (frames.empty()) {
            frames = pk_.takeFrames(false);
            if (!frames.empty()) break;
            kavb::AvPacket a;
            if (!sentId_ || pk_.framesEmitted() - idAtFrame_ >= idEveryFrames_) {
                sentId_ = true;
                idAtFrame_ = pk_.framesEmitted();
                kavb::AvPacket id;
                id.type = kavb::PktType::Text;
                id.stream = 0;
                id.seq = idSeq_++;
                const std::string t = "KAVB de " + plan_.callsign;
                id.payload.assign(t.begin(), t.end());
                pk_.push(id);
                continue;
            }
            if (nextPacket(a)) { pk_.push(a); continue; }
            frames = pk_.takeFrames(true);
            last = true;
            break;
        }
        if (frames.empty()) {   // the last chunk ended on a group boundary: send what is held back
            if (started_) emit(pend_, true);
            done_ = true;
            return;
        }
        const long first = framesOut_;
        framesOut_ += (long)frames.size();
        const std::vector<kavb::cf> out = tx_.modulate(frames, plan_.fec, 0, (int)(first & 0xFFFF), (int)(first % p_.sweepFrames), kEdgeSyms, kEdgeSyms);
        const size_t e2 = (size_t)(2 * kEdgeSyms * kavb::kSps);
        std::vector<kavb::cf> comb = out;
        if (started_) {
            for (size_t i = 0; i < e2 && i < comb.size() && i < pend_.size(); i++) comb[i] += pend_[i];
        } else {
            started_ = true;
            emit(std::vector<kavb::cf>((size_t)leadSyms_ * kavb::kSps, kavb::cf(0.f, 0.f)), false);
        }
        if (last) {
            emit(comb, true);
            done_ = true;
            return;
        }
        const size_t keep = std::min(e2, comb.size());
        pend_.assign(comb.end() - (long)keep, comb.end());
        comb.resize(comb.size() - keep);
        emit(comb, false);
    }

    double rate_;
    KavbTxPlan plan_;
    kavb::Profile p_;
    kavb::Transmitter tx_;
    kavb::Packetiser pk_;
    ExactResampler rs_;
    std::unique_ptr<kavb::SyntheticAv> syn_;
    size_t maxPay_ = 1000, off_ = 0;
    uint16_t fileSeq_ = 0, idSeq_ = 0;
    bool sentId_ = false, started_ = false, done_ = false;
    int leadSyms_ = kLeadSilenceSyms;
    long idAtFrame_ = 0, idEveryFrames_ = 100, framesOut_ = 0;
    std::vector<kavb::cf> pend_;
    std::vector<cf32> q_;
    size_t pos_ = 0;
};

} // namespace

double kavbSymRate(int profile) { return kavb::profileOf(std::max(0, std::min(3, profile)), false).symRate; }
double kavbMinRate(int profile) { return 1.6 * kavbSymRate(profile); }

bool kavbFromRequest(const TxRequest& r, KavbTxPlan& plan, std::string& err) {
    plan = KavbTxPlan();
    for (char c : r.callsign) plan.callsign += (char)std::toupper((unsigned char)c);
    while (!plan.callsign.empty() && plan.callsign.back() == ' ') plan.callsign.pop_back();
    if (plan.callsign.empty()) plan.callsign = "ONAIR";
    if (plan.callsign.size() > 7) { err = "KAVB: the callsign is at most 7 characters"; return false; }
    for (char c : plan.callsign) {
        if (!(c == '/' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) { err = "KAVB: the callsign may hold only letters, digits and /"; return false; }
    }
    plan.profile = (int)std::lround(r.o("kavb.profile", 0));
    if (plan.profile < 0 || plan.profile >= kavb::kNumProfiles) { err = "KAVB: the profile is A to D"; return false; }
    plan.fec = (int)std::lround(r.o("kavb.fec", 1));
    if (plan.fec < 0 || plan.fec >= kavb::kNumFecProfiles) { err = "KAVB: the FEC profile is 0 to 7"; return false; }
    plan.seconds = r.o("kavb.seconds", 5);
    if (!(plan.seconds >= 1 && plan.seconds <= 600)) { err = "KAVB: the test stream is 1 to 600 seconds"; return false; }
    if (!r.file.empty()) {
        std::ifstream in(r.file, std::ios::binary);
        if (!in) { err = "KAVB: the file cannot be opened: " + r.file; return false; }
        plan.file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (plan.file.empty()) { err = "KAVB: the file is empty"; return false; }
        if (plan.file.size() > kMaxFile) { err = "KAVB: a file is at most 64 MiB"; return false; }
    }
    return true;
}

double kavbAirtimeSeconds(size_t fileBytes, int profile, int fec, double syntheticSeconds) {
    const kavb::Profile p = kavb::profileOf(std::max(0, std::min(3, profile)), false);
    const kavb::BitBudget bb = kavb::bitBudget(p, std::max(0, std::min(kavb::kNumFecProfiles - 1, fec)));
    double bits;
    if (fileBytes > 0) bits = (double)fileBytes * 8.0 * (kFilePacketBytes + kavb::kPacketOverhead + 1.0) / kFilePacketBytes;
    else bits = syntheticSeconds * (0.8 * bb.forVideo + 96000.0) * 1.05;
    return bits / std::max(1.0, bb.forPackets) + 0.5;
}

std::unique_ptr<TxGenerator> makeKavbTxGenerator(const TxRequest& r, double rateHz, std::string& err) {
    KavbTxPlan plan;
    if (!kavbFromRequest(r, plan, err)) return nullptr;
    if (rateHz < kavbMinRate(plan.profile) - 1) {
        char b[160];
        snprintf(b, sizeof b, "KAVB profile %c needs a transmit rate of at least %.2f MHz", 'A' + plan.profile, kavbMinRate(plan.profile) / 1e6);
        err = b;
        return nullptr;
    }
    return std::make_unique<KavbGen>(rateHz, plan, r.o("out.rampMs", kTxDefaultRampMs) * 1e-3);
}

} // namespace dect2

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS transmit generator: see kaws_txgen.h.
#include "dect2/kaws/kaws_txgen.h"
#include "dect2/kaws/kaws_frame.h"
#include "dect2/kaws/kaws_phy.h"
#include "dect2/kaws/kaws_sched.h"
#include "dect2/kapp/kapp_frame.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

namespace dect2 {

namespace {

constexpr float kLevel = 0.6f;           // peak of the audio as a fraction of full scale: leaves the limiter alone
constexpr double kTailSec = 0.5;

double systemUtc() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// One burst waiting to go out
struct Item {
    int profile = 0;
    std::vector<kaws::Bits> blocks;
};

class KawsGen : public TxGenerator {
public:
    KawsGen(double rate, std::vector<Item> items, double utcStart, bool align)
        : rate_(rate), items_(std::move(items)), now_(utcStart), align_(align) {}
    double sampleRate() const override { return rate_; }
    bool finished() const override { return done_ && mode_ == 0 && left_ == 0; }
    void generate(cf32* out, size_t n) override {
        size_t have = 0;
        while (have < n) {
            if (mode_ == 0 && left_ == 0) {
                if (done_) { std::fill(out + have, out + n, cf32(0.f, 0.f)); return; }
                next();
                continue;
            }
            if (mode_ == 1) {   // silence
                const size_t k = (size_t)std::min<uint64_t>(left_, n - have);
                std::fill(out + have, out + have + k, cf32(0.f, 0.f));
                have += k; left_ -= k;
                if (left_ == 0) mode_ = 0;
                continue;
            }
            // a burst: through the resampler in pieces
            if (obPos_ >= ob_.size()) {
                if (!refill()) { mode_ = 0; left_ = 0; continue; }
            }
            const size_t k = std::min(n - have, ob_.size() - obPos_);
            std::copy(ob_.begin() + (long)obPos_, ob_.begin() + (long)(obPos_ + k), out + have);
            obPos_ += k; have += k;
        }
    }

private:
    void silence(double sec) {
        mode_ = 1;
        left_ = (uint64_t)std::max(0LL, std::llround(sec * rate_));
        if (left_ == 0) mode_ = 0;
        now_ += sec;
    }
    bool refill() {
        ob_.clear(); obPos_ = 0;
        if (rs_) {
            while (ob_.empty() && inPos_ < an_.size()) {
                const size_t k = std::min<size_t>(1200, an_.size() - inPos_);
                std::vector<kapp::cf> in(an_.begin() + (long)inPos_, an_.begin() + (long)(inPos_ + k));
                inPos_ += k;
                ob_ = rs_->process(in.data(), in.size());
            }
            if (ob_.empty() && !flushed_) { flushed_ = true; ob_ = rs_->flush(); }
        } else if (inPos_ < an_.size()) {
            ob_.assign(an_.begin() + (long)inPos_, an_.end());
            inPos_ = an_.size();
        }
        return !ob_.empty();
    }
    void next() {
        if (idx_ >= items_.size()) {
            if (!tailDone_) { tailDone_ = true; silence(kTailSec); return; }
            done_ = true;
            return;
        }
        const Item& it = items_[idx_];
        // wait for the next caller slot (even) whose transmit start is not in the past
        if (!sentSilence_) {
            sentSilence_ = true;
            const kaws::SlotGrid g(kaws::kProfiles[it.profile].slotSec);
            if (idx_ > 0 || align_) {
                int64_t s = g.index(now_ - kaws::kTxOffsetSec);
                if (g.txStart(s) < now_ - 1e-6) s++;
                if (!g.callerSlot(s)) s++;
                const double wait = g.txStart(s) - now_;
                if (wait > 1e-6) { silence(wait); return; }
            }
        }
        sentSilence_ = false;
        idx_++;
        const std::vector<kaws::cf> iq = kaws::buildBurst(it.profile, it.blocks);
        std::vector<float> audio = kaws::toAudio12k(iq, kLevel);
        an_ = kawsAnalytic(audio);
        now_ += (double)an_.size() / kaws::kAudioRate;
        inPos_ = 0; flushed_ = false;
        rs_.reset();
        if (std::fabs(rate_ - kaws::kAudioRate) > 0.5) rs_ = std::make_unique<kapp::Resampler>((double)kaws::kAudioRate, rate_);
        mode_ = 2;
        ob_.clear(); obPos_ = 0;
        left_ = 1;   // the burst runs until refill() has nothing more
    }

    double rate_;
    std::vector<Item> items_;
    double now_;
    bool align_;
    size_t idx_ = 0;
    bool done_ = false, tailDone_ = false, sentSilence_ = false, flushed_ = false;
    int mode_ = 0;           // 0 choose, 1 silence, 2 burst
    uint64_t left_ = 0;
    std::vector<kapp::cf> an_, ob_;
    size_t inPos_ = 0, obPos_ = 0;
    std::unique_ptr<kapp::Resampler> rs_;
};

}  // namespace

std::vector<cf32> kawsAnalytic(const std::vector<float>& audio) {
    size_t n = 1;
    while (n < audio.size() + 4096) n <<= 1;
    std::vector<cf32> x(n, cf32(0.f, 0.f));
    for (size_t i = 0; i < audio.size(); i++) x[i] = cf32(audio[i], 0.f);
    Fft fft((int)n);
    fft.forward(x.data());
    for (size_t k = 1; k < n / 2; k++) x[k] *= 2.f;
    for (size_t k = n / 2 + 1; k < n; k++) x[k] = cf32(0.f, 0.f);
    fft.inverse(x.data());
    const float g = 1.f / (float)n;
    x.resize(audio.size());
    for (cf32& v : x) v *= g;
    return x;
}

bool kawsFromRequest(const TxRequest& r, KawsTxPlan& plan, std::string& err) {
    using namespace kaws;
    std::string cs;
    for (char c : r.callsign) cs += (char)std::toupper((unsigned char)c);
    while (!cs.empty() && cs.back() == ' ') cs.pop_back();
    if (cs.empty()) cs = "ONAIR";
    if (!callsignValid(cs)) { err = "KAWS: the callsign is 1 to 9 characters of letters, digits, - / and ."; return false; }
    plan.callsign = cs;
    plan.profile = (int)std::lround(r.o("kaws.profile", 3));
    if (plan.profile == 6 || plan.profile == 7) { err = "KAWS: voice profiles need a speech codec that the user installs (docs/CODECS.md); voice codec not installed"; return false; }
    if (plan.profile < 0 || plan.profile > 5) { err = "KAWS: the profile is 0 to 5"; return false; }
    plan.kind = (int)std::lround(r.o("kaws.kind", 0));
    if (plan.kind < 0 || plan.kind > 2) { err = "KAWS: the message kind is 0 chat, 1 short beacon, 2 full beacon"; return false; }
    plan.passes = (int)std::lround(r.o("kaws.repeat", 2));
    if (plan.passes < 1 || plan.passes > 9) { err = "KAWS: the number of passes is 1 to 9"; return false; }
    const double mid = r.o("kaws.msgid", -1);
    plan.msgId = mid >= 0 ? (int)std::lround(mid) & 15 : (int)((long long)systemUtc() / 7) & 15;
    std::string grid = r.grid.empty() ? std::string("AA00AA") : r.grid;
    for (char& c : grid) c = (char)std::toupper((unsigned char)c);
    uint32_t gv = 0;
    if (!gridEncode(grid, gv)) { err = "KAWS: the grid is 6 characters like KO24RM"; return false; }
    const int eirpStep = std::max(0, std::min(255, (int)std::lround((r.o("kaws.eirp", 30) + 40) * 2)));
    Bits msg;
    if (plan.kind == 0) {
        Chat c;
        c.from = cs;
        c.to = r.dest.empty() ? std::string("CQ") : r.dest;
        for (char& ch : c.to) ch = (char)std::toupper((unsigned char)ch);
        if (!callsignValid(c.to)) { err = "KAWS: the recipient is 1 to 9 characters of letters, digits, - / and ."; return false; }
        c.firstMessage = true;
        c.text = r.text.empty() ? std::string("OnAir KAWS test message") : r.text;
        if (c.text.size() > 100) { err = "KAWS: a chat message is at most 100 characters"; return false; }
        msg = encodeChat(c);
    } else if (plan.kind == 1) {
        BeaconShort b;
        b.call = cs; b.grid = grid;
        b.powerStep = std::min(63, eirpStep);   // the short beacon's 6-bit field spans -40 .. -8.5 dBm
        b.linkQuality = 0;
        msg = encodeBeaconShort(b);
    } else {
        BeaconFull b;
        b.call = cs; b.grid = grid;
        b.lat = r.lat; b.lon = r.lon;
        b.powerStep = eirpStep;
        b.text = r.text.size() > 10 ? r.text.substr(0, 10) : r.text;
        msg = encodeBeaconFull(b);
    }
    if (msg.empty()) { err = "KAWS: the message cannot be coded"; return false; }
    const std::vector<Bits> blocks = splitMessage(kProfiles[plan.profile].K, (uint8_t)plan.msgId, msg);
    if (blocks.empty()) { err = "KAWS: the message needs more than 16 blocks at this profile"; return false; }
    plan.message = msg;
    plan.blocks = (int)blocks.size();
    const int per = std::max(1, blocksPerSlot(plan.profile));
    plan.slotsPerPass = (plan.blocks + per - 1) / per;
    return true;
}

double kawsAirtimeSeconds(const KawsTxPlan& plan) {
    using namespace kaws;
    const int per = std::max(1, blocksPerSlot(plan.profile));
    double total = 0;
    int slots = 0;
    for (int pass = 0; pass < plan.passes; pass++) {
        for (int left = plan.blocks; left > 0; left -= per) {
            const int b = std::min(left, per);
            total = (double)slots * 2 * kProfiles[plan.profile].slotSec + kTxOffsetSec + (double)burstSamples(plan.profile, b) / kBaseRate;
            slots++;
        }
    }
    return total;
}

std::unique_ptr<TxGenerator> makeKawsTxGenerator(const TxRequest& r, double rateHz, std::string& err) {
    if (rateHz < kaws::kAudioRate - 1) { err = "KAWS needs a transmit rate of at least 12 kHz"; return nullptr; }
    KawsTxPlan plan;
    if (!kawsFromRequest(r, plan, err)) return nullptr;
    const std::vector<kaws::Bits> blocks = kaws::splitMessage(kaws::kProfiles[plan.profile].K, (uint8_t)plan.msgId, plan.message);
    const int per = std::max(1, kaws::blocksPerSlot(plan.profile));
    std::vector<Item> items;
    for (int pass = 0; pass < plan.passes; pass++) {
        for (size_t at = 0; at < blocks.size(); at += (size_t)per) {
            Item it;
            it.profile = plan.profile;
            it.blocks.assign(blocks.begin() + (long)at, blocks.begin() + (long)std::min(blocks.size(), at + (size_t)per));
            items.push_back(std::move(it));
        }
    }
    const double utc0 = r.o("kaws.utc", 0) > 0 ? r.o("kaws.utc", 0) : systemUtc();
    return std::make_unique<KawsGen>(rateHz, std::move(items), utc0, r.o("kaws.align", 1) >= 0.5);
}

} // namespace dect2

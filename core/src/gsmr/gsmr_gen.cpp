// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GSM-R test signal (gsmr_gen.h).
#include "dect2/gsmr/gsmr_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/gsmr/gsmr_phy.h"
#include "dect2/source/source.h"
#include <cmath>
#include <random>

namespace dect2 {

int gsmrTestCells(int scene) { return scene == 1 ? 1 : (scene == 2 ? 3 : 2); }

GsmrCellCfg gsmrTestCell(int scene, int index) {
    struct S { int arfcn, ncc, bcc; uint32_t lac, cell; int phase; uint32_t fn0; float amp; };
    static const S s0[2] = {{960, 1, 2, 4711, 12001, 3100, 100, 0.30f}, {968, 1, 5, 4711, 12002, 7777, 1900, 0.22f}};
    static const S s1[1] = {{955, 3, 6, 9001, 777, 1234, 4000, 0.28f}};
    static const S s2[3] = {{957, 0, 1, 4711, 20001, 500, 51, 0.30f}, {964, 0, 4, 4712, 20002, 9100, 6000, 0.26f}, {971, 0, 7, 4713, 20003, 4400, 20000, 0.08f}};
    const S& s = scene == 1 ? s1[0] : (scene == 2 ? s2[std::max(0, std::min(2, index))] : s0[std::max(0, std::min(1, index))]);
    GsmrCellCfg c;
    c.arfcn = s.arfcn; c.ncc = s.ncc; c.bcc = s.bcc;
    c.si.cellId = s.cell;
    c.si.lai.mcc = 1; c.si.lai.mnc = 1; c.si.lai.mncDigits = 2; c.si.lai.lac = s.lac;
    c.si.t3212 = 10; c.si.rxLevAccessMin = 12; c.si.reselHystDb = 4;
    // the frequency lists in the 1024 range format, as a GSM-R network sends them (the channels are above ARFCN 954)
    c.cellAlloc = {s.arfcn - 2, s.arfcn, s.arfcn + 2};
    c.neigh.clear();
    for (int n : {955, 958, 961, 967, 970, 973}) if (n != s.arfcn) c.neigh.push_back(n);
    c.fn0 = s.fn0; c.phase = s.phase; c.amp = s.amp;
    return c;
}

namespace gsmr {

std::vector<cf32> btsFrames(const GsmrCellCfg& c, uint32_t fn0, int nFrames) {
    constexpr int kFrame = 10000, kSlot = 1250, kSps8 = 8;
    std::vector<cf32> buf((size_t)nFrames * kFrame + 128);
    const int base = 64;
    auto place = [&](const Bits& burst, int frame, int slot) {
        const std::vector<cf32> g = gmskBurst(burst, kSps8);
        const long at = base + (long)frame * kFrame + (long)slot * kSlot - 2 * kSps8;
        for (size_t i = 0; i < g.size(); i++) buf[(size_t)(at + (long)i)] += g[i];
    };
    const Bits dummy(kDummy, kDummy + 148);
    // the four bursts of a block, and the system information schedule
    Frame23 sched[8];
    Si3 si = c.si;
    sched[0] = encodeSi1(c.cellAlloc, si);
    sched[1] = encodeSi2(c.neigh, c.nccPermitted, si);
    sched[2] = encodeSi3(si);
    sched[3] = encodeSi4(si);
    sched[4] = encodeSi2ter();
    sched[5] = idleFrame();
    sched[6] = encodeSi3(si);
    sched[7] = encodeSi4(si);
    const int bsic = (c.ncc << 3) | c.bcc;
    Bits blk[4];
    for (int f = 0; f < nFrames; f++) {
        const uint32_t fn = (fn0 + (uint32_t)f) % (26 * 51 * 2048);
        const int mod = (int)(fn % 51), tc = (int)((fn / 51) % 8);
        for (int s = 1; s < 8; s++) place(dummy, f, s);
        if (mod % 10 == 0 && mod <= 40) place(fcchBurst(), f, 0);
        else if (mod % 10 == 1 && mod <= 41) place(syncBurst(schEncode(bsic, fn)), f, 0);
        else if (mod == 50) place(dummy, f, 0);
        else {
            static const int starts[10] = {2, 6, 12, 16, 22, 26, 32, 36, 42, 46};
            int b0 = -1;
            for (int st : starts) if (mod >= st && mod < st + 4) b0 = st;
            const bool bcch = b0 == 2;
            // the block is built when its first burst is reached; encode once per frame, cheap enough
            // CCCH: a paging request type 1 in the first block, an immediate assignment in every second multiframe's second block, the
            // rest idle (the bodies carry no identities)
            const uint32_t mf = fn / 51;
            const Frame23 fr = bcch ? sched[tc] : !c.ccchLoad ? idleFrame() : b0 == 6 ? encodePagingKind(0x21) : (b0 == 12 && (mf & 1) == 0) ? encodePagingKind(0x3F) : idleFrame();
            const Bits coded = xcchEncode(fr.data());
            xcchInterleave(coded, blk);
            place(normalBurst(blk[mod - b0], c.bcc, 1), f, 0);
        }
    }
    std::vector<cf32> out(buf.begin() + base, buf.begin() + base + (long)nFrames * kFrame);
    return out;
}

} // namespace gsmr

namespace {

class GsmrSynth : public ModeSynth {
public:
    GsmrSynth(const SynthConfig& c, double rate) : out_(rate), rng_(23), cfo_(c.cfoHz) {
        scene_ = c.modeOpt[0];
        const int n = gsmrTestCells(scene_);
        double pmax = 0;
        for (int i = 0; i < n; i++) {
            Bts b;
            b.c = gsmrTestCell(scene_, i);
            b.offHz = gsmrArfcnHz(b.c.arfcn) - kGsmrTestCentreHz;
            b.rs.reset(new cell::Resampler(std::min(0.5, 0.5 * out_ / gsmr::kNativeRate) * 0.95));
            b.fn = b.c.fn0;
            b.skip = b.c.phase;
            bts_.push_back(std::move(b));
            pmax = std::max(pmax, (double)bts_.back().c.amp * bts_.back().c.amp);
        }
        noise_ = (float)std::sqrt(pmax * (out_ / 200e3) / std::pow(10.0, c.snrDb / 10) / 2);
        if (c.snrDb >= 99) noise_ = 0;
    }
    double sampleRate() const override { return out_; }
    void generate(cf32* o, size_t n) override {
        for (size_t i = 0; i < n; i++) o[i] = 0;
        std::vector<cf32> tmp(n);
        const double step = gsmr::kNativeRate / out_;
        for (auto& b : bts_) {
            const int H = b.rs->halfLen();
            size_t done = 0;
            while (done < n) {
                if (b.pos + H + 2 >= (double)b.buf.size()) { more(b); continue; }
                const size_t fit = (size_t)std::floor(((double)b.buf.size() - H - 2 - b.pos) / step) + 1;
                const size_t m = std::min(n - done, fit);
                b.rs->run(b.buf.data(), b.buf.size(), b.pos, step, tmp.data() + done, m);
                b.pos += step * (double)m;
                done += m;
            }
            const double w = 2 * M_PI * b.offHz / out_;
            for (size_t i = 0; i < n; i++) {
                o[i] += tmp[i] * b.c.amp * cf32((float)std::cos(b.ph), (float)std::sin(b.ph));
                b.ph = std::fmod(b.ph + w, 2 * M_PI);
            }
        }
        dect2::normal_distribution<float> nd(0.f, 1.f);
        const double wc = 2 * M_PI * cfo_ / out_;
        for (size_t i = 0; i < n; i++) {
            if (cfo_ != 0) { o[i] *= cf32((float)std::cos(cph_), (float)std::sin(cph_)); cph_ = std::fmod(cph_ + wc, 2 * M_PI); }
            if (noise_ > 0) o[i] += cf32{noise_ * nd(rng_), noise_ * nd(rng_)};
        }
    }

private:
    struct Bts {
        GsmrCellCfg c;
        double offHz = 0, pos = 0, ph = 0;
        int skip = 0;
        uint32_t fn = 0;
        std::unique_ptr<cell::Resampler> rs;
        std::vector<cf32> buf;
    };
    static double gsmrArfcnHz(int n) { return (935.0 + 0.2 * (n - 1024)) * 1e6; }
    void more(Bts& b) {
        const long keep = std::max(0L, (long)std::floor(b.pos) - b.rs->halfLen() - 2);
        if (keep > 0) { b.buf.erase(b.buf.begin(), b.buf.begin() + keep); b.pos -= (double)keep; }
        constexpr int kChunk = 51;   // one multiframe at a time
        std::vector<cf32> f = gsmr::btsFrames(b.c, b.fn, kChunk);
        b.fn = (b.fn + kChunk) % (26 * 51 * 2048);
        size_t from = 0;
        if (b.skip > 0) { from = (size_t)b.skip; b.skip = 0; }
        b.buf.insert(b.buf.end(), f.begin() + (long)from, f.end());
    }
    double out_, cfo_, cph_ = 0;
    float noise_ = 0;
    int scene_ = 0;
    std::vector<Bts> bts_;
    std::mt19937 rng_;
};

} // namespace

std::unique_ptr<ModeSynth> makeGsmrSynth(const SynthConfig& cfg, double sampleRate) { return std::unique_ptr<ModeSynth>(new GsmrSynth(cfg, sampleRate)); }

} // namespace dect2

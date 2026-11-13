// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers test signal (cell_gen.h).
#include "dect2/cell/cell_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/cell/cell_asn.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/source/source.h"
#include <cmath>
#include <random>

namespace dect2 {

cell::LteGenCfg cellTestCell(int scene, CellTestTruth* truth, int pciOverride) {
    using namespace cell;
    LteGenCfg g;
    struct S { int nRb, ports, pci; uint32_t enb; int sector; int band; int earfcn; };
    static const S sc[4] = {{50, 2, 123, 54321, 2, 20, 6300}, {25, 4, 301, 7001, 1, 3, 1300}, {6, 1, 7, 99, 0, 8, 3500}, {100, 2, 450, 900001 & 0xFFFFF, 3, 1, 300}};
    const S& s = sc[std::max(0, std::min(3, scene))];
    g.cell.nRb = s.nRb; g.cell.ports = s.ports; g.cell.pci = pciOverride >= 0 ? pciOverride : s.pci;
    g.cell.phichRes = 1;
    g.cfi = s.nRb <= 10 ? 2 : 2;
    g.seed = 7 + (uint32_t)g.cell.pci;
    LteSib1 s1;
    Plmn p; p.mcc = 1; p.mnc = 1; p.mncDigits = 2;
    s1.plmns.push_back(p);
    s1.tac = 0x1A2B + (uint32_t)scene;
    s1.cellId = (s.enb << 8) | (uint32_t)s.sector;
    s1.qRxLevMinDbm = -124; s1.pMax = 23; s1.band = s.band;
    s1.sched.push_back({16, {}});      // SI-1: SIB2 (always the first SI message, not listed)
    s1.sched.push_back({32, {3}});     // SI-2: SIB3
    s1.sched.push_back({64, {4, 5}});  // SI-3: SIB4 and SIB5
    s1.siWindowMs = 20; s1.valueTag = 5;
    LteSib2 s2;
    s2.numRaPreambles = 52; s2.preambleTransMax = 10; s2.raRespWindow = 10; s2.modPeriodCoeff = 4; s2.pagingCycleRf = 128;
    s2.rootSeqIndex = 22; s2.prachConfigIndex = 3; s2.prachFreqOffset = 4; s2.refSignalPowerDbm = 15; s2.pb = 1; s2.n1PucchAn = 36;
    s2.p0NominalPusch = -96; s2.alpha = 7; s2.p0NominalPucch = -110;
    s2.t300 = 1000; s2.t301 = 1000; s2.t310 = 1000; s2.n310 = 10; s2.t311 = 10000; s2.n311 = 1;
    s2.ulEarfcn = s.earfcn + 18000; s2.ulBwRb = s.nRb; s2.timeAlignMs = -1;
    LteSib3 s3; s3.qHystDb = 4; s3.threshServingLow = 6; s3.priority = 5; s3.qRxLevMinDbm = -124; s3.sIntraSearch = 62; s3.sNonIntraSearch = 8; s3.tReselSec = 1;
    LteSib4 s4; s4.neigh = {{g.cell.pci + 1, 0}, {g.cell.pci + 2, 2}, {17, -2}};
    LteSib5 s5;
    { LteSib5Freq f; f.earfcn = 1300; f.qRxLevMinDbm = -124; f.priority = 6; f.threshHigh = 10; f.threshLow = 4; f.bwRb = 75; f.neigh = {{88, 0}}; s5.freqs.push_back(f); }
    { LteSib5Freq f; f.earfcn = 3500; f.qRxLevMinDbm = -120; f.priority = 3; f.threshHigh = 12; f.threshLow = 2; f.bwRb = 50; s5.freqs.push_back(f); }
    LteSib a; a.bits = lteEncodeSib1(s1); a.period = 2; a.sfnMod = 0; a.subframe = 5;
    LteSib b; b.bits = lteEncodeSi(&s2, nullptr, nullptr, nullptr); b.period = 16; b.sfnMod = 0; b.subframe = 3;
    LteSib c; c.bits = lteEncodeSi(nullptr, &s3, nullptr, nullptr); c.period = 32; c.sfnMod = 2; c.subframe = 4;
    LteSib d; d.bits = lteEncodeSi(nullptr, nullptr, &s4, &s5); d.period = 64; d.sfnMod = 4; d.subframe = 1; d.c1 = true;
    g.sibs = {a, b, c, d};
    if (truth) {
        truth->pci = g.cell.pci; truth->nRb = s.nRb; truth->ports = s.ports; truth->mcc = 1; truth->mnc = 1; truth->band = s.band;
        truth->tac = s1.tac; truth->cellId = s1.cellId;
        truth->intraPci.clear(); for (const auto& n : s4.neigh) truth->intraPci.push_back(n.pci);
        truth->interEarfcn.clear(); for (const auto& f : s5.freqs) truth->interEarfcn.push_back(f.earfcn);
    }
    return g;
}

namespace {

class CellSynth : public ModeSynth {
public:
    CellSynth(const SynthConfig& c, double rate) : out_(rate), rng_(11) {
        gc_ = cellTestCell(c.modeOpt[0]);
        native_ = cell::lteRate(cell::lteNfft(gc_.cell.nRb));
        step_ = native_ / out_;
        rs_.reset(new cell::Resampler(std::min(0.5, 0.5 * out_ / native_) * 0.95));
        cfo_ = c.cfoHz;
        const double occ = gc_.cell.nRb * 180e3;
        // the generator's rms is 0.2; noise over the whole output band for snrDb inside the carrier
        const double sig = 0.04 * std::min(1.0, out_ / native_);
        noise_ = (float)std::sqrt(sig * (out_ / occ) / std::pow(10.0, c.snrDb / 10) / 2);
        if (c.snrDb >= 99) noise_ = 0;
    }
    double sampleRate() const override { return out_; }
    void generate(cf32* o, size_t n) override {
        size_t done = 0;
        dect2::normal_distribution<float> nd(0.f, 1.f);
        const int H = rs_->halfLen();
        while (done < n) {
            if (pos_ + H + 2 >= (double)buf_.size()) { more(); continue; }
            const size_t fit = (size_t)std::floor(((double)buf_.size() - H - 2 - pos_) / step_) + 1;
            const size_t m = std::min(n - done, fit);
            rs_->run(buf_.data(), buf_.size(), pos_, step_, o + done, m);
            pos_ += step_ * (double)m;
            done += m;
        }
        const double w = 2 * M_PI * cfo_ / out_;
        for (size_t i = 0; i < n; i++) {
            o[i] *= cf32((float)std::cos(ph_), (float)std::sin(ph_));
            ph_ = std::fmod(ph_ + w, 2 * M_PI);
            if (noise_ > 0) o[i] += cf32{noise_ * nd(rng_), noise_ * nd(rng_)};
        }
    }

private:
    void more() {
        // drop what is no longer needed, then add the next radio frame
        const long keep = std::max(0L, (long)std::floor(pos_) - rs_->halfLen() - 2);
        if (keep > 0) { buf_.erase(buf_.begin(), buf_.begin() + keep); pos_ -= (double)keep; }
        cell::LteGenCfg g = gc_;
        g.sfn0 = sfn_;
        sfn_ = (sfn_ + 1) & 1023;
        const std::vector<cf32> f = cell::lteGenerate(g, 1);
        buf_.insert(buf_.end(), f.begin(), f.end());
    }
    double out_, native_ = 1, step_ = 1, pos_ = 0, cfo_ = 0, ph_ = 0;
    float noise_ = 0;
    int sfn_ = 0;
    cell::LteGenCfg gc_;
    std::unique_ptr<cell::Resampler> rs_;
    std::vector<cf32> buf_;
    std::mt19937 rng_;
};

} // namespace

std::unique_ptr<ModeSynth> makeCellSynth(const SynthConfig& cfg, double sampleRate) { return std::unique_ptr<ModeSynth>(new CellSynth(cfg, sampleRate)); }

} // namespace dect2

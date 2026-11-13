// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NB-IoT test signal (nbiot_gen.h, nbiot_phy.h).
#include "dect2/nbiot/nbiot_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/source/source.h"
#include "../cell/cell_lte_int.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2::nbiot {

struct Generator::Impl {
    GenCfg c;
    int nf = 0;
    std::mt19937 rng;
    // the NPBCH codeword of the 640 ms period being sent
    int ttiIndex = -1;
    Bits npbch;
    // SIB1-NB
    Bits sib1e;          // the rate-matched codeword
    int nbBits = 0;      // bits per subframe
    bool inband = false;
    int crsPorts = 1, crsShift = 0;
    std::vector<std::pair<int, int>> pbRes, pdRes;
    float a = 0;

    explicit Impl(const GenCfg& cfg) : c(cfg), nf(cfg.sfn0 & 1023), rng(cfg.seed) {
        a = (float)(c.amp / std::sqrt(12.0));
        inband = c.mib.mode == kInbandSame || c.mib.mode == kInbandDiff;
        crsPorts = c.mib.mode == kInbandSame ? c.ports : (c.mib.numCrs ? 4 : c.ports);
        crsShift = c.mib.mode == kInbandSame ? c.pci % 6 : (c.crsShift >= 0 ? c.crsShift : c.pci % 6);
        pbRes = npbchRes(c.pci);
        pdRes = npdschRes(c.pci, c.ports, inband, crsPorts, crsShift);
        nbBits = (int)pdRes.size() * 2;
        const int tbs = sib1Tbs(c.mib.sched);
        Bits tb = sib1Encode(c.sib1, (size_t)tbs);
        tb.resize((size_t)tbs);
        cell::appendCrc(tb, cell::kCrc24A, 24);
        sib1e = cell::convRateMatch(cell::convEncode(tb), (size_t)nbBits * 8);
    }

    cf32 qpsk() {
        const uint32_t r = rng();
        return cell::qpsk((uint8_t)(r & 1), (uint8_t)((r >> 1) & 1));
    }

    // add symbols (in mapping order) to the REs on the ports, with transmit diversity
    void put(std::vector<Grid>& g, const std::vector<std::pair<int, int>>& res, const std::vector<cf32>& d) {
        std::vector<std::vector<cf32>> y;
        cell::lteTxdEncode(d, c.ports, y);
        for (int p = 0; p < c.ports; p++)
            for (size_t j = 0; j < res.size() && j < y[(size_t)p].size(); j++) g[(size_t)p].at(res[j].first, res[j].second) += a * y[(size_t)p][j];
    }

    void nrs(std::vector<Grid>& g, int sf) {
        for (int p = 0; p < c.ports; p++)
            for (int l : {5, 6, 12, 13}) {
                const int base = nrsBase(p, l, c.pci);
                for (int m = 0; m < 2; m++) g[(size_t)p].at(l, base + 6 * m) = a * nrsValue(c.pci, sf, l, m);
            }
    }

    void crs(std::vector<Grid>& g) {
        if (!inband) return;
        for (int p = 0; p < crsPorts; p++) for (int l = 0; l < kSyms; l++) {
            const int base = crsBase(p, l, crsShift);
            if (base < 0) continue;
            for (int k = base; k < kSc; k += 6) g[(size_t)std::min(p, c.ports - 1)].at(l, k) = a * qpsk();
        }
    }

    // is this frame one that carries a SIB1-NB subframe? returns the subframe index 0 .. 7 and the first frame of its repetition, or -1
    int sib1Index(int frame, int& firstFrame) const {
        const int reps = sib1Reps(c.mib.sched);
        if (reps == 0) return -1;
        const int pos = frame % 256, step = 256 / reps, s0 = sib1StartFrame(reps, c.pci);
        for (int r = 0; r < reps; r++) {
            const int fb = s0 + step * r;
            const int d = pos - fb;
            if (d >= 0 && d <= 14 && d % 2 == 0) { firstFrame = frame - d; return d / 2; }
        }
        return -1;
    }

    void frame(cf32* out) {
        const int f = nf & 1023;
        Mib mib = c.mib;
        mib.sfnMsb = (f / 64) & 15;
        if (ttiIndex != f / 64) { ttiIndex = f / 64; npbch = npbchCode(mib, c.ports, c.pci); }
        std::vector<Grid> g((size_t)c.ports);
        for (int sf = 0; sf < 10; sf++) {
            for (auto& x : g) x.clear();
            bool nrsOn = false;
            bool dataSf = false;
            if (sf == 0) {
                const int seg = (f % 64) / 8;
                std::vector<cf32> d(100);
                for (int i = 0; i < 100; i++) d[(size_t)i] = cell::qpsk(npbch[(size_t)(200 * seg + 2 * i)], npbch[(size_t)(200 * seg + 2 * i + 1)]);
                put(g, pbRes, d);
                nrsOn = true;
                crs(g);
            } else if (sf == 4) {
                int first = 0;
                const int idx = sib1Index(f, first);
                if (idx >= 0) {
                    const uint32_t cinit = (0xFFFFu << 14) + (uint32_t)((first & 1) << 13) + (4u << 9) + (uint32_t)c.pci;
                    const Bits sc = cell::gold(cinit, (size_t)nbBits * 8);
                    std::vector<cf32> d((size_t)nbBits / 2);
                    for (size_t i = 0; i < d.size(); i++) {
                        const size_t b = (size_t)idx * (size_t)nbBits + 2 * i;
                        d[i] = cell::qpsk((uint8_t)(sib1e[b] ^ sc[b]), (uint8_t)(sib1e[b + 1] ^ sc[b + 1]));
                    }
                    put(g, pdRes, d);
                    nrsOn = true;
                    crs(g);
                } else dataSf = c.loadOther;
            } else if (sf == 5) {
                cf32 d[11];
                npssSeq(d);
                for (int l = 3; l < kSyms; l++) for (int k = 0; k < 11; k++) g[0].at(l, k) = a * (float)npssCover(l) * d[k];
            } else if (sf == 9 && f % 2 == 0) {
                cf32 d[132];
                nsssSeq(c.pci, (f / 2) % 4, d);
                for (int n = 0; n < 132; n++) g[0].at(3 + n / 12, n % 12) = a * d[n];
            } else dataSf = c.loadOther;
            if (dataSf) {
                nrsOn = true;
                std::vector<cf32> d(pdRes.size());
                for (auto& x : d) x = qpsk();
                put(g, pdRes, d);
                crs(g);
            }
            if (nrsOn) nrs(g, sf);
            // the channel of each port: the ports are summed here (a flat channel)
            Grid tot;
            for (int p = 0; p < c.ports; p++) {
                const cf32 gain = p == 0 ? cf32(1, 0) : std::polar(c.port1Gain, c.port1Phase);
                for (int i = 0; i < kSyms * kSc; i++) tot.v[i] += gain * g[(size_t)p].v[i];
            }
            modulateSubframe(tot, out + sf * kSubframe);
        }
        nf = (nf + 1) & 1023;
    }
};

Generator::Generator(const GenCfg& c) : p_(new Impl(c)) {}
Generator::~Generator() = default;
void Generator::nextFrame(cf32* out) { p_->frame(out); }
int Generator::frame() const { return p_->nf; }
double Generator::carrierPower() const { return p_->c.amp * p_->c.amp; }

} // namespace dect2::nbiot

namespace dect2 {

nbiot::GenCfg nbiotTestCell(int scene, NbTestTruth* truth, int pciOverride) {
    using namespace nbiot;
    struct S { int pci, ports, mode, sched, band; uint32_t tac, cellId; int raster; };
    static const S sc[4] = {
        {100, 1, kStandalone, 1, 8, 0x1A2B, 0x0123456, 2},
        {250, 2, kGuardband, 2, 20, 0x2B3C, 0x0234567, 0},
        {17, 2, kInbandSame, 0, 3, 0x3C4D, 0x0345678, 1},
        {400, 1, kInbandDiff, 4, 20, 0x4D5E, 0x0456789, 3},
    };
    const S& s = sc[std::max(0, std::min(3, scene))];
    GenCfg g;
    g.pci = pciOverride >= 0 ? pciOverride : s.pci;
    g.ports = s.ports;
    g.mib.mode = s.mode;
    g.mib.sched = s.sched;
    g.mib.valueTag = 5;
    g.mib.hsfnLsb = 1;
    g.mib.raster = s.raster;
    g.mib.crsSeq = 9;
    g.mib.numCrs = scene == 3 ? 1 : 0;
    g.crsPorts = s.mode == kInbandDiff ? 4 : s.ports;
    g.crsShift = s.mode == kInbandDiff ? (g.pci + 2) % 6 : -1;
    cell::Plmn p; p.mcc = 1; p.mnc = 1; p.mncDigits = 2;
    g.sib1.plmns.push_back(p);
    g.sib1.tac = s.tac;
    g.sib1.cellId = s.cellId;
    g.sib1.band = s.band;
    g.sib1.qRxLevMinDbm = -128;
    g.sib1.qQualMinDb = -20;
    g.sib1.pMax = 23;
    g.sib1.hsfnMsb = 3;
    g.seed = 5 + (uint32_t)g.pci;
    g.sfn0 = 37 * (scene + 1);
    if (truth) {
        truth->pci = g.pci; truth->ports = s.ports; truth->mode = s.mode; truth->mcc = 1; truth->mnc = 1; truth->band = s.band; truth->sched = s.sched;
        truth->tac = s.tac; truth->cellId = s.cellId;
        // the offset of the carrier from the 100 kHz raster: the MIB says it for the in-band (other PCI) and guard-band cells
        static const double off[4] = {2.5, -7.5, -2.5, 7.5};
        truth->rasterKhz = off[scene & 3];
    }
    return g;
}

namespace {

class NbiotSynth : public ModeSynth {
public:
    NbiotSynth(const SynthConfig& c, double rate) : out_(rate), rng_(23) {
        NbTestTruth t;
        gc_ = nbiotTestCell(c.modeOpt[0], &t);
        gen_.reset(new nbiot::Generator(gc_));
        step_ = (double)nbiot::kFs / out_ * (1.0 + c.sroPpm * 1e-6);
        rs_.reset(new cell::Resampler(0.5, 14));
        cfo_ = c.cfoHz + t.rasterKhz * 1e3;
        // a resource element has power amp^2 / 12; the noise of one resource element after the receiver's FFT is the noise density times 15 kHz,
        // so a complex noise variance of amp^2 * rate / (180 kHz * snr) per sample gives snrDb per resource element
        const double sig = gc_.amp * gc_.amp;
        noise_ = (float)std::sqrt(sig * out_ / 180e3 / std::pow(10.0, c.snrDb / 10) / 2);
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
        const long keep = std::max(0L, (long)std::floor(pos_) - rs_->halfLen() - 2);
        if (keep > 0) { buf_.erase(buf_.begin(), buf_.begin() + keep); pos_ -= (double)keep; }
        std::vector<cf32> f((size_t)nbiot::kFrame);
        gen_->nextFrame(f.data());
        buf_.insert(buf_.end(), f.begin(), f.end());
    }
    double out_, step_ = 1, pos_ = 0, cfo_ = 0, ph_ = 0;
    float noise_ = 0;
    nbiot::GenCfg gc_;
    std::unique_ptr<nbiot::Generator> gen_;
    std::unique_ptr<cell::Resampler> rs_;
    std::vector<cf32> buf_;
    std::mt19937 rng_;
};

} // namespace

std::unique_ptr<ModeSynth> makeNbiotSynth(const SynthConfig& cfg, double sampleRate) { return std::unique_ptr<ModeSynth>(new NbiotSynth(cfg, sampleRate)); }

} // namespace dect2

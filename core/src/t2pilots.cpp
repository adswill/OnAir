#include "dect2/t2pilots.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dect2 {

namespace {
// PP supported per FFT size (index = S2 field 1 code: 2K,8K,4K,1K,16K,32K), PP1..PP8
const bool kSupported[6][8] = {
    {1, 1, 1, 1, 1, 0, 1, 0}, // 2K
    {1, 1, 1, 1, 1, 0, 1, 1}, // 8K
    {1, 1, 1, 1, 1, 0, 1, 0}, // 4K
    {1, 1, 1, 1, 1, 0, 0, 0}, // 1K
    {1, 1, 1, 1, 1, 1, 1, 1}, // 16K
    {0, 1, 0, 1, 0, 1, 1, 1}, // 32K
};
// Frame-closing symbol present (before the guard-interval exceptions below)
const bool kHasFc[6][8] = {
    {1, 1, 1, 1, 1, 0, 1, 0},
    {1, 1, 1, 1, 1, 0, 1, 0},
    {1, 1, 1, 1, 1, 0, 1, 0},
    {1, 1, 1, 1, 1, 0, 0, 0},
    {1, 1, 1, 1, 1, 1, 1, 0},
    {0, 1, 0, 1, 0, 1, 0, 0},
};
} // namespace

PilotMap::PilotMap(const PilotConfig& c) : cfg_(c) {
    const FftMode* m = fftModeFromS2(c.fftCode);
    if (!m || c.fftCode > 5 || c.pp < 0 || c.pp > 7) return;
    if (c.ext && !m->kExt) return;
    if (!kSupported[c.fftCode][c.pp]) return;
    for (int i = 0; i < kNumPilotEntries; i++) {
        const PilotEntry& e = kPilotEntries[i];
        if (e.fft == c.fftCode && e.ext == (c.ext ? 1 : 0) && e.pp == c.pp) { e_ = &e; break; }
    }
    if (!e_) return;
    n_ = m->n;
    nP2_ = m->nP2;
    cps_ = c.ext ? m->kExt : m->kNormal;
    kExt_ = c.ext ? (n_ == 8192 ? 48 : n_ == 16384 ? 144 : 288) : 0;
    kOff_ = !c.ext ? (n_ == 8192 ? 48 : n_ == 16384 ? 144 : n_ == 32768 ? 288 : 0) : 0;
    dx_ = e_->dx; dy_ = e_->dy; sp_ = e_->sp; cp_ = e_->cp; p2_ = e_->p2;
    contMask_.assign(cps_, 0);
    for (int i = 0; i < e_->nCont; i++) contMask_[e_->cont[i]] = 1;
    // PRBS x^11 + x^9 + 1
    prbs_.resize(27841 + 288);
    int sr = 0x7ff;
    for (size_t i = 0; i < prbs_.size(); i++) {
        int b = (sr ^ (sr >> 2)) & 1;
        prbs_[i] = sr & 1;
        sr >>= 1;
        if (b) sr |= 0x400;
    }
    // P2 symbol: every 3rd carrier is a pilot (every 6th for 32K SISO), edge carriers in extended mode, and reserved carriers
    p2Types_.assign(cps_, kCellData);
    int step = (c.fftCode == 5) ? 6 : 3;
    for (int i = 0; i < cps_; i += step) p2Types_[i] = kCellP2Pilot;
    if (c.ext)
        for (int i = 0; i < kExt_; i++) { p2Types_[i] = kCellP2Pilot; p2Types_[i + (cps_ - kExt_)] = kCellP2Pilot; }
    {
        const TrTable& t = kP2PaprTable[c.fftCode];
        for (int i = 0; i < t.n; i++) p2Types_[t.v[i] + kExt_] = kCellP2Papr;
    }
    cP2_ = 0;
    for (auto v : p2Types_) cP2_ += v == kCellData;
    // frame-closing symbol
    hasFc_ = kHasFc[c.fftCode][c.pp];
    if (!c.ext || true) {
        int gi = c.giIdx;
        if ((gi == 4 && c.pp == 6) || (gi == 0 && c.pp == 3) || ((gi == 1 || gi == 6) && c.pp == 1)) hasFc_ = false;
    }
    fcTypes_.assign(cps_, kCellData);
    for (int i = 0; i < cps_; i++) if (i % dx_ == 0) fcTypes_[i] = kCellScattered;
    if (c.fftCode == 3 && (c.pp == 3 || c.pp == 4)) fcTypes_[cps_ - 2] = kCellScattered;
    else if (c.fftCode == 0 && c.pp == 6) fcTypes_[cps_ - 2] = kCellScattered;
    fcTypes_[0] = kCellScattered;
    fcTypes_[cps_ - 1] = kCellScattered;
    if (c.tr) {
        const TrTable& t = kP2PaprTable[c.fftCode];
        for (int i = 0; i < t.n; i++) fcTypes_[t.v[i] + kExt_] = kCellTrPapr;
    }
    nFc_ = 0;
    for (auto v : fcTypes_) nFc_ += v == kCellData;
    std::vector<uint8_t> t;
    symbolTypes(nP2_, nP2_ + 2, t); // any ordinary data symbol
    cData_ = 0;
    for (auto v : t) cData_ += v == kCellData;
    valid_ = true;
}

void PilotMap::symbolTypes(int l, int numSyms, std::vector<uint8_t>& t) const {
    if (l < nP2_) { t = p2Types_; return; }
    if (isFc(l, numSyms)) { t = fcTypes_; return; }
    t.assign(cps_, kCellData);
    for (int i = 0; i < cps_; i++) if (contMask_[i]) t[i] = kCellContinual;
    const int dxy = dx_ * dy_;
    for (int i = 0; i < cps_; i++) {
        int r = (i - kExt_) % dxy;
        if (r < 0) r += dxy;
        if (r == dx_ * (l % dy_)) t[i] = kCellScattered;
    }
    t[0] = kCellScattered;
    t[cps_ - 1] = kCellScattered;
    if (cfg_.tr) {
        int shift = !cfg_.ext ? dx_ * (l % dy_) : dx_ * ((l + kExt_ / dx_) % dy_);
        const TrTable& tr = kTrPaprTable[cfg_.fftCode];
        for (int i = 0; i < tr.n; i++) t[tr.v[i] + shift] = kCellTrPapr;
    }
}

cf32 PilotMap::pilot(int l, int k, uint8_t type) const {
    int pn = (kPilotPn[l >> 3] >> (7 - (l & 7))) & 1;
    int bit = prbs_[k + kOff_] ^ pn;
    float amp = type == kCellP2Pilot ? p2_ : type == kCellContinual ? cp_ : sp_;
    return cf32(bit ? -amp : amp, 0.f);
}

} // namespace dect2

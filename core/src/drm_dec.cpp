// DRM back end: FAC, SDC and MSC decoding from equalised cells.
#include "dect2/drm_dec.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 { namespace drm {

DrmBackend::DrmBackend() {}

void DrmBackend::dropData() {
    sfValid_ = false; mscFilled_ = 0; framesInSf_ = 0;
    hist_.clear();
    gap_ = true;
}

void DrmBackend::reset() {
    mode_ = -1; occ_ = -1; layout_.reset();
    facValid_ = false; sdcSeen_ = false; hierarchical_ = false;
    fac_ = FacInfo(); sdc_ = SdcInfo();
    for (int i = 0; i < 4; i++) facSeen_[i] = false;
    pos_ = -1;
    facCode_.reset(); sdcCode_.reset(); mscCode_.reset();
    sdcCodeMode_ = -1; mscKey_ = -1;
    dropData();
    facConst.clear(); sdcConst.clear(); mscConst.clear();
}

void DrmBackend::setMode(int mode) {
    if (mode == mode_) return;
    reset();
    mode_ = mode;
}

std::shared_ptr<const Layout> DrmBackend::layout() const { return layout_; }

int DrmBackend::mscQamBits() const { return mscQam_; }

void DrmBackend::frameLost() {
    dropData();
    pos_ = -1;
}

void DrmBackend::decodeFac(const CellGrid& g) {
    if (mode_ == kModeE) return;
    const ModeParams& mp = modeParams(mode_);
    const int nFac = 65;
    // the FAC cells of a mode do not depend on the occupancy: use the cell table of any occupancy
    auto L = layout_ ? layout_ : drm::layout(mode_, mode_ == kModeA ? 2 : mode_ == kModeB ? 2 : 3);
    if (!L) return;
    if (!facCode_) {
        MlcParams f;
        f.levels = 1; f.n2 = nFac; f.fac = true; f.rxB[0] = 3; f.ryB[0] = 5;
        facCode_ = std::make_unique<MlcCode>(f);
    }
    std::vector<cf32> z((size_t)nFac);
    std::vector<float> w((size_t)nFac);
    for (int i = 0; i < nFac; i++) {
        const auto& c = L->facCells[(size_t)i];
        if (c.second < g.kmin || c.second > g.kmax || c.first >= g.ns) { facBad++; return; }
        z[(size_t)i] = g.zAt(c.first, c.second);
        w[(size_t)i] = g.wAt(c.first, c.second);
    }
    std::vector<uint8_t> u((size_t)facCode_->infoBits()), pr(u.size());
    facCode_->decode(z.data(), w.data(), u.data(), 1);
    prbs(pr.data(), pr.size());
    for (size_t i = 0; i < u.size(); i++) u[i] ^= pr[i];
    FacInfo f;
    facConst = z;
    if (!facParse(u.data(), false, f) || !occupancyValid(mode_, f.occupancy)) { facBad++; return; }
    (void)mp;
    facOk++;
    const bool first = !facValid_;
    const bool changed = facValid_ && (f.occupancy != fac_.occupancy || f.interleaver != fac_.interleaver || f.mscMode != fac_.mscMode || f.sdcMode != fac_.sdcMode ||
                                       f.numServicesCode != fac_.numServicesCode);
    fac_ = f;
    facValid_ = true;
    FacService& s = facSvc_[f.svc[0].shortId & 3];
    s = f.svc[0];
    facSeen_[f.svc[0].shortId & 3] = true;
    if (first && log) {
        char b[200];
        snprintf(b, sizeof b, "DRM: FAC read: mode %c, %s, %s interleaving, MSC %s, SDC %s", modeParams(mode_).name, occupancyName(false, f.occupancy), f.interleaver ? "short" : "long",
                 mscModeName(false, f.mscMode), f.sdcMode ? "4-QAM" : "16-QAM");
        log(b);
    }
    if (changed && log) log("DRM: the channel parameters changed (reconfiguration)");
    if (first || changed) {
        if (occ_ != f.occupancy) { occ_ = f.occupancy; layout_ = drm::layout(mode_, occ_); }
        sdcSeen_ = changed ? false : sdcSeen_;
        dropData();
        if (changed) { mscCode_.reset(); mscKey_ = -1; }
    }
    if (onFac) onFac(f);
}

void DrmBackend::buildMscCode() {
    if (!layout_ || !facValid_ || !sdc_.mux.present) { mscCode_.reset(); return; }
    const SdcMux& mx = sdc_.mux;
    int bits = 0;
    if (fac_.mscMode == 0) bits = 6; else if (fac_.mscMode == 3) bits = 4;
    hierarchical_ = bits == 0;
    if (!bits) { mscCode_.reset(); mscKey_ = -1; return; }
    int X = 0, Y = 0;
    for (int i = 0; i < mx.nStreams; i++) { X += mx.stream[i].lenA; Y += mx.stream[i].lenB; }
    int key = ((((mode_ * 8 + occ_) * 4 + mx.protA) * 4 + mx.protB) * 2 + fac_.interleaver) * 64 + bits * 4 + mx.nStreams;
    for (int i = 0; i < mx.nStreams; i++) key = key * 31 + mx.stream[i].lenA * 7 + mx.stream[i].lenB;
    if (key == mscKey_ && mscCode_) return;
    mscKey_ = key;
    mscCode_.reset();
    hist_.clear();
    nStreams_ = mx.nStreams;
    for (int i = 0; i < 4; i++) { streamA_[i] = mx.stream[i].lenA; streamB_[i] = mx.stream[i].lenB; }
    if (mx.protB >= mscProtLevels(mode_, bits) || (X > 0 && mx.protA >= mscProtLevels(mode_, bits))) return;
    MlcParams p;
    p.levels = bits / 2;
    for (int l = 0; l < p.levels; l++) {
        const Rate b = mscRate(mode_, bits, mx.protB, l);
        p.rxB[l] = b.rx; p.ryB[l] = b.ry;
        const Rate a = mscRate(mode_, bits, X > 0 ? mx.protA : mx.protB, l);
        p.rxA[l] = a.rx; p.ryA[l] = a.ry;
    }
    if (X > 0) {
        const int lcm = mscRyLcm(mode_, bits, mx.protA);
        int den = 0;
        for (int l = 0; l < p.levels; l++) den += 2 * p.rxA[l] * (lcm / p.ryA[l]);
        p.n1 = lcm * ((8 * X + den - 1) / den);
    }
    p.n2 = layout_->nMux - p.n1;
    if (p.n2 < 20) return;
    auto code = std::make_unique<MlcCode>(p);
    if (code->infoBits() < 8 * (X + Y)) { if (log) log("DRM: the multiplex description does not fit the multiplex frame"); return; }
    mscCode_ = std::move(code);
    mscQam_ = 1 << bits;
    depth_ = fac_.interleaver == 0 ? modeParams(mode_).depth : 1;
    if (log) {
        char b[200];
        snprintf(b, sizeof b, "DRM: multiplex: %d stream(s), %d bytes in part A and %d in part B, %d-QAM, protection %d/%d", mx.nStreams, X, Y, 1 << bits, mx.protA, mx.protB);
        log(b);
    }
}

void DrmBackend::decodeSdc() {
    if (!layout_ || !facValid_) return;
    const int mode = fac_.sdcMode;
    if (!sdcCode_ || sdcCodeMode_ != mode) {
        MlcParams s;
        s.n2 = layout_->nSdc;
        if (mode == 0) { s.levels = 2; s.rxB[0] = 1; s.ryB[0] = 3; s.rxB[1] = 2; s.ryB[1] = 3; }
        else { s.levels = 1; s.rxB[0] = 1; s.ryB[0] = 2; }
        sdcCode_ = std::make_unique<MlcCode>(s);
        sdcCodeMode_ = mode;
    }
    std::vector<uint8_t> u((size_t)sdcCode_->infoBits()), pr(u.size());
    sdcCode_->decode(sdcZ_.data(), sdcW_.data(), u.data(), 2);
    prbs(pr.data(), pr.size());
    for (size_t i = 0; i < u.size(); i++) u[i] ^= pr[i];
    sdcConst.assign(sdcZ_.begin(), sdcZ_.begin() + (ptrdiff_t)std::min<size_t>(sdcZ_.size(), 1024));
    const int F = sdcDataBytes(mode_, occ_, mode);
    int afs = 0;
    if (F < 4 || !sdcCheckBits(u.data(), F, afs)) { sdcBad++; return; }
    sdcOk++;
    std::vector<uint8_t> field((size_t)F, 0);
    for (int i = 0; i < F * 8; i++) field[(size_t)i / 8] |= (uint8_t)(u[(size_t)(4 + i)] << (7 - i % 8));
    const bool first = !sdcSeen_;
    sdcSeen_ = true;
    sdc_.afsIndex = afs;
    sdcParse(field.data(), F, sdc_);
    if (first && log) {
        char b[200];
        snprintf(b, sizeof b, "DRM: SDC read: %d data entities", sdc_.entities);
        log(b);
    }
    buildMscCode();
}

void DrmBackend::decodeMux() {
    if (!mscCode_ || (int)hist_.size() < depth_) return;
    const int M = layout_->nMux;
    const std::vector<int>& pi = interleavePerm(M, 5);
    std::vector<cf32> z((size_t)M);
    std::vector<float> w((size_t)M);
    for (int i = 0; i < M; i++) {
        const MuxFrame& f = hist_[(size_t)(depth_ > 1 ? i % depth_ : 0)];
        z[(size_t)pi[(size_t)i]] = f.z[(size_t)i];
        w[(size_t)pi[(size_t)i]] = f.w[(size_t)i];
    }
    const int L = mscCode_->infoBits();
    std::vector<uint8_t> u((size_t)L), pr((size_t)L);
    mscCode_->decode(z.data(), w.data(), u.data(), 4);
    prbs(pr.data(), pr.size());
    for (int i = 0; i < L; i++) u[(size_t)i] ^= pr[(size_t)i];
    mscConst.assign(z.begin(), z.begin() + (ptrdiff_t)std::min<size_t>(z.size(), 1024));
    // bytes of the multiplex frame: part A of every stream, then part B of every stream
    std::vector<uint8_t> by((size_t)L / 8, 0);
    for (int i = 0; i < (L / 8) * 8; i++) by[(size_t)i / 8] |= (uint8_t)(u[(size_t)i] << (7 - i % 8));
    int offA = 0, offB = 0;
    for (int i = 0; i < nStreams_; i++) offB += streamA_[i];
    mscOk++;
    for (int i = 0; i < nStreams_; i++) {
        lf_.assign(by.begin() + offA, by.begin() + offA + streamA_[i]);
        lf_.insert(lf_.end(), by.begin() + offB, by.begin() + offB + streamB_[i]);
        offA += streamA_[i]; offB += streamB_[i];
        if (onLogical) {
            LogicalFrame f;
            f.stream = i; f.lenA = streamA_[i]; f.lenB = streamB_[i]; f.data = lf_.data(); f.index = muxIndex_; f.afterGap = gap_;
            onLogical(f);
        }
    }
    muxIndex_++;
    gap_ = false;
}

void DrmBackend::frame(const CellGrid& g) {
    if (mode_ < 0 || mode_ == kModeE) return;
    const ModeParams& mp = modeParams(mode_);
    const int before = pos_;
    const bool hadFac = facValid_;
    (void)hadFac;
    const uint64_t okBefore = facOk;
    decodeFac(g);
    int position = pos_;
    if (facOk != okBefore) {
        const int idn = fac_.identity;
        const int p = idn == 1 ? 1 : idn == 2 ? 2 : 0;
        if (before >= 0 && p != before && sfValid_) dropData();     // the identity does not follow the sequence: what was collected may belong to another super frame
        position = p;
    }
    if (position < 0) { pos_ = -1; return; }
    pos_ = (position + 1) % mp.frames;
    if (!layout_ || g.kmin != layout_->kmin || g.kmax != layout_->kmax) return;
    const Layout& L = *layout_;
    const int q = position;
    if (q == 0) {
        sfZ_.assign((size_t)L.nSfa, cf32(0, 0)); sfW_.assign((size_t)L.nSfa, 0.f);
        sdcZ_.assign((size_t)L.nSdc, cf32(0, 0)); sdcW_.assign((size_t)L.nSdc, 0.f);
        mscFilled_ = 0; framesInSf_ = 0; sfValid_ = true;
        for (int i = 0; i < L.nSdc; i++) {
            const auto& c = L.sdcCells[(size_t)i];
            sdcZ_[(size_t)i] = g.zAt(c.first, c.second);
            sdcW_[(size_t)i] = g.wAt(c.first, c.second);
        }
    }
    if (!sfValid_ || q != framesInSf_) { sfValid_ = false; return; }
    // MSC cells of this frame: those in symbols q * ns .. (q + 1) * ns - 1
    const auto cmp = [](const std::pair<int16_t, int16_t>& c, int sym) { return c.first < sym; };
    size_t i0 = (size_t)(std::lower_bound(L.mscCells.begin(), L.mscCells.end(), q * mp.ns, cmp) - L.mscCells.begin());
    size_t i1 = (size_t)(std::lower_bound(L.mscCells.begin(), L.mscCells.end(), (q + 1) * mp.ns, cmp) - L.mscCells.begin());
    if ((int)i0 != mscFilled_) { sfValid_ = false; return; }
    for (size_t i = i0; i < i1; i++) {
        const auto& c = L.mscCells[i];
        sfZ_[i] = g.zAt(c.first - q * mp.ns, c.second);
        sfW_[i] = g.wAt(c.first - q * mp.ns, c.second);
    }
    mscFilled_ = (int)i1;
    framesInSf_++;
    if (q != mp.frames - 1) return;
    // a whole super frame: the SDC, then the multiplex frames
    decodeSdc();
    if (!mscCode_) { mscBad++; return; }
    for (int f = 0; f < mp.frames; f++) {
        MuxFrame m;
        m.z.assign(sfZ_.begin() + (ptrdiff_t)f * L.nMux, sfZ_.begin() + (ptrdiff_t)(f + 1) * L.nMux);
        m.w.assign(sfW_.begin() + (ptrdiff_t)f * L.nMux, sfW_.begin() + (ptrdiff_t)(f + 1) * L.nMux);
        hist_.push_back(std::move(m));
        while ((int)hist_.size() > depth_) hist_.pop_front();
        decodeMux();
    }
    sfValid_ = false;
}

}} // namespace dect2::drm

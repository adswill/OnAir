// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM back end: FAC, SDC and MSC decoding from equalised cells.
#include <atomic>
#include "dect2/drm/drm_dec.h"
#include "dect2/common/diag.h"
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
    wRefFac_ = wRefSdc_ = wRefMux_ = 0; pendFacValid_ = false; pendPos_ = -1; announced_ = 0;
    facValid_ = false; sdcSeen_ = false; hierarchical_ = false;
    fac_ = FacInfo(); sdc_ = SdcInfo(); afs_.clear(); sdcPrevZ_.clear(); sdcPrevW_.clear(); sdcPrevN_ = 0;
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
    if (mode_ == kModeE) { occ_ = 0; layout_ = drm::layout(kModeE, 0); }   // mode E has one spectrum occupancy (100 kHz): its layout is known from the start
}

std::shared_ptr<const Layout> DrmBackend::layout() const { return layout_; }

int DrmBackend::mscQamBits() const { return mscQam_; }

void DrmBackend::frameLost() {
    dropData();
    pos_ = -1;
}

void DrmBackend::decodeFac(const CellGrid& g) {
    const bool E = mode_ == kModeE;
    const int nFac = E ? 244 : 65;
    // the FAC cells of a mode do not depend on the occupancy: use the cell table of any occupancy
    auto L = layout_ ? layout_ : drm::layout(mode_, E ? 0 : mode_ == kModeA ? 2 : mode_ == kModeB ? 2 : 3);
    if (!L) return;
    if (!facCode_) {   // clause 7.5.2: 4-QAM, rate 0.6 (mode E: 0.25)
        MlcParams f;
        f.levels = 1; f.n2 = nFac; f.fac = true; f.rxB[0] = E ? 1 : 3; f.ryB[0] = E ? 4 : 5;
        facCode_ = std::make_unique<MlcCode>(f);
    }
    std::vector<cf32> z((size_t)nFac);
    std::vector<float> w((size_t)nFac);
    double wSum = 0;
    for (int i = 0; i < nFac; i++) {
        const auto& c = L->facCells[(size_t)i];
        if (c.second < g.kmin || c.second > g.kmax || c.first >= g.ns) { DIAG_DEBUG_EVERY(758171, 5, "A DRM FAC cell lies outside the received carriers (carrier %d, range %d to %d), so this FAC block is counted as bad.", (int)c.second, g.kmin, g.kmax); facBad++; return; }
        z[(size_t)i] = g.zAt(c.first, c.second);
        w[(size_t)i] = g.wAt(c.first, c.second);
        wSum += w[(size_t)i];
    }
    facConst = z;
    // a fade: the cells are noise, the block is lost (its 8 bit CRC would pass one block of noise in 256)
    if (facValid_ && fadedBlock(wSum / nFac, wRefFac_)) { DIAG_DEBUG_EVERY(447837, 5, "The DRM FAC cells carry no usable signal (a fade), so this FAC block is skipped and the last good FAC stays in use."); facBad++; return; }
    std::vector<uint8_t> u((size_t)facCode_->infoBits()), pr(u.size());
    facCode_->decode(z.data(), w.data(), u.data(), 1);
    prbs(pr.data(), pr.size());
    for (size_t i = 0; i < u.size(); i++) u[i] ^= pr[i];
    FacInfo f;
    if (!facParse(u.data(), E, f) || !occupancyValid(mode_, f.occupancy)) { DIAG_DEBUG_EVERY(857861, 5, "A DRM FAC block failed its CRC or names an invalid spectrum occupancy, so it is discarded and the last good FAC stays in use."); facBad++; return; }
    learnWeight(wRefFac_, wSum / nFac);
    const bool first = !facValid_;
    auto sameConfig = [](const FacInfo& a, const FacInfo& b) {
        return a.occupancy == b.occupancy && a.interleaver == b.interleaver && a.mscMode == b.mscMode && a.sdcMode == b.sdcMode && a.numServicesCode == b.numServicesCode;
    };
    const bool changed = facValid_ && !sameConfig(f, fac_);
    if (announced_ > 0) announced_--;
    if (!changed) pendFacValid_ = false;
    else if (announced_ == 0 && (!pendFacValid_ || !sameConfig(f, pendFac_))) {
        // A new configuration is taken when the next block confirms it (a reconfiguration lasts; a block of noise that passed the CRC does not)
        pendFac_ = f; pendFacValid_ = true;
        DIAG_DEBUG_EVERY(130852, 5, "The DRM FAC announced a changed channel configuration without a reconfiguration notice, so it is held until the next block confirms it.");
        facRejected++;
        return;
    }
    pendFacValid_ = false;
    facOk++;
    if (f.reconfig != 0) announced_ = 12;   // the FAC announces a reconfiguration (its reconfiguration index): the changed FAC that follows is taken at once
    // what the change touches: the multiplex (occupancy, interleaver, constellation) or only the SDC and the service list
    const bool mscChanged = changed && (f.occupancy != fac_.occupancy || f.interleaver != fac_.interleaver || f.mscMode != fac_.mscMode);
    fac_ = f;
    facValid_ = true;
    for (int i = 0; i < f.nSvc && i < 2; i++) {     // mode E carries two sets of service parameters
        facSvc_[f.svc[i].shortId & 3] = f.svc[i];
        facSeen_[f.svc[i].shortId & 3] = true;
    }
    if (first && log) {
        char b[200];
        snprintf(b, sizeof b, "DRM: FAC read: mode %c, %s, %s interleaving, MSC %s, SDC %s", modeParams(mode_).name, occupancyName(E, f.occupancy), f.interleaver ? "short" : "long",
                 mscModeName(E, f.mscMode), E ? (f.sdcMode ? "4-QAM rate 1/4" : "4-QAM rate 1/2") : f.sdcMode ? "4-QAM" : "16-QAM");
        log(b);
    }
    if (changed && log) log("DRM: the channel parameters changed (reconfiguration)");
    if (first || mscChanged) {
        if (occ_ != f.occupancy) { occ_ = f.occupancy; layout_ = drm::layout(mode_, occ_); }
        sdcSeen_ = mscChanged ? false : sdcSeen_;
        dropData();
        if (mscChanged) { mscCode_.reset(); mscKey_ = -1; }
    }
    if (onFac) onFac(f);
}

void DrmBackend::buildMscCode() {
    if (!layout_ || !facValid_ || !sdc_.mux.present) { mscCode_.reset(); return; }
    const SdcMux& mx = sdc_.mux;
    int bits = 0;   // per cell: modes A to D 64-QAM or 16-QAM (the others are hierarchical), mode E 16-QAM or 4-QAM
    if (fac_.mscMode == 0) bits = mode_ == kModeE ? 4 : 6; else if (fac_.mscMode == 3) bits = mode_ == kModeE ? 2 : 4;
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
    if (mx.protB >= mscProtLevels(mode_, bits) || (X > 0 && mx.protA >= mscProtLevels(mode_, bits))) {
        DIAG_DEBUG_EVERY(832938, 5, "The DRM SDC names the MSC protection levels %d and %d, which this constellation does not have, so the MSC cannot be decoded.", mx.protA, mx.protB);
        return;
    }
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
    if (p.n2 < 20) {
        DIAG_DEBUG_EVERY(647768, 5, "The DRM multiplex leaves only %d cells for part B, too few to decode, so the MSC is skipped.", p.n2);
        return;
    }
    auto code = std::make_unique<MlcCode>(p);
    if (code->infoBits() < 8 * (X + Y)) { DIAG_EVERY(583554, 10, "The DRM SDC describes %d bytes of multiplex data but the frame can carry only %d, so the MSC is not decoded.", X + Y, code->infoBits() / 8); if (log) log("DRM: the multiplex description does not fit the multiplex frame"); return; }
    mscCode_ = std::move(code);
    mscQam_ = 1 << bits;
    depth_ = fac_.interleaver == 0 ? modeParams(mode_).depth : 1;
    if (log) {
        char b[200];
        snprintf(b, sizeof b, "DRM: multiplex: %d stream(s), %d bytes in part A and %d in part B, %d-QAM, protection %d/%d", mx.nStreams, X, Y, 1 << bits, mx.protA, mx.protB);
        log(b);
    }
}

std::atomic<bool> gDrmSdcCombine{true};   // tests compare with and without

void DrmBackend::decodeSdc() {
    if (!layout_ || !facValid_) return;
    const int mode = fac_.sdcMode;
    if (!sdcCode_ || sdcCodeMode_ != mode) {
        MlcParams s;
        s.n2 = layout_->nSdc;
        if (mode_ == kModeE) { s.levels = 1; s.rxB[0] = 1; s.ryB[0] = mode == 0 ? 2 : 4; }   // mode E: 4-QAM, rate 1/2 or 1/4
        else if (mode == 0) { s.levels = 2; s.rxB[0] = 1; s.ryB[0] = 3; s.rxB[1] = 2; s.ryB[1] = 3; }
        else { s.levels = 1; s.rxB[0] = 1; s.ryB[0] = 2; }
        sdcCode_ = std::make_unique<MlcCode>(s);
        sdcCodeMode_ = mode;
    }
    double sdcMeanW = 0;
    {   // a fade: nothing to decode
        double ws = 0;
        for (float v : sdcW_) ws += v;
        sdcMeanW = sdcW_.empty() ? 0 : ws / (double)sdcW_.size();
        if (fadedBlock(sdcMeanW, wRefSdc_)) { DIAG_DEBUG_EVERY(842605, 5, "The DRM SDC cells carry no usable signal (a fade), so this SDC block is skipped and the last service list stays."); sdcBad++; return; }
    }
    std::vector<uint8_t> u((size_t)sdcCode_->infoBits()), pr(u.size());
    sdcCode_->decode(sdcZ_.data(), sdcW_.data(), u.data(), 2);
    prbs(pr.data(), pr.size());
    for (size_t i = 0; i < u.size(); i++) u[i] ^= pr[i];
    sdcConst.assign(sdcZ_.begin(), sdcZ_.begin() + (ptrdiff_t)std::min<size_t>(sdcZ_.size(), 1024));
    const int F = sdcDataBytes(mode_, occ_, mode);
    int afs = 0;
    bool sdcGood = F >= 4 && sdcCheckBits(u.data(), F, afs);
    if (!sdcGood && F >= 4 && sdcPrevZ_.size() == sdcZ_.size() && gDrmSdcCombine.load(std::memory_order_relaxed)) {
        // The SDC usually repeats unchanged: the cells of the failed block added to those of the earlier blocks (weighted). A block whose content changed fails its CRC and nothing is lost;
        // only a block that passes the CRC is used.
        const size_t N = sdcZ_.size();
        std::vector<cf32> z(N); std::vector<float> w(N);
        for (size_t i = 0; i < N; i++) {
            const float wa = sdcW_[i], wb = sdcPrevW_[i], ws = wa + wb;
            z[i] = ws > 0 ? (sdcZ_[i] * wa + sdcPrevZ_[i] * wb) / ws : sdcZ_[i];
            w[i] = ws;
        }
        sdcCode_->decode(z.data(), w.data(), u.data(), 2);
        for (size_t i = 0; i < u.size(); i++) u[i] ^= pr[i];
        sdcGood = sdcCheckBits(u.data(), F, afs);
        if (sdcGood) sdcCombined++;
    }
    if (!sdcGood) {
        // keep the running sum of the failed blocks, at most about four frames' worth (weights are capped by resetting after a success)
        if (sdcPrevZ_.size() != sdcZ_.size() || sdcPrevN_ >= 3) { sdcPrevZ_ = sdcZ_; sdcPrevW_ = sdcW_; sdcPrevN_ = 1; }
        else {
            for (size_t i = 0; i < sdcZ_.size(); i++) {
                const float wa = sdcW_[i], wb = sdcPrevW_[i], ws = wa + wb;
                if (ws > 0) sdcPrevZ_[i] = (sdcZ_[i] * wa + sdcPrevZ_[i] * wb) / ws;
                sdcPrevW_[i] = ws;
            }
            sdcPrevN_++;
        }
        DIAG_DEBUG_EVERY(875038, 5, "A DRM SDC block failed its CRC, also after combining it with the earlier blocks, so it is discarded and the last service list stays.");
        sdcBad++; return;
    }
    sdcPrevZ_ = sdcZ_; sdcPrevW_ = sdcW_; sdcPrevN_ = 1;
    sdcOk++;
    learnWeight(wRefSdc_, sdcMeanW);
    std::vector<uint8_t> field((size_t)F, 0);
    for (int i = 0; i < F * 8; i++) field[(size_t)i / 8] |= (uint8_t)(u[(size_t)(4 + i)] << (7 - i % 8));
    const bool first = !sdcSeen_;
    sdcSeen_ = true;
    sdc_.afsIndex = afs;
    sdcParse(field.data(), F, sdc_);
    afs_.feed(field.data(), F);
    if (first && log) {
        char b[200];
        snprintf(b, sizeof b, "DRM: SDC read: %d data entities", sdc_.entities);
        log(b);
    }
    buildMscCode();
}

void DrmBackend::decodeMux(int txPos) {
    if (!mscCode_ || (int)hist_.size() < depth_) return;
    const int M = layout_->nMux;
    const std::vector<int>& pi = interleavePerm(M, 5);
    std::vector<cf32> z((size_t)M);
    std::vector<float> w((size_t)M);
    double wSum = 0;
    for (int i = 0; i < M; i++) {
        const MuxFrame& f = hist_[(size_t)(depth_ > 1 ? i % depth_ : 0)];
        z[(size_t)pi[(size_t)i]] = f.z[(size_t)i];
        w[(size_t)pi[(size_t)i]] = f.w[(size_t)i];
        wSum += f.w[(size_t)i];
    }
    const ModeParams& mp = modeParams(mode_);
    if (fadedBlock(wSum / M, wRefMux_)) {
        // the cells carry no signal (a fade longer than the interleaver can bridge): the frame is lost, the next one comes after a gap
        DIAG_DEBUG_EVERY(228561, 5, "The DRM MSC cells of a multiplex frame carry no signal (a fade longer than the time interleaver bridges), so the frame is lost and the audio has a gap.");
        mscSkipped++; mscBad++;
        muxIndex_++;
        gap_ = true;
        return;
    }
    learnWeight(wRefMux_, wSum / M);
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
            f.position = ((txPos - (depth_ - 1)) % mp.frames + mp.frames) % mp.frames;   // the time interleaver delays the data by depth - 1 frames
            onLogical(f);
        }
    }
    muxIndex_++;
    gap_ = false;
}

void DrmBackend::frame(const CellGrid& g) {
    if (mode_ < 0) return;
    const ModeParams& mp = modeParams(mode_);
    const int before = pos_;
    const bool hadFac = facValid_;
    (void)hadFac;
    const uint64_t okBefore = facOk;
    decodeFac(g);
    int position = pos_;
    if (facOk != okBefore) {
        const int idn = fac_.identity;
        const int p = mode_ == kModeE ? idn : idn == 1 ? 1 : idn == 2 ? 2 : 0;   // mode E: the four frames 00 .. 11; A to D: 11 is a first frame too
        if (before >= 0 && p != before) {
            // The identity does not follow the sequence. Taken when the next block confirms it (then what was collected may belong to another super
            // frame); a single odd block is more likely a FAC that was misread.
            if (p == pendPos_) { if (sfValid_) dropData(); position = p; pendPos_ = -1; }
            else { pendPos_ = (p + 1) % mp.frames; facRejected++; }
        } else {
            position = p;
            pendPos_ = -1;
        }
    } else if (pendPos_ >= 0) pendPos_ = (pendPos_ + 1) % mp.frames;
    if (position < 0) { pos_ = -1; return; }
    pos_ = (position + 1) % mp.frames;
    if (!layout_ || g.kmin != layout_->kmin || g.kmax != layout_->kmax) return;
    const Layout& L = *layout_;
    const int q = position;
    if (q == 0) {
        sfZ_.assign((size_t)L.nSfa, cf32(0, 0)); sfW_.assign((size_t)L.nSfa, 0.f);
        sdcZ_.assign((size_t)L.nSdc, cf32(0, 0)); sdcW_.assign((size_t)L.nSdc, 0.f);
        mscFilled_ = 0; framesInSf_ = 0; muxDone_ = 0; sfValid_ = true;
        for (int i = 0; i < L.nSdc; i++) {
            const auto& c = L.sdcCells[(size_t)i];
            sdcZ_[(size_t)i] = g.zAt(c.first, c.second);
            sdcW_[(size_t)i] = g.wAt(c.first, c.second);
        }
        // the SDC is all in the first frame: read it now, so that the multiplex frames can be decoded as their cells come in
        decodeSdc();
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
    // The multiplex frames whose cells are all in (each one is a third of the super frame's MSC cells, not aligned with the transmission frames):
    // decoded at once, so that the audio comes every 400 ms and not in bursts of a whole super frame.
    if (mscCode_) {
        while (muxDone_ < mp.frames && (muxDone_ + 1) * L.nMux <= mscFilled_) {
            const int f = muxDone_++;
            MuxFrame m;
            m.z.assign(sfZ_.begin() + (ptrdiff_t)f * L.nMux, sfZ_.begin() + (ptrdiff_t)(f + 1) * L.nMux);
            m.w.assign(sfW_.begin() + (ptrdiff_t)f * L.nMux, sfW_.begin() + (ptrdiff_t)(f + 1) * L.nMux);
            hist_.push_back(std::move(m));
            while ((int)hist_.size() > depth_) hist_.pop_front();
            decodeMux(f);
            if (!mscCode_) break;
        }
    }
    if (q != mp.frames - 1) return;
    if (!mscCode_) { DIAG_DEBUG_EVERY(754155, 10, "The DRM MSC cannot be decoded because the multiplex description from the SDC is missing or invalid, so this super frame is skipped."); mscBad++; }
    sfValid_ = false;
}

}} // namespace dect2::drm

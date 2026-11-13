// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog TV receiver, picture side. See atv_video.h. The state is in atv_video_impl.h; the bigger member functions are in
// atv_video_{sync,colour,picture}.cpp; this file holds the public AtvVideo class.
#include "atv_video_impl.h"

namespace dect2 {


// ------------------------------------------------------------------------------------------------ the class

AtvVideo::AtvVideo() : p_(std::make_unique<Impl>()) {}
AtvVideo::~AtvVideo() = default;
void AtvVideo::configure(double r, bool c) { p_->configure(r, c); p_->owner = this; }
void AtvVideo::reset() { Impl& s = *p_; std::fill(s.rv.begin(), s.rv.end(), 0.f); s.nAbs = 0; s.fullReset(); }
void AtvVideo::setParams(const AtvVideoParams& p) {
    Impl& s = *p_;
    const bool re = p.forceSys != s.prm.forceSys || p.forceColour != s.prm.forceColour || p.setupMode != s.prm.setupMode || p.chromaDelayNs != s.prm.chromaDelayNs;
    s.prm = p;
    // a forced system with the other polarity or another line count: the line search starts again
    const int fp = s.forcedPol();
    if ((fp != 0 && fp != s.pol) || (p.forceSys >= 0 && s.fmtKnown && atvSysLines(p.forceSys) != s.fmt.lines)) {
        if (fp != 0 && fp != s.pol) s.flipPolarity();
        s.resetSync();
        return;
    }
    if (re && s.fmtKnown) {
        if (p.forceColour >= 0 || !s.colourCapable) s.colourKind = s.startColour();
        s.makeFormat(s.fmt.lines);
    }
}
void AtvVideo::setSoundSpacing(double mhz) { p_->soundSpacing = mhz; }
void AtvVideo::setMirrored(bool on) { p_->mirroredHint = on; }
bool AtvVideo::positive() const { return p_->pol < 0; }
void AtvVideo::setSlicerWidth(double us) { p_->setSlicerWidth(us); p_->fullReset(); }
void AtvVideo::setNoiseScale(double s) { p_->noiseScale = s; }
void AtvVideo::setFmVideo(bool on) { p_->fmVideo = on; }
void AtvVideo::process(const float* v, const float* i, const float* q, size_t n) { p_->process(v, i, q, n); }
bool AtvVideo::wantSyncDetector() const { return p_->phaseLockedFlag && p_->loopRunning; }
void AtvVideo::syncDetectorChanged(bool on) {
    Impl& s = *p_;
    if (on == s.syncDetOn) return;
    s.syncDetOn = on;
    s.levelsRestart = 3;                            // the detectors differ in level: take the next lines as they are
}
bool AtvVideo::lineLocked() const { return p_->loopRunning && p_->syncQ > 0.5f; }
bool AtvVideo::fieldLocked() const { return lineLocked() && p_->haveVsync && p_->vsyncMiss < 6; }
double AtvVideo::secsSinceLineLock() const { return 0; }
double AtvVideo::soundSpacingHint() const { return p_->soundSpacing; }
AtvVideo::Info AtvVideo::info() const { return p_->info(); }
AtvFormat AtvVideo::format() const { return p_->fmt; }

} // namespace dect2

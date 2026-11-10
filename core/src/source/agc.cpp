#include "dect2/source/agc.h"
#include "dect2/sweep/station_health.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

// ------------------------------------------------------------------------------------------------------------------------------ gain stages
const std::vector<int>& rtlTunerGainTable() {
    // R820T/R820T2 tuner gains in tenths of a dB (librtlsdr), rounded to whole dB; duplicates removed
    static const std::vector<int> t = [] {
        const int tenths[] = {0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
        std::vector<int> v;
        for (int x : tenths) { const int d = (int)std::lround(x / 10.0); if (v.empty() || v.back() != d) v.push_back(d); }
        return v;
    }();
    return t;
}

GainModel gainModelFor(const DeviceInfo& d, int adcBits) {
    GainModel m;
    m.adcBits = adcBits;
    if (d.kind == DeviceInfo::HackRF) {
        m.kind = GainModel::HackRf;
        m.minDb = 0; m.maxDb = 40 + 62; m.stepDb = 2;
        m.ampSwitch = true; m.ampDb = 14;
        return m;
    }
    const int lo = (int)std::ceil(d.gainMinDb), hi = (int)std::floor(d.gainMaxDb);
    m.minDb = lo; m.maxDb = std::max(hi, lo + 1);
    if (d.board == "rtlsdr" || d.board == "rtltcp") {
        m.kind = GainModel::Table;
        m.table = rtlTunerGainTable();
        m.minDb = m.table.front(); m.maxDb = m.table.back();
    } else if (d.board == "airspyhf") {
        m.kind = GainModel::Range;
        m.stepDb = 6;
        m.minDb = 0; m.maxDb = 48;
        m.ampSwitch = true; m.ampDb = 6; m.ampIsTopOfScale = true; m.ampSettingKey = "preamp";
        m.levelCompensated = true;
    }
    return m;
}

int GainModel::minTotal(bool ampOn) const {
    return kind == HackRf && ampOn ? minDb + ampDb : minDb;
}

int GainModel::maxTotal(bool ampOn, bool ampAllowed, int lnaCap) const {
    if (kind == HackRf) return std::min(lnaCap, 40) + 62 + (ampOn ? ampDb : 0);
    if (ampSwitch && ampIsTopOfScale && !ampOn && !ampAllowed) return std::max(minDb, maxDb - ampDb);   // below where the driver switches the preamp on
    return maxDb;
}

int GainModel::snap(int db, int lo, int hi) const {
    db = std::max(lo, std::min(hi, db));
    if (kind == Table && !table.empty()) {
        int best = table.front();
        for (int v : table) if (v >= lo && v <= hi && std::abs(v - db) < std::abs(best - db)) best = v;
        if (best < lo || best > hi) {   // the range holds no table entry: the nearest one inside it
            best = -1;
            for (int v : table) if (v >= lo && v <= hi) { best = v; if (v >= db) break; }
            if (best < 0) best = lo;
        }
        return best;
    }
    const int st = std::max(1, stepDb);
    int v = minDb + (int)std::lround((double)(db - minDb) / st) * st;
    while (v > hi && v - st >= lo) v -= st;
    while (v < lo && v + st <= hi) v += st;
    return std::max(lo, std::min(hi, v));
}

int GainModel::nextToward(int cur, double delta, int lo, int hi) const {
    if (kind == Table && !table.empty()) {
        // the entry nearest cur + delta; at least the next entry when delta is not tiny
        int tgt = snap((int)std::lround(cur + delta), lo, hi);
        if (tgt == cur && std::fabs(delta) >= 1.0) {
            int best = cur;
            if (delta > 0) { for (int v : table) if (v > cur && v <= hi) { best = v; break; } }
            else { for (size_t i = table.size(); i-- > 0;) if (table[i] < cur && table[i] >= lo) { best = table[i]; break; } }
            tgt = best;
        }
        return tgt;
    }
    const int st = std::max(1, stepDb);
    int tgt = snap((int)std::lround(cur + delta), lo, hi);
    if (tgt == cur && std::fabs(delta) >= st * 0.5 && std::fabs(delta) >= 1.0) tgt = snap(cur + (delta > 0 ? st : -st), lo, hi);
    return tgt;
}

GainSetting GainModel::split(int total, bool ampOn, int lnaCap) const {
    GainSetting g;
    g.amp = ampOn && ampSwitch;
    const int r = std::max(0, total - (g.amp ? ampDb : 0));
    lnaCap = std::max(0, std::min(40, lnaCap / 8 * 8));
    int lna = std::min(lnaCap, std::max(0, (r - 8) / 8 * 8));   // LNA first: the best noise figure; the VGA keeps at least a little to trim with
    int vga = r - lna;
    if (vga > 62) { lna = std::min(lnaCap, lna + (vga - 62 + 7) / 8 * 8); vga = std::min(62, r - lna); }
    g.lna = lna;
    g.vga = std::max(0, std::min(62, vga)) & ~1;
    return g;
}

// ------------------------------------------------------------------------------------------------------------------------------ profile
AgcProfile agcProfileFor(int std_, double bwHz, double rateHz, int bits) {
    AgcProfile p;
    const bool pulsed = std_ == 13 || std_ == 45;                  // ADS-B, Mode S uplink: short pulses
    const bool strong = std_ == 7 || std_ == 23 || std_ == 28;     // FM broadcast, HD Radio, airband: strong nearby transmitters
    const bool wide = bwHz >= 1.0e6 && !pulsed;                    // TV, DAB, DTMB, Wi-Fi ...
    if (pulsed) { p.cls = AgcClass::Pulsed; p.name = "pulsed"; p.targetRms = -26; p.window = 4; p.maxPeak = 0.7; p.marginGoal = 12; p.oobCeil = -20; }
    else if (strong) { p.cls = AgcClass::Strong; p.name = "strong"; p.targetRms = -20; p.window = 3; p.maxPeak = 0.7; p.marginGoal = 6; p.oobCeil = -12; p.lnaCap = 24; }
    else if (wide) { p.cls = AgcClass::Wideband; p.name = "wideband"; p.targetRms = -16; p.window = 4; p.maxPeak = 0.8; p.marginGoal = 10; p.oobCeil = -8; }
    else { p.cls = AgcClass::Narrow; p.name = "narrow"; p.targetRms = -26; p.window = 4; p.maxPeak = 0.6; p.marginGoal = 14; p.oobCeil = -22; }
    (void)rateHz;
    if (bits > 8) p.targetRms = std::max(-34.0, p.targetRms - std::min(8.0, 1.5 * (bits - 8)));   // a deeper converter keeps more headroom for strong signals
    return p;
}

// ------------------------------------------------------------------------------------------------------------------------------ measurement
AgcMeasure measureFront(const SignalStats& st, const std::vector<float>& dbfs, double rateHz, double chanBwHz, double centreOffsetHz, int bits) {
    AgcMeasure m;
    m.rmsDbfs = st.rmsDbfs; m.peak = st.peak; m.clipFraction = st.clipFraction;
    const size_t n = dbfs.size();
    if (n < 64 || !(rateHz > 0) || !(chanBwHz > 0)) return m;
    const double binHz = rateHz / (double)n;
    // a channel filling most of the capture (TV): only its skirts are left outside it, so look closer in than for a narrow one
    const double edgeFrac = chanBwHz >= 1.0e6 ? 0.6 : 0.75;
    double inSum = 0, outSum = 0, blk = -300;
    std::vector<float> outside;
    size_t inN = 0;
    for (size_t i = 0; i < n; i++) {
        const double f = ((double)i - (double)n / 2.0) * binHz;
        if (std::fabs(f) < 2.5 * binHz) continue;
        const double d = std::fabs(f - centreOffsetHz);
        const float v = dbfs[i];
        if (!std::isfinite(v)) continue;
        const double p = std::pow(10.0, v / 10.0);
        if (d <= 0.5 * chanBwHz) { inSum += p; inN++; }
        else if (d >= edgeFrac * chanBwHz && std::fabs(f) < 0.92 * 0.5 * rateHz) { outSum += p; outside.push_back((float)p); blk = std::max<double>(blk, v); }   // the radio's anti-alias roll-off at the capture's edge is left out
    }
    m.haveSpec = true;
    // bins -> power: a Hann-windowed bin sums to 1.5 times the power it holds
    if (inSum > 0) m.inbandDbfs = 10.0 * std::log10(inSum / 1.5);
    if (outSum > 0) { m.oobDbfs = 10.0 * std::log10(outSum / 1.5); m.blockerBinDb = blk; }
    if (outside.size() >= 16) {
        const size_t k = outside.size() / 4;
        std::nth_element(outside.begin(), outside.begin() + (long)k, outside.end());
        const double floor = 10.0 * std::log10(std::max<double>(outside[k], 1e-30) * std::pow(10.0, 0.055));
        m.floorMarginDb = sweep::adcMarginDb(floor, binHz, rateHz, bits);
        m.haveFloor = true;
    }
    {   // the DC spike: the strongest of the middle bins over the median of the bins around them
        const size_t c = n / 2;
        double mx = -300;
        for (size_t i = c - 2; i <= c + 2 && i < n; i++) mx = std::max<double>(mx, dbfs[i]);
        std::vector<float> nb;
        for (size_t i = c - 12; i < c + 13 && i < n; i++) if ((i + 3 < c) || (i > c + 3)) nb.push_back(dbfs[i]);
        if (nb.size() > 4) { std::nth_element(nb.begin(), nb.begin() + (long)nb.size() / 2, nb.end()); m.dcSpurDb = mx - nb[nb.size() / 2]; }
    }
    return m;
}

// ------------------------------------------------------------------------------------------------------------------------------ control loop
const char* agcReasonName(AgcReason r) {
    switch (r) {
    case AgcReason::Settling: return "settling";
    case AgcReason::Hold: return "holding";
    case AgcReason::Overload: return "overload: gain cut";
    case AgcReason::TooHot: return "level too high";
    case AgcReason::TooLow: return "level too low";
    case AgcReason::Margin: return "noise too close to the ADC's";
    case AgcReason::Wasted: return "gain not needed";
    case AgcReason::Blocker: return "strong neighbour";
    case AgcReason::Locked: return "locked: gain frozen";
    case AgcReason::Reverted: return "worse quality: undone";
    case AgcReason::Recovering: return "recovering after a burst";
    case AgcReason::LimitHit: return "at the limit";
    case AgcReason::TopGain: return "least attenuation (the radio sets its own level)";
    default: return "";
    }
}

void SmartAgc::configure(const GainModel& m, const AgcProfile& p) {
    model_ = m;
    prof_ = p;
    lnaCap_ = p.lnaCap;
    begun_ = false;
}

int SmartAgc::effMax() const {
    int hi = model_.maxTotal(amp_, ampAllowed_, lnaCap_);
    if (model_.kind == GainModel::HackRf && ampAllowed_ && !amp_) hi = std::min(model_.maxTotal(true, true, lnaCap_), hi + model_.ampDb);   // the amp may join: the range grows
    return hi;
}
int SmartAgc::effMin() const { return model_.minTotal(amp_); }

void SmartAgc::begin(double now, int currentTotalDb, bool ampOn) {
    amp_ = ampOn;
    lnaCap_ = prof_.lnaCap;
    cur_ = model_.snap(currentTotalDb, effMin(), std::max(effMin(), effMax()));
    snapPending_ = cur_ != currentTotalDb;
    last_ = now;
    changedAt_ = now - 10;
    moveAt_ = -1e9;
    acquireUntil_ = now + 8;
    init_ = false;
    pending_ = false;
    dir_ = 0;
    preAttack_ = -1;
    ceilUntil_ = 0;
    ovTimes_.clear();
    ampBanUntil_ = 0;
    capChangedAt_ = -1e9;
    reason_ = AgcReason::Settling;
    begun_ = true;
}

void SmartAgc::apply(double now, int total, AgcReason why, AgcCommand& out) {
    cur_ = total;
    changedAt_ = now;
    init_ = false;   // judge the new level from scratch
    reason_ = why;
    out.totalDb = cur_;
    out.ampOn = amp_;
    out.lnaCap = lnaCap_;
    out.reason = why;
    if (model_.kind == GainModel::HackRf) out.stages = model_.split(cur_, amp_, lnaCap_);
}

static double db20(double x) { return 20.0 * std::log10(std::max(x, 1e-6)); }

bool SmartAgc::update(double now, const AgcMeasure& m, const AgcQuality& q, AgcCommand& out) {
    if (!begun_) return false;
    const AgcProfile& P = prof_;
    if (!init_) { rms_ = m.rmsDbfs; peak_ = m.peak; clip_ = m.clipFraction; oob_ = m.haveSpec ? m.oobDbfs : -200; margin_ = m.floorMarginDb; init_ = true; last_ = now; }
    const double dt = std::max(0.0, now - last_);
    last_ = now;
    const double kf = 1.0 - std::exp(-dt / 0.35), ks = 1.0 - std::exp(-dt / 1.5);
    rms_ += kf * (m.rmsDbfs - rms_);
    // overload is judged on the worst of the recent history: it rises at once and decays slowly
    peak_ = m.peak > peak_ ? m.peak : peak_ + ks * (m.peak - peak_);
    clip_ = m.clipFraction > clip_ ? m.clipFraction : clip_ + ks * (m.clipFraction - clip_);
    if (m.haveSpec) {
        oob_ = m.oobDbfs > oob_ ? m.oobDbfs : oob_ + ks * (m.oobDbfs - oob_);
        haveFloor_ = m.haveFloor;
        if (m.haveFloor) margin_ += kf * (m.floorMarginDb - margin_);
    }
    {   // the decoder's own quality, smoothed; starts from the first value after a lock
        const bool v = q.locked && q.snrValid;
        if (v) snr_ = snrValid_ ? snr_ + kf * (q.snrDb - snr_) : q.snrDb;
        snrValid_ = v;
    }

    const bool acquiring = now < acquireUntil_;
    if (now - changedAt_ > 5 && reason_ != AgcReason::Overload) steadyDb_ = cur_;   // the gain has stood still for a while
    const int lo0 = effMin();
    int hi0 = effMax();
    // ---- 1. overload: fast attack, regardless of settle time, lock or rate limit
    const bool overload = clip_ > 0.002 || peak_ >= 0.99;
    if (overload && now - changedAt_ >= 0.25) {
        const double amount = clip_ > 0.2 || rms_ > -4 ? 20 : (clip_ > 0.05 || rms_ > -8) ? 12 : clip_ > 0.01 ? 8 : 6;
        // a new episode: where to climb back to, if the gain had been steady (a start gain that was simply too high is no gain to return to)
        if (now - lastOverload_ > 20) preAttack_ = (!acquiring && now - changedAt_ > 5 && cur_ == steadyDb_) ? cur_ : -1;
        lastOverload_ = now;
        overloads_++;
        ovTimes_.push_back(now);
        while (!ovTimes_.empty() && now - ovTimes_.front() > 30) ovTimes_.erase(ovTimes_.begin());
        const bool repeated = ovTimes_.size() >= 3;   // overloading again and again: stay lower for longer
        ceil_ = cur_ - (repeated ? 6 : 2);
        ceilUntil_ = now + (repeated ? 90 : 10);
        pending_ = false;
        bool ampChanged = false;
        if (amp_ && ampAllowed_) { amp_ = false; ampBanUntil_ = now + 120; ampChanged = true; }   // the amp is the first stage to go
        // the amp's dB leave with it (the LNA and VGA stay), so switching it off is already a cut of its size
        int t = cur_ - (int)(ampChanged ? std::max<double>(amount, model_.ampDb) : amount);
        hi0 = effMax();
        t = model_.snap(t, effMin(), std::max(effMin(), hi0));
        if (t >= cur_ && !ampChanged) t = model_.nextToward(cur_, -amount, effMin(), std::max(effMin(), hi0));
        if (t == cur_ && !ampChanged) { reason_ = AgcReason::LimitHit; return false; }
        apply(now, std::min(t, cur_), AgcReason::Overload, out);
        dir_ = -1; moveAt_ = now;
        return true;
    }
    // ---- 2. settle time after a change
    const double settle = acquiring ? 0.45 : 0.8;
    if (now - changedAt_ < settle) { reason_ = AgcReason::Settling; return false; }

    // ---- 3. strong neighbours: the LNA sits before the baseband filter, so a neighbour is amplified by it in full. What reaches the end of the LNA is
    // estimated from what the ADC sees outside the channel less the VGA's gain; when that is too high the LNA gives up gain and the VGA takes it
    // (the total stays), and the LNA gets it back (with 3 dB of hysteresis) when the neighbour is gone.
    if (m.haveSpec && model_.kind == GainModel::HackRf && now - capChangedAt_ > 3) {
        const GainSetting now_ = model_.split(cur_, amp_, lnaCap_);
        const double excess = oob_ - now_.vga - P.lnaOutCeil;
        int cap = lnaCap_;
        if (excess > 0) cap = std::max(0, std::min(lnaCap_, (int)std::floor((now_.lna - excess) / 8.0) * 8));
        else if (lnaCap_ < P.lnaCap && oob_ - now_.vga + 8 <= P.lnaOutCeil - 3) cap = std::min(P.lnaCap, lnaCap_ + 8);
        if (cap != lnaCap_) {
            lnaCap_ = cap;
            capChangedAt_ = now;
            hi0 = effMax();
            const int t = std::min(cur_, std::max(effMin(), hi0));
            if (!(model_.split(t, amp_, lnaCap_) == now_)) {
                apply(now, t, AgcReason::Blocker, out);   // the same total gain (or the highest the new cap allows), split differently
                return true;
            }
        }
    }

    // ---- quality of the last upward step: if the decoder got worse, undo it
    if (pending_) {
        if (now - pendingAt_ > 6) pending_ = false;
        else if (snrValid_ && now - changedAt_ > 1.5) {
            pending_ = false;
            if (snr_ < snrBefore_ - 1.5) {
                ceil_ = upFrom_; ceilUntil_ = now + 30;
                apply(now, upFrom_, AgcReason::Reverted, out);
                dir_ = -1; moveAt_ = now;
                return true;
            }
        }
    }

    // ---- 4a. a radio that keeps its own output level (AirspyHF+): the level says nothing about the gain. The least attenuation the
    // radio allows (the best noise figure), down only when the peak nears full scale (clipping is handled above, with its ceiling)
    if (model_.levelCompensated) {
        hi0 = effMax();
        int lim = std::max(effMin(), hi0);
        if (now - lastOverload_ < 300) lim = std::min(lim, std::max(effMin(), (int)ceil_));   // a gain that clipped stays out of reach for 5 minutes
        if (snapPending_) { snapPending_ = false; apply(now, cur_, AgcReason::LimitHit, out); moveAt_ = now; return true; }   // begin() moved it into the allowed range
        if (cur_ < lim && now - moveAt_ >= (acquiring ? 0.5 : 2.5)) {
            const int t = model_.nextToward(cur_, lim - cur_, effMin(), lim);
            if (t != cur_) {
                pending_ = snrValid_; pendingAt_ = now; upFrom_ = cur_; snrBefore_ = snrValid_ ? snr_ : 0;
                apply(now, t, AgcReason::TopGain, out); dir_ = 1; moveAt_ = now; return true;
            }
        }
        reason_ = cur_ >= lim ? AgcReason::TopGain : AgcReason::Settling;
        return false;
    }

    // ---- 4. what the level and the neighbours ask for
    const double lo = P.targetRms - P.window, hi = P.targetRms + P.window;
    const double pkDb = db20(peak_), pkMax = db20(P.maxPeak);
    double needDown = 0;
    AgcReason why = AgcReason::Hold;
    if (rms_ > hi) { needDown = rms_ - P.targetRms; why = AgcReason::TooHot; }
    if (pkDb > pkMax && pkDb - pkMax + 2 > needDown) { needDown = pkDb - pkMax + 2; why = AgcReason::TooHot; }
    if (m.haveSpec && oob_ > P.oobCeil && oob_ - P.oobCeil + 1 > needDown) { needDown = oob_ - P.oobCeil + 1; why = AgcReason::Blocker; }
    if (needDown <= 0 && haveFloor_ && margin_ > P.marginGoal + 14 && rms_ > lo) { needDown = std::min(4.0, margin_ - (P.marginGoal + 8)); why = AgcReason::Wasted; }

    double needUp = 0;
    if (needDown <= 0) {
        if (rms_ < lo) { needUp = P.targetRms - rms_; why = AgcReason::TooLow; }
        if (haveFloor_ && margin_ < P.marginGoal - 1.5 && rms_ < hi - 2 && P.marginGoal - margin_ > needUp) { needUp = P.marginGoal - margin_; why = AgcReason::Margin; }
        if (needUp > 0) {   // never into a limit
            double room = 1e9;
            if (pkDb < pkMax) room = std::min(room, pkMax - pkDb - 1);
            if (m.haveSpec) room = std::min(room, P.oobCeil - oob_ - 1);
            room = std::min(room, hi - rms_);
            if (now < ceilUntil_) room = std::min(room, (double)ceil_ - cur_);
            if (room < 1.0) { needUp = 0; why = (m.haveSpec && P.oobCeil - oob_ - 1 < 1.0) ? AgcReason::Blocker : (now < ceilUntil_ ? AgcReason::Recovering : AgcReason::Hold); }
            else needUp = std::min(needUp, room);
        }
    }
    // after a burst the gain climbs back to where it was, faster than it normally rises
    const bool recovering = preAttack_ >= 0 && cur_ < preAttack_ && now - lastOverload_ < 90;
    if (needDown <= 0 && needUp <= 0 && recovering && now - lastOverload_ > 2.5 && rms_ < P.targetRms - 1 && !(now < ceilUntil_ && cur_ >= ceil_)) {
        needUp = std::min<double>(std::min<double>(4.0, preAttack_ - cur_), P.targetRms - rms_);
        why = AgcReason::Recovering;
        if (needUp > 0 && m.haveSpec) needUp = std::min(needUp, P.oobCeil - oob_ - 1);
    }
    if (needDown <= 0 && needUp < 1.0) {
        if (preAttack_ >= 0 && (cur_ >= preAttack_ || now - lastOverload_ > 90)) preAttack_ = -1;
        reason_ = (why == AgcReason::Recovering || why == AgcReason::Blocker) ? why : AgcReason::Hold;
        return false;
    }
    const bool down = needDown > 0;
    if (down && needDown < 1.0) { reason_ = AgcReason::Hold; return false; }

    // ---- 5. a decoder locked on good data keeps the gain where it is (a hot level still comes down)
    const bool lockedGood = q.locked && (!q.snrValid || q.snrDb >= P.lockSnrDb);
    if (lockedGood && !(down && needDown > P.window + 2)) { reason_ = AgcReason::Locked; return false; }

    // ---- 6. rate limits and hysteresis (no hunting: a reversal needs a clearly larger error)
    const double gapDown = acquiring ? 0.3 : 0.8, gapUp = acquiring ? 0.5 : (recovering ? 1.2 : 2.5);
    if (now - moveAt_ < (down ? gapDown : gapUp)) { reason_ = AgcReason::Settling; return false; }
    if (!acquiring && dir_ != 0 && (down ? -1 : 1) != dir_ && now - moveAt_ < 8.0) {
        if (why != AgcReason::Blocker && (down ? needDown : needUp) < P.window * 1.5) { reason_ = AgcReason::Hold; return false; }
    }
    const double maxStep = acquiring ? 24 : (down ? 10 : 6);
    const double delta = (down ? -1 : 1) * std::min(maxStep, down ? needDown : needUp);
    hi0 = effMax();
    int t = model_.nextToward(cur_, delta, effMin(), std::max(effMin(), hi0));
    // a coarse radio (6 dB steps) must not overshoot the dead band: a step that would land beyond the opposite edge is not taken
    if (t != cur_ && !acquiring && why != AgcReason::Blocker && why != AgcReason::Recovering) {
        const double pred = rms_ + (t - cur_);
        if ((down && pred < lo - (why == AgcReason::Wasted ? -1 : 1)) || (!down && pred > hi + 1)) t = cur_;   // gain not needed: never down into the low side of the dead band
    }
    if (t == cur_) { reason_ = AgcReason::LimitHit; return false; }
    if (!down) {
        pending_ = true; pendingAt_ = now; upFrom_ = cur_; snrBefore_ = snrValid_ ? snr_ : 0;
        if (!snrValid_) pending_ = false;
    } else pending_ = false;
    // ---- the amp, only when the user allowed it: on for the best noise figure when a lot of gain is wanted and nothing strong is near, off again with margin
    if (model_.kind == GainModel::HackRf && ampAllowed_) {
        const bool quiet = !m.haveSpec || oob_ < P.oobCeil - 12;
        if (!amp_ && !down && t >= 70 && quiet && now >= ampBanUntil_ && now - lastOverload_ > 60) { amp_ = true; }
        else if (amp_ && (t < 56 || !quiet)) { amp_ = false; }
    }
    apply(now, t, why, out);
    dir_ = down ? -1 : 1;
    moveAt_ = now;
    return true;
}

} // namespace dect2

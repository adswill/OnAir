// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Lightning position (network): one station's processing and the server's (ltpos_core.h).
#include "dect2/sferics/ltpos_core.h"
#include <algorithm>
#include <cmath>

namespace dect2::ltpos {

const std::vector<RefTx>& builtinRefs() {
    // VLF / LF MSK stations, 200 Bd unless noted. Positions rounded to about 100 m from public listings; the doc says to check them.
    static const std::vector<RefTx> r = {
        {1, "NAA 24.0 kHz (Cutler, US)", 24000, 200, 44.6464, -67.2810},
        {2, "NLK 24.8 kHz (Jim Creek, US)", 24800, 200, 48.2036, -121.9171},
        {3, "NML 25.2 kHz (LaMoure, US)", 25200, 200, 46.3661, -98.3355},
        {4, "NPM 21.4 kHz (Lualualei, US)", 21400, 200, 21.4202, -158.1511},
        {5, "NAU 40.75 kHz (Aguada, PR)", 40750, 200, 18.3988, -67.1776},
        {6, "GBZ 19.58 kHz (Anthorn, UK)", 19580, 200, 54.9117, -3.2786},
        {7, "GQD 22.1 kHz (Anthorn, UK)", 22100, 200, 54.9117, -3.2786},
        {8, "DHO38 23.4 kHz (Rhauderfehn, DE)", 23400, 200, 53.0789, 7.6150},
        {9, "ICV 20.27 kHz (Tavolara, IT)", 20270, 200, 40.9230, 9.7310},
        {10, "HWU 18.3 kHz (Rosnay, FR)", 18300, 200, 46.7130, 1.2450},
        {11, "NSY 45.9 kHz (Niscemi, IT)", 45900, 200, 37.1260, 14.4360},
        {12, "TBB 26.7 kHz (Bafa, TR)", 26700, 200, 37.4120, 27.3230},
        {13, "JXN 16.4 kHz (Novik, NO)", 16400, 200, 66.9824, 13.8730},
        {14, "VTX 18.2 kHz (Vijayanarayanam, IN)", 18200, 200, 8.3864, 77.7528},
        {15, "NWC 19.8 kHz (Exmouth, AU)", 19800, 200, -21.8163, 114.1658},
        {16, "JJI 22.2 kHz (Ebino, JP)", 22200, 200, 32.0920, 130.8290},
    };
    return r;
}

const RefTx* refById(int id) {
    for (const RefTx& r : builtinRefs()) if (r.id == id) return &r;
    return nullptr;
}

// ---------------------------------------------------------------- station
void StationCore::configure(double rate, const StationCfg& c) {
    rate_ = rate;
    c_ = c;
    pt_.configure(rate);
    pt_.setThresholdDb(c.thresholdDb);
    if (c.refOn && c.ref.baud > 0) msk_.configure(rate, c.refOffsetHz, c.ref.baud, c.ref.id, 1.0);
    nr_ = rate > 0 ? std::exp(-2 * 3.14159265358979 * 300.0 / rate) : 0;
    nw_ = rate > 0 ? std::polar(1.0, 2 * 3.14159265358979 * c.refOffsetHz / rate) : std::complex<double>(1, 0);
    reset();
}

void StationCore::reset() {
    n_ = 0;
    nz1_ = nz2_ = 0;
    pt_.reset();
    msk_.reset();
    hw_.reset(); gnss_.reset(); cv_.reset(); sys_.reset();
    cvAnchored_ = false; cvAnchor_ = 0; cvRestarts_ = cv_.restarts();
    refPending_ = false;
    out_.clear(); outSrc_ = TimeSrc::None;
    lastPulseS_ = -1e9; pulses_ = 0; lastPeak_ = 0;
}

void StationCore::addMark(const TimeMark& m) {
    if (m.src == TimeSrc::GpsHw) hw_.add(m);
    else if (m.src == TimeSrc::Gnss) gnss_.add(m);
    else if (m.src == TimeSrc::Ntp || m.src == TimeSrc::Ptp || m.src == TimeSrc::System) sys_.add(m);
}

bool StationCore::fresh(const SampleClock& c) const { return c.valid() && sampleS() - c.lastMarkS() <= c_.maxMarkAgeS; }

const SampleClock* StationCore::clockFor(TimeSrc s) const {
    switch (s) {
    case TimeSrc::GpsHw: return fresh(hw_) ? &hw_ : nullptr;
    case TimeSrc::Gnss: return fresh(gnss_) ? &gnss_ : nullptr;
    case TimeSrc::CommonView: return c_.refOn && cvAnchored_ && fresh(cv_) ? &cv_ : nullptr;
    case TimeSrc::Ptp: case TimeSrc::Ntp: case TimeSrc::System: return sys_.valid() && sys_.source() == s ? &sys_ : nullptr;
    default: return nullptr;
    }
}

double StationCore::sourceSigmaNs(TimeSrc s) const {
    const SampleClock* c = clockFor(s);
    if (!c) return -1;
    int64_t u; double sg;
    return c->stamp(sampleS(), u, sg) ? sg : -1;
}

TimeSrc StationCore::activeSource(double& sigmaNs, double& ppm, double& ppmSigma) const {
    for (TimeSrc s : {TimeSrc::GpsHw, TimeSrc::Gnss, TimeSrc::CommonView, TimeSrc::Ptp, TimeSrc::Ntp, TimeSrc::System}) {
        const SampleClock* c = clockFor(s);
        if (!c) continue;
        int64_t u;
        c->stamp(sampleS(), u, sigmaNs);
        ppm = c->ppm(); ppmSigma = c->ppmSigma();
        return s;
    }
    sigmaNs = -1; ppm = 0; ppmSigma = 0;
    return TimeSrc::None;
}

void StationCore::feed(const std::complex<float>* x, size_t n, int64_t nowNs) {
    if (rate_ <= 0 || n == 0) return;
    pbuf_.clear(); rbuf_.clear();
    if (c_.refOn) {
        msk_.feed(x, n, rbuf_);
        nbuf_.resize(n);
        for (size_t i = 0; i < n; i++) {
            const std::complex<double> v(x[i]);
            nz1_ = (1 - nr_) * v + nr_ * nw_ * nz1_;
            const std::complex<double> a = v - nz1_;
            nz2_ = (1 - nr_) * a + nr_ * nw_ * nz2_;
            nbuf_[i] = std::complex<float>(a - nz2_);
        }
        pt_.feed(nbuf_.data(), n, pbuf_);
    } else pt_.feed(x, n, pbuf_);
    n_ += n;
    if (nowNs > 0) sys_.add(systemClockMark(c_.sysKind, sampleS(), nowNs, c_.sysSigmaUs, (double)n / rate_));
    // the common-view clock: each symbol boundary is a mark on the transmitter's time grid (anchor + k T), k counted with the coarse clock
    for (const RefObs& o : rbuf_) {
        const SampleClock* abs = clockFor(TimeSrc::GpsHw);
        if (!abs) abs = clockFor(TimeSrc::Gnss);
        const SampleClock* coarse = abs ? abs : (sys_.valid() ? &sys_ : nullptr);
        if (!coarse) continue;
        int64_t cu; double cs;
        coarse->stamp(o.sampleS, cu, cs);
        const double T = 1e9 / c_.ref.baud;
        if (!cvAnchored_) { cvAnchor_ = cu; cvAnchored_ = true; cv_.reset(); cvRestarts_ = cv_.restarts(); }
        const double k = std::round((double)(cu - cvAnchor_) / T);
        TimeMark m;
        m.src = TimeSrc::CommonView;
        m.sampleS = o.sampleS;
        m.utcNs = cvAnchor_ + (int64_t)std::llround(k * T);
        m.sigmaNs = o.sigmaS * 1e9;
        cv_.add(m);
        if (cv_.restarts() != cvRestarts_) {   // the timeline jumped: anchor again
            cvAnchor_ = cu; cv_.reset(); cvRestarts_ = cv_.restarts();
            m.utcNs = cvAnchor_;
            cv_.add(m);
        }
        RefRec r;
        r.refId = c_.ref.id;
        r.baud = c_.ref.baud;
        r.snrDb = o.snrDb;
        if (abs) {   // a GPS-timed station reads the boundary in UTC: it ties the reference's epoch for everyone
            int64_t au; double as;
            abs->stamp(o.sampleS, au, as);
            r.anchorNs = au;
            r.sigmaNs = std::hypot(as, o.sigmaS * 1e9);
        } else {
            int64_t vu; double vs;
            cv_.stamp(o.sampleS, vu, vs);
            r.anchorNs = vu;
            r.sigmaNs = vs;
        }
        ref_ = r;
        refPending_ = true;
    }
    for (const Pulse& p : pbuf_) {
        if (p.sampleS - lastPulseS_ < c_.mergeS) continue;   // its own ringing or sky wave
        lastPulseS_ = p.sampleS;
        pulses_++;
        lastPeak_ = p.peakDb;
        double sg, ppm, pps;
        const TimeSrc s = activeSource(sg, ppm, pps);
        const SampleClock* c = clockFor(s);
        if (!c) continue;
        DetRec d;
        double cs;
        c->stamp(p.sampleS, d.utcNs, cs);
        d.sigmaNs = std::hypot(cs, p.sigmaS * 1e9);
        d.peakDb = p.peakDb; d.amp = p.amp; d.widthUs = p.widthUs; d.riseUs = p.riseUs; d.freqHz = p.freqHz;
        if (!out_.empty() && s != outSrc_) continue;   // the source changed under a batch not yet taken: wait for the next one
        outSrc_ = s;
        outSigma_ = sg;
        out_.push_back(d);
    }
}

std::vector<DetRec> StationCore::takeDetections(TimeSrc& src, double& clockSigmaNs) {
    std::vector<DetRec> v;
    v.swap(out_);
    src = outSrc_;
    clockSigmaNs = outSigma_;
    return v;
}

bool StationCore::takeRef(RefRec& out) {
    if (!refPending_) return false;
    out = ref_;
    refPending_ = false;
    return true;
}

// ---------------------------------------------------------------- server
ServerCore::ServerCore(const std::string& key, MatchOpts mo, PropModel pm) : mo_(mo), pm_(pm), m_(mo, pm) {
    setKey(key);
    setRefs(builtinRefs());
}

void ServerCore::setKey(const std::string& key) { key_ = networkKey(key); }

void ServerCore::setOpts(const MatchOpts& mo, const PropModel& pm) { mo_ = mo; pm_ = pm; m_.setOpts(mo); m_.setProp(pm); }

void ServerCore::setRefs(const std::vector<RefTx>& refs) {
    for (const RefTx& r : refs) cv_[r.id].setRef(r);
}

ServerCore::In ServerCore::ingest(const uint8_t* d, size_t n, int64_t nowNs) {
    Packet p;
    const PktErr e = decodePacket(d, n, key_, p);
    if (e == PktErr::Auth) { auth_++; return In::Auth; }
    if (e != PktErr::Ok) { bad_++; return In::Malformed; }
    if (!allowed_.empty() && std::find(allowed_.begin(), allowed_.end(), p.station) == allowed_.end()) { auth_++; return In::NotAllowed; }
    if (st_.size() >= kLtStationsMax && !st_.count(p.station)) { bad_++; return In::Full; }
    switch (guard_.check(p.station, p.seq, p.sendNs, nowNs)) {
    case ReplayGuard::Fresh: break;
    case ReplayGuard::Full: bad_++; return In::Full;
    default: replay_++; return In::Replay;
    }
    ok_++;
    apply(p, nowNs);
    return In::Ok;
}

void ServerCore::local(const Packet& p, int64_t nowNs) { apply(p, nowNs); }

void ServerCore::apply(const Packet& p, int64_t nowNs) {
    ServerStation& s = st_[p.station];
    s.id = p.station;
    s.hdr = p.hdr;
    s.lastNs = nowNs;
    if (p.type == PktType::Hello) { s.name = p.name; return; }
    const bool absolute = p.hdr.src == TimeSrc::GpsHw || p.hdr.src == TimeSrc::Gnss;
    if (p.type == PktType::Reference) {
        auto it = cv_.find(p.ref.refId);
        if (it == cv_.end() || std::fabs(it->second.ref().baud - p.ref.baud) > 1e-3) return;   // a reference this server does not know
        it->second.report(p.station, p.ref.anchorNs, p.ref.sigmaNs, p.hdr.lat, p.hdr.lon, absolute, pm_);
        stRef_[p.station] = p.ref.refId;
        s.hasRef = true;
        return;
    }
    for (const DetRec& r : p.dets) {
        Det d;
        d.station = p.station;
        d.lat = p.hdr.lat; d.lon = p.hdr.lon;
        d.peakDb = r.peakDb; d.amp = r.amp; d.widthUs = r.widthUs; d.riseUs = r.riseUs; d.freqHz = r.freqHz;
        d.src = p.hdr.src;
        d.tNs = r.utcNs;
        d.sigmaNs = r.sigmaNs;
        if (p.hdr.src == TimeSrc::CommonView) {
            const auto rf = stRef_.find(p.station);
            double corr = 0, cs = 0;
            bool tied = false;
            if (rf != stRef_.end() && cv_[rf->second].correction(p.station, corr, cs, tied)) {
                d.tNs = r.utcNs + (int64_t)std::llround(corr);
                d.sigmaNs = std::hypot(r.sigmaNs, cs);
                d.group = tied ? 0 : 100 + rf->second;
                d.ambNs = cv_[rf->second].periodNs();
                s.corrNs = corr; s.corrSigmaNs = cs; s.tied = tied;
            } else {
                d.sigmaNs = std::max(r.sigmaNs, timeSrcTypicalNs(TimeSrc::Ntp));   // no reading of the reference yet: coarse
                s.tied = true;
            }
        } else s.tied = true;
        d.fine = fineTiming(d.src, d.sigmaNs);
        s.dets++;
        s.sigmaNs = d.sigmaNs;
        m_.add(d);
    }
}

std::vector<Strike> ServerCore::process(int64_t nowNs) {
    std::vector<Strike> v = m_.process(nowNs);
    for (const Strike& k : v) for (uint32_t id : k.stations) { auto it = st_.find(id); if (it != st_.end()) it->second.used++; }
    return v;
}

LtStrikeRow strikeRow(const Strike& s) {
    LtStrikeRow r;
    r.utcNs = s.timeNs;
    r.lat = s.fix.lat; r.lon = s.fix.lon;
    r.majorKm = (float)(s.fix.semiMajorM / 1e3); r.minorKm = (float)(s.fix.semiMinorM / 1e3); r.azDeg = (float)s.fix.majorAzDeg;
    r.stations = (int)s.stations.size();
    r.alsoHeard = (int)s.alsoHeard.size();
    r.rmsUs = (float)(s.fix.rmsS * 1e6);
    r.chi2 = (float)s.fix.chi2;
    r.dof = s.fix.dof;
    r.unchecked = s.fix.unchecked;
    r.ambiguous = s.fix.ambiguous;
    r.utc = s.utc;
    return r;
}

} // namespace dect2::ltpos

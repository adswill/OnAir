// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Lightning position (network): one station's processing (StationCore: time the sferics, discipline the sample clock, pick the best timing
// source, produce the detections and the common-view readings) and the server's (ServerCore: check and decode packets, keep the station table,
// correct the common-view clocks, match and locate). Both are pure (no sockets, no clock): the receiver (ltpos_rx.h) and the tests drive them.
#pragma once
#include "dect2/sferics/ltpos.h"
#include "dect2/sferics/ltpos_tel.h"
#include <complex>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2::ltpos {

// Well-known MSK stations for the common-view reference (positions and rates from public listings, rounded; check before relying on them).
const std::vector<RefTx>& builtinRefs();
const RefTx* refById(int id);

struct StationCfg {
    double thresholdDb = 12;
    double mergeS = 0.001;        // pulses within this of a detection are its ringing / sky wave, not a new strike
    int sysKind = 1;              // the computer's clock: 0 not synchronised, 1 NTP, 2 PTP
    double sysSigmaUs = 0;        // 0: the typical figure for that kind
    bool refOn = false;
    RefTx ref;
    double refOffsetHz = 0;       // the reference carrier relative to the tuned centre
    double maxMarkAgeS = 5;       // a GPS / GNSS clock with no mark for this long is not used
};

class StationCore {
public:
    void configure(double rate, const StationCfg& c);
    const StationCfg& cfg() const { return c_; }
    void reset();
    // marks from the radio's hardware time (GPS) or OnAir's GNSS receiver; the system clock is marked by feed()
    void addMark(const TimeMark& m);
    // samples; nowNs > 0: the system clock when this block arrived (it marks the end of the block)
    void feed(const std::complex<float>* x, size_t n, int64_t nowNs);
    double sampleS() const { return rate_ > 0 ? (double)n_ / rate_ : 0; }
    // what feed() produced since the last call
    std::vector<DetRec> takeDetections(TimeSrc& src, double& clockSigmaNs);
    bool takeRef(RefRec& out);
    // the clock in use now and the alternatives
    TimeSrc activeSource(double& sigmaNs, double& ppm, double& ppmSigma) const;
    double sourceSigmaNs(TimeSrc s) const;   // -1: not available now
    const PulseTimer& pulses() const { return pt_; }
    const MskRefTimer& refTimer() const { return msk_; }
    uint64_t pulseCount() const { return pulses_; }
    float lastPeakDb() const { return lastPeak_; }
private:
    const SampleClock* clockFor(TimeSrc s) const;
    bool fresh(const SampleClock& c) const;
    double rate_ = 0;
    uint64_t n_ = 0;
    StationCfg c_;
    PulseTimer pt_;
    MskRefTimer msk_;
    SampleClock hw_{60}, gnss_{60}, cv_{300}, sys_{120};
    int64_t cvAnchor_ = 0;
    bool cvAnchored_ = false;
    uint64_t cvRestarts_ = 0;
    bool refPending_ = false;
    RefRec ref_;
    // a double notch on the reference carrier ahead of the sferic timer, so the strong station does not lift the noise floor
    std::complex<double> nz1_ = 0, nz2_ = 0, nw_ = 1;
    double nr_ = 0;
    std::vector<std::complex<float>> nbuf_;
    std::vector<Pulse> pbuf_;
    std::vector<RefObs> rbuf_;
    std::vector<DetRec> out_;
    TimeSrc outSrc_ = TimeSrc::None;
    double outSigma_ = 0;
    double lastPulseS_ = -1e9;
    uint64_t pulses_ = 0;
    float lastPeak_ = 0;
};

struct ServerStation {
    uint32_t id = 0;
    std::string name;
    StationHdr hdr;
    int64_t lastNs = 0;
    uint64_t dets = 0, used = 0;
    bool hasRef = false;
    double corrNs = 0, corrSigmaNs = 0;
    bool tied = true;
    double sigmaNs = 0;           // the timing error of its latest detection after correction
};

class ServerCore {
public:
    explicit ServerCore(const std::string& key = std::string(), MatchOpts mo = MatchOpts(), PropModel pm = PropModel());
    void setKey(const std::string& key);
    void setOpts(const MatchOpts& mo, const PropModel& pm);
    void setRefs(const std::vector<RefTx>& refs);   // the references the stations may use (by id)
    void setAllowed(const std::vector<uint32_t>& ids) { allowed_ = ids; }   // empty: any station that knows the key
    enum class In { Ok, Auth, Replay, Malformed, NotAllowed, Full };
    In ingest(const uint8_t* d, size_t n, int64_t nowNs);   // one datagram
    void local(const Packet& p, int64_t nowNs);              // the server's own station, no encoding
    std::vector<Strike> process(int64_t nowNs);
    const std::map<uint32_t, ServerStation>& stations() const { return st_; }
    const MatchStats& stats() const { return m_.stats(); }
    uint64_t ok() const { return ok_; }
    uint64_t authFail() const { return auth_; }
    uint64_t replays() const { return replay_; }
    uint64_t bad() const { return bad_; }
private:
    void apply(const Packet& p, int64_t nowNs);
    std::vector<uint8_t> key_;
    MatchOpts mo_;
    PropModel pm_;
    Matcher m_;
    ReplayGuard guard_;
    std::map<int, CommonViewAligner> cv_;
    std::map<uint32_t, ServerStation> st_;
    std::map<uint32_t, int> stRef_;   // the reference each station last reported
    std::vector<uint32_t> allowed_;
    uint64_t ok_ = 0, auth_ = 0, replay_ = 0, bad_ = 0;
};

// one strike as a telemetry row
LtStrikeRow strikeRow(const Strike& s);

} // namespace dect2::ltpos

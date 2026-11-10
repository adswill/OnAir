// Service off-air detector (3.9, docs/SMART5-tv-3.9.md item 11). A transport stream keeps its PAT, PMT and null packets going
// while one service's encoder stops: the mux looks healthy and the receiver shows a frozen or black picture. This tracker learns
// each elementary PID's usual bitrate and reports the service "off air at the source" when its video or audio PID stops, or falls
// to a small fraction of its usual rate, while the mux itself (total packets, continuity) is fine.
// Feed: mapPid() from the PMT, packet() for every TS packet (or counts per PID), tick() once a second.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct PidOffAirParams {
    double learnSec = 20;      // seconds of normal rate before a PID is judged
    double dropFrac = 0.1;     // below this share of the usual rate counts as stopped
    double holdSec = 3;        // that long before it is reported
    double ccBadPct = 1;       // the mux is "fine" with continuity errors below this share of packets
};

struct OffAirEvent {
    unsigned service = 0;
    uint16_t pid = 0;
    bool video = false;
    double since = 0;
    double usualKbps = 0, nowKbps = 0;
};

class PidOffAir {
public:
    explicit PidOffAir(PidOffAirParams p = {}) : p_(p) {}
    void mapPid(unsigned service, uint16_t pid, bool video);
    void packet(uint16_t pid, bool ccError = false) { cur_[pid]++; total_++; if (ccError) cc_++; }
    void packets(uint16_t pid, unsigned n) { cur_[pid] += n; total_ += n; }
    void tick(double now, double sec = 1.0);
    bool muxFine() const { return muxFine_; }
    std::vector<OffAirEvent> offAir() const;                 // PIDs reported now
    std::vector<OffAirEvent> log() const { return log_; }    // every report, with its start time
    std::string text(const OffAirEvent& e) const;
private:
    struct Pid { unsigned service = 0; bool video = false; double ewma = 0, learned = 0, lowFor = 0, since = 0, nowRate = 0; bool off = false; };
    PidOffAirParams p_;
    std::map<uint16_t, Pid> pids_;
    std::map<uint16_t, unsigned> cur_;
    unsigned total_ = 0, cc_ = 0;
    double totalEwma_ = 0;
    bool muxFine_ = true;
    std::vector<OffAirEvent> log_;
};

} // namespace smart5
} // namespace dect2

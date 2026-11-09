// ETSI TR 101 290 stream measurements on a live transport stream: priority 1, 2 and 3 checks, each with a counter and the seconds since it last fired. A separate
// monitor, driven by the packets as they arrive; `t` is the signal time of the packet (seconds), which on an RF chain comes from the sample clock.
#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct Tr290Check { const char* name = ""; int priority = 1; uint64_t count = 0; double lastSec = -1; };

class Tr290Monitor {
public:
    Tr290Monitor();
    void feed(const uint8_t* pkt188, double t);           // one packet, in stream order
    void reset();
    const std::vector<Tr290Check>& checks() const { return c_; }
    uint64_t packets() const { return n_; }
    double pcrJitterMs() const { return jitterMs_; }      // largest PCR arrival jitter seen against a straight line (ms)
private:
    void fire(int idx, double t);
    void section(int pid, const uint8_t* s, int len, double t);
    std::vector<Tr290Check> c_;
    uint64_t n_ = 0;
    int syncRun_ = 0; bool synced_ = false;
    std::map<int, int> cc_;
    std::map<int, double> lastSeen_, lastSection_;
    std::map<int, bool> ccInit_;
    std::map<int, double> pcrLast_; std::map<int, double> pcrWall_;
    std::map<int, double> ptsLast_;
    std::map<int, std::vector<uint8_t>> sec_;
    std::map<int, int> pmtPids_;                          // PMT PID -> program number
    std::map<int, bool> pcrPids_, esPids_, known_;
    double jitterMs_ = 0, lineA_ = 0, lineB_ = 0; int lineN_ = 0;
    double lastCheck_ = -1;
};

}  // namespace dect2

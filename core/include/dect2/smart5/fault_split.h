// Fault or reception? (3.9, docs/SMART5-broadcast-3.9.md item 7). Multiplexed broadcasts carry many services over one signal.
// When the signal is the problem every service suffers at once; when one service fails while the multiplex signalling (DAB FIC,
// DVB PAT/SDT, DRM FAC/SDC) and the other services decode cleanly, the fault is at the station's feed or encoder.
// Feed per interval (one second is typical): the common channel's good/bad counts and each service's good/bad units
// (audio superframes, PES packets, transport packets with the right continuity...). verdict() says which it is and since when.
#pragma once
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

enum class FaultKind { Ok, Reception, ServiceFeed, NoData };

struct ServiceFault {
    unsigned id = 0;
    FaultKind kind = FaultKind::Ok;
    double badPct = 0;          // over the window
    double since = 0;           // when the present state began
};

struct FaultSplitParams { int windowSec = 10; double badPct = 20; double cleanPct = 2; double noDataSec = 5; };

class FaultSplit {
public:
    explicit FaultSplit(FaultSplitParams p = {}) : p_(p) {}
    void common(double now, unsigned good, unsigned bad);
    void service(unsigned id, unsigned good, unsigned bad);
    void tick(double now);                 // after the counts of one interval
    std::vector<ServiceFault> verdict() const;
    FaultKind overall() const { return overall_; }
    static const char* name(FaultKind k);
    std::string text(unsigned id, const std::string& label) const;
private:
    struct Cnt { unsigned g = 0, b = 0; };
    struct Svc { std::deque<Cnt> w; Cnt cur; double lastData = -1, since = 0; FaultKind kind = FaultKind::Ok; };
    FaultSplitParams p_;
    std::deque<Cnt> common_; Cnt commonCur_;
    std::map<unsigned, Svc> svc_;
    FaultKind overall_ = FaultKind::Ok;
    double now_ = 0;
    static double pct(const std::deque<Cnt>& w);
};

} // namespace smart5
} // namespace dect2

#pragma once
#include "dect2/uat/uat_nexrad.h"
#include "uat_proto.h"
#include "uat_tel.h"
#include <deque>
#include <map>
#include <unordered_map>

namespace dect2 {

class UatTracker {
public:
    void reset();
    void setReference(double lat, double lon);
    void clearReference();
    void setExpiry(double sec) { expirySec_ = sec < 5 ? 5 : sec; }
    void apply(const UatDecoded& d, float levelDbfs, double timeSec);
    void tick(double now);
    void snapshot(UatTelemetry& out, double now);
    std::string saveResults() const;
    bool loadResults(const std::string& blob, double now);
    uint64_t good() const { return good_; }
    uint64_t bad() const { return bad_; }

private:
    struct Rec { UatAircraft ac; double last = 0; };
    std::unordered_map<uint32_t, Rec> ac_;
    std::deque<UatFisbProduct> fisb_;
    struct RadarBlock { UatNexradBlock b; double t = 0; };
    std::map<uint64_t, RadarBlock> radar_;
    std::deque<UatFrameInfo> log_;
    double expirySec_ = 60;
    bool refValid_ = false;
    double refLat_ = 0, refLon_ = 0;
    uint64_t good_ = 0, bad_ = 0;
    int goodWindow_ = 0;
    double windowT0_ = 0;
};

} // namespace dect2

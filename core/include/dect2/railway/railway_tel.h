#pragma once
#include "railway_proto.h"
#include <vector>
namespace dect2 {
struct RailwayTelemetry {
    uint64_t seq = 0, blocksOk = 0, blocksBad = 0, blocks = 0;
    int state = 0;
    double inputRate = 0, centerHz = 0, timeSec = 0, cfoHz = 0;
    float snrDb = 0, levelDb = -200;
    bool dataValid = false;
    std::vector<railway::Message> messages;
};
inline std::string railwaySummary(const RailwayTelemetry& t, const char* mode) {
    if (t.state == 0) return std::string(mode) + ": waiting for samples";
    if (t.messages.empty()) return std::string(mode) + ": listening";
    const auto& m = t.messages.front();
    return std::string(mode) + " " + (m.protocol == railway::Protocol::Tdcs ? m.train : m.action) + " " + m.locomotive;
}
} // namespace dect2

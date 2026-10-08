// Helpers for the AIS tests and the tool: random messages, running a receiver over a signal, and collecting what it decoded from its !AIVDM sentences.
#pragma once
#include "ais_gen.h"
#include "ais_proto.h"
#include "ais_rx.h"
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace dect2 {
namespace aistest {

// a class A position report (type 1) with random fields
inline ais::Bits randomPosition(std::mt19937& rng, uint32_t mmsi = 0) {
    using namespace ais;
    Bits b;
    putU(b, 0, 6, 1);
    putU(b, 8, 30, mmsi ? mmsi : 200000000u + rng() % 500000000u);
    putU(b, 38, 4, rng() % 9);
    putS(b, 42, 8, (int)(rng() % 200) - 100);
    putU(b, 50, 10, rng() % 300);
    putU(b, 60, 1, rng() & 1);
    putS(b, 61, 28, (int)(rng() % 20000000) + 30000000);
    putS(b, 89, 27, (int)(rng() % 5000000) + 12000000);
    putU(b, 116, 12, rng() % 3600);
    putU(b, 128, 9, rng() % 360);
    putU(b, 137, 6, rng() % 60);
    putU(b, 149, 19, rng() & 0x7FFFF);
    b.resize(168, 0);
    return b;
}

// Splits what the receiver published into whole messages (payload bits and channel) and keeps only the ones that are new since last time.
class Collector {
public:
    struct Got { ais::Bits bits; char channel; };
    // call after every telemetry report
    void update(const AisTelemetry& t) {
        std::vector<Got> all;
        std::map<std::string, std::string> part;      // sequence id -> payload characters so far
        for (const std::string& s : t.nmea) {
            std::string chars; int fill = 0, cnt = 0, no = 0; char ch = 'A';
            if (!ais::parseNmea(s, chars, fill, cnt, no, &ch)) continue;
            if (cnt == 1) { all.push_back({ais::unarmour(chars, fill), ch}); continue; }
            const size_t c1 = s.find(',', 7), c2 = c1 == std::string::npos ? c1 : s.find(',', c1 + 1);
            const std::string id = c2 == std::string::npos ? "" : s.substr(c2 + 1, 1) + ch;
            if (no == 1) part[id] = chars; else part[id] += chars;
            if (no == cnt) all.push_back({ais::unarmour(part[id], fill), ch});
        }
        const uint64_t fresh = t.blocksOk - lastOk_;
        lastOk_ = t.blocksOk;
        const size_t take = (size_t)std::min<uint64_t>(fresh, all.size());
        for (size_t i = all.size() - take; i < all.size(); i++) got.push_back(all[i]);
    }
    std::vector<Got> got;
private:
    uint64_t lastOk_ = 0;
};

// feeds 'x' in chunks and returns the last report; 'col' (optional) collects the decoded messages
inline AisTelemetry runReceiver(AisReceiver& rx, const std::vector<cf32>& x, size_t chunk = 16384, Collector* col = nullptr) {
    AisTelemetry t, last;
    uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += chunk) {
        rx.feed(x.data() + i, std::min(chunk, x.size() - i));
        while (rx.telemetry(t, seq)) { seq = t.seq; last = t; if (col) col->update(t); }
    }
    return last;
}

} // namespace aistest
} // namespace dect2

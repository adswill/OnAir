// Telemetry shared by the aviation, rail and road receivers of 3.8 (families 400 - 412, docs/AVIA-ROAD-3.8.md) and their screens.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct AvRoadRow {
    double timeSec = 0;
    std::string kind, id, text;      // what it is (English, shown through tr()), who sent it, the decoded content
    bool hasPos = false;
    double lat = 0, lon = 0;
    float levelDb = 0;
};

struct AvRoadTelemetry {
    // ---- the part every mode has: the engine copies it into RxTelemetry; keep these members and what they mean
    uint64_t seq = 0;
    int state = 0;                   // 0 no signal, 1 energy but nothing decoded lately, 2 decoding
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;

    // ---- this family's own fields
    int variant = -1;                // avroad::Variant
    double timeSec = 0, inputRate = 0;
    float levelDb = -120;
    std::string headline;            // one line on what is being heard
    std::vector<std::pair<std::string, std::string>> facts;   // label (English, shown through tr()), value
    std::vector<AvRoadRow> rows;     // newest first
};

} // namespace dect2

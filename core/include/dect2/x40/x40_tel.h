// Telemetry of the 4.0 signal modes (family block 881 - 896, docs/X40-4.0.md) and their screens.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct X40Row {
    double timeSec = 0;
    std::string kind, id, text;      // what it is (English, shown through tr()), a short value, the readout
    float levelDb = 0;
};

struct X40Telemetry {
    // ---- the part every mode has: the engine copies it into RxTelemetry; keep these members and what they mean
    uint64_t seq = 0;
    int state = 0;                   // 0 no signal, 1 energy but nothing decoded lately, 2 decoding
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;

    // ---- this family's own fields
    int variant = -1;                // x40::Variant
    double timeSec = 0, inputRate = 0;
    float levelDb = -120;
    std::string headline;            // one line on what is being heard
    std::vector<std::pair<std::string, std::string>> facts;   // label (English, shown through tr()), value
    std::vector<X40Row> rows;        // newest first
};

} // namespace dect2

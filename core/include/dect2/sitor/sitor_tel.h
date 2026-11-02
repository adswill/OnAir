// SITOR telemetry. RxTelemetry::sitor, family 119, active standard 120, engine standard 121.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2 {

struct SitorTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no FSK found, 1 FSK found, 2 locked on a SITOR-B (FEC) stream
    double cfoHz = 0;              // offset of the FSK centre from the dial
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0;         // code words with four marks while locked
    uint64_t blocksBad = 0;        // code words that were not valid
    double shiftHz = 0;            // measured tone spacing
    double baud = 0;               // measured symbol rate
    bool inverted = false;         // the stream was found with the tones swapped
    uint64_t chars = 0;
    std::string text;              // the last part of the text, newest at the end
};

inline std::string sitorSummary(const SitorTelemetry& t) {
    char b[160];
    if (t.state == 0) return "SITOR: no FSK signal";
    if (t.state == 1) { snprintf(b, sizeof b, "SITOR: FSK %.0f Hz shift, not locked", t.shiftHz); return b; }
    snprintf(b, sizeof b, "SITOR-B locked, %llu characters", static_cast<unsigned long long>(t.chars));
    return b;
}

} // namespace dect2

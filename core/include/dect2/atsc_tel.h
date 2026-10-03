// Telemetry of the ATSC receiver (plain data, shared with RxTelemetry).
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {

struct AtscTelemetry {
    bool valid = false;           // the ATSC receiver has produced anything
    bool pilot = false;           // carrier (pilot) locked
    bool segSync = false;         // segment sync found, symbol clock locked
    bool fieldSync = false;       // field sync found (frame timing)
    bool eqTrained = false;       // equaliser solved for the current field
    bool tsOk = false;            // transport stream flowing
    int fieldParity = 0;          // 1 or 2 (which field sync polarity was seen last)
    double cfoHz = 0;             // carrier offset
    double sroPpm = 0;            // symbol clock offset
    double snrDb = 0;             // after equalisation, from the known field sync / segment sync symbols
    double dataSnrDb = 0;         // from the distance of the data symbols to the nearest of the 8 levels
    double pilotDb = 0;           // pilot level over the data level
    double syncQuality = 0;       // 0..1 correlation of the segment sync
    uint64_t fields = 0, segments = 0, rsClean = 0, rsCorrected = 0, rsFailed = 0, fieldSyncMisses = 0, lockLosses = 0;
    double segErrorRate = 0;      // recent share of uncorrectable segments (0..1)
    std::vector<float> eqTaps;    // equaliser impulse response (feed-forward), cursor at `eqCursor`
    int eqCursor = 0;
    std::vector<float> levels;    // a sample of equalised data symbols (nominal -7..+7), for the eye/histogram view
    uint64_t seq = 0;
};

} // namespace dect2

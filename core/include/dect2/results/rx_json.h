// The receiver's telemetry as one JSON object (the remote-control API, scripts). Every mode gets the common block (standard, state, lock, the
// SNR the mode reports, carrier offset, FEC block counts); the modes with a block of their own (DVB-T2, DVB-T, ATSC, DAB, FM, DVB-S/S2, DTMB,
// DVB-C) add their main figures under "details". LBJ, KLW and TDCS also export their bounded message lists there.
// The other modes carry only the common block: their lists and tables are not exported here.
#pragma once
#include "dect2/engine/quality.h"
#include "dect2/t2/t2rx.h"
#include <string>

namespace dect2 {

// q: the quality meter's report for the same telemetry (optional)
std::string rxTelemetryJson(const RxTelemetry& rx, const QualityReport* q = nullptr);

} // namespace dect2

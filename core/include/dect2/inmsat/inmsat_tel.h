// Inmarsat all-carriers telemetry, shared by the receiver and the UI. The first members (the Snapshot's common prefix) are copied into RxTelemetry.
#pragma once
#include "inmsat_manager.h"
#include "inmsat_voice.h"

namespace dect2 {

struct InmsatTelemetry : inmsat::Snapshot {
    bool codecReady = false;
    std::string voiceMessage;                        // what to show on the Voice tab ("" when the codec works)
    int activeCalls = 0;
    std::vector<inmsat::VoiceChannelInfo> voice;
    std::vector<inmsat::CallRecord> callHistory;      // finished calls, newest last
    double inputRate = 0;
};

inline std::string inmsatSummary(const InmsatTelemetry& t) {
    if (t.state == 0) return "Inmarsat: searching";
    int dec = 0;
    for (const auto& c : t.carriers) dec += c.state >= 2;
    char b[160];
    std::snprintf(b, sizeof b, "Inmarsat: %zu carriers, %d decoding, %llu messages, %zu aircraft", t.carriers.size(), dec, (unsigned long long)t.messagesTotal, t.aircraft.size());
    return b;
}

} // namespace dect2

// Motorola Type II (SmartNet, SmartZone) telemetry. RxTelemetry::smartnet, engine standard 322, active standard 321 (family 320). Receive only.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct SmartnetRow {
    int64_t wallTime = 0;
    double timeSec = 0;
    int addr = 0, cmd = 0;
    bool group = false;
    std::string text;
};

struct SmartnetCall {
    uint64_t id = 0;
    int tg = -1, src = -1;
    double freqHz = 0;
    bool group = true, digital = false, emergency = false, encrypted = false, followed = false, active = false, missed = false, listening = false;
    int64_t wallStart = 0;
    double durationSec = 0, audioSec = 0;
    std::string state, note;
};

struct SmartnetTalkgroup {
    int tg = 0;
    uint32_t calls = 0;
    int lastSrc = -1;
    double lastFreqHz = 0;
    int64_t lastWall = 0;
    bool active = false, digital = false, encrypted = false, patched = false;
};

struct SmartnetSiteRow {
    int system = -1, site = -1, band = -1, features = -1;
    double controlHz = 0;
    bool adjacent = true;
};

struct SmartnetPatch { int tg = 0, member = 0; double lastSec = 0; };

struct SmartnetTelemetry {
    uint64_t seq = 0;
    int state = 0;                    // 0 no input, 1 listening, 2 a good word in the last 5 s
    bool locked = false, dataValid = false, inverted = false;
    float snrDb = 0, levelDb = -200, deviationHz = 0;
    uint64_t oswOk = 0, oswBad = 0, oswFixed = 0, messages = 0, messagesKnown = 0;
    double timeSec = 0, inputRate = 0, centerHz = 0;
    int systemId = -1, siteId = -1, features = -1;
    double controlHz = 0;
    bool typeIIKnown = false, typeII = true;
    bool smartZone = false;           // the site says it is wide area, or sites are listed
    std::string band;                 // the plan in use
    bool bandAuto = true;
    std::vector<SmartnetRow> rows;    // newest first
    std::vector<SmartnetCall> calls;  // active first, then the newest
    std::vector<SmartnetTalkgroup> talkgroups;
    std::vector<SmartnetSiteRow> sites;
    std::vector<SmartnetPatch> patches;
    bool follow = true, followPrivate = false, audioOpen = false;
    int listenTg = -1;
    uint64_t grants = 0, followed = 0, missedOutside = 0, missedBusy = 0, encrypted = 0;
    std::string poolStatus, note;
};

constexpr size_t kSmartnetRowsMax = 400, kSmartnetCallsMax = 200, kSmartnetTgMax = 200;

inline std::string smartnetSummary(const SmartnetTelemetry& t) {
    char b[160];
    if (t.state == 0) return "Type II: waiting for samples";
    if (!t.dataValid) { snprintf(b, sizeof b, "Type II: listening (%.0f dBFS)", t.levelDb); return b; }
    int active = 0;
    for (const auto& c : t.calls) active += c.active;
    if (active) snprintf(b, sizeof b, "%s: %s, %d call%s on air", t.smartZone ? "SmartZone" : "SmartNet", t.locked ? "locked" : "no lock", active, active == 1 ? "" : "s");
    else snprintf(b, sizeof b, "%s: %s, %llu good words", t.smartZone ? "SmartZone" : "SmartNet", t.locked ? "locked" : "no lock", (unsigned long long)t.oswOk);
    return b;
}

} // namespace dect2

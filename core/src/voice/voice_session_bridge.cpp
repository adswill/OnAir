#include "dect2/session/voice_bridge.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::sess::voicebridge {

namespace {
struct ModeRow { const char* monitor; int number; const char* name; };
const ModeRow kModes[] = {
    {"dmr", 9, "DMR"}, {"nxdn", 38, "NXDN"}, {"p25", 50, "P25"}, {"dpmr", 45, "dPMR"},
    {"dstar", 33, "D-STAR"}, {"ysf", 128, "Yaesu Fusion"}, {"tetra", 46, "TETRA"}, {"m17", 31, "M17"},
};
}  // namespace

int modeNumber(const std::string& monitorMode) {
    for (const ModeRow& m : kModes) if (monitorMode == m.monitor) return m.number;
    return -1;
}

const char* modeName(const std::string& monitorMode) {
    for (const ModeRow& m : kModes) if (monitorMode == m.monitor) return m.name;
    return "";
}

Session toSession(const voice::CallEntry& e, const std::string& monitorMode, uint16_t receiver, bool ended) {
    const voice::CallInfo& c = e.info;
    Session s;
    s.id = e.sessionId;
    s.mode = (uint16_t)std::max(0, modeNumber(monitorMode));
    s.receiver = receiver;
    s.kind = (uint8_t)Kind::Call;
    const std::string target = voice::targetText(c), source = voice::sourceText(c);
    s.key = target.empty() ? std::string("?") : target;
    if (c.slot > 0) s.key += "/slot" + std::to_string(c.slot);
    s.who[0] = source;
    s.who[1] = target;
    s.alias[0] = c.alias;
    s.alias[1] = e.label;
    s.startWall = e.wallStart * 1000;
    s.endWall = ended ? std::max<int64_t>(e.wallEnd, e.wallStart) * 1000 : 0;
    if (ended && s.endWall <= s.startWall) s.endWall = s.startWall + 1;   // 0 means active
    const double secs = e.seconds();
    s.durationMs = (uint32_t)std::min(std::max(0.0, secs) * 1000.0, 4.0e9);
    s.errRate = e.errorRate;
    s.freqHz = c.carrierHz > 0 ? (uint64_t)c.carrierHz : 0;
    s.slot = c.slot > 0 ? (int16_t)c.slot : (int16_t)-1;
    s.eventCount = (uint32_t)std::min<uint64_t>(e.frames, 0xFFFFFFFFu);
    if (c.encrypted) s.verdict = (uint8_t)Verdict::Encrypted;
    else if (c.codecMissing || e.verdict == voice::Verdict::NoCodec) s.verdict = (uint8_t)Verdict::Unreadable;   // heard and timed, not decodable here
    else s.verdict = (uint8_t)Verdict::Clear;
    if (c.emergency) s.flags |= FlagEmergency;
    if (e.priority) s.flags |= FlagPriority;
    if (!e.file.empty()) s.flags |= FlagRecording;
    std::string title = modeName(monitorMode);
    if (title.empty()) title = monitorMode;
    title += " " + (target.empty() ? std::string("call") : target);
    if (!e.label.empty() && e.label != target) title += " " + e.label;
    if (ended) {
        char b[32];
        snprintf(b, sizeof b, ", %.0f s", std::max(0.0, secs));
        title += b;
    }
    s.title = title;
    return s;
}

}  // namespace dect2::sess::voicebridge

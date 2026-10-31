// APRS-IS iGate rules (receive-only gating of what the packet decoder hears to the APRS Internet System).
// The Feeds thread logs in to the server the user names with the user's callsign and passcode and sends each gated packet as
//   FROM>TO,PATH,qAR,MYCALL:info
// Nothing the server sends is ever turned into radio traffic: the iGate only receives from the air and only uploads. It is off until the user turns it on,
// the passcode is entered by the user, kept only in the settings (never in the log, the status text or the files OnAir writes) and not in the backups.
// AprsGate decides, for one received frame, whether it is gated; it also keeps the rate limits and counts why packets were not gated.
// Rules (after the iGate guidance of aprs-is.net/IGating.aspx): only UI frames with PID F0; not empty; not third-party or query; no NOGATE / RFONLY /
// TCPIP / TCPXX / q-construct in the path; not the iGate's own packets and not packets that already went through it (own callsign in the path); a source
// that is a valid callsign (letters and digits, 3 - 6 characters with a digit, optional -SSID 0 - 15); a line within 510 bytes; the same
// source / destination / information is sent once per dedup window (30 s); and the rate limits below.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dect2 {

struct AprsGateConfig {
    int maxPerMin = 60;            // packets uploaded per minute in all (0 = no limit)
    int perStationPerMin = 10;     // per source callsign (0 = no limit)
    int dedupSec = 30;             // the same packet again within this time is dropped (0 = off)
};

enum class AprsGateVerdict { Gate, NoCall, BadCall, NotUi, Empty, ThirdParty, Query, NoGatePath, FromInternet, OwnPacket, BadSource, TooLong, BadChars, Duplicate, RateLimited, StationLimited, kCount };
const char* aprsGateVerdictText(AprsGateVerdict v);

struct AprsGateStats {
    uint64_t gated = 0, duplicates = 0, rateLimited = 0, stationLimited = 0, notGateable = 0;   // notGateable: every rule above except the dedup and the limits
    uint64_t byVerdict[(int)AprsGateVerdict::kCount] = {};
};

// "LY1ABC", "LY1ABC-9", "N0CALL-15": letters and digits, 3 - 6 characters, at least one digit and one letter, optional -SSID of 1 or 2 digits, 0 - 15
bool aprsValidCallsign(const std::string& call);
// The login line with the passcode hidden, for logs and diagnostics: "user LY1ABC pass ****"
std::string aprsIsLoginRedacted(const std::string& call);

class AprsGate {
public:
    void configure(const AprsGateConfig& c);
    // now: seconds on any clock that only goes forward. On Gate, *line holds the line for the server (with CRLF).
    AprsGateVerdict decide(const std::string& from, const std::string& to, const std::string& path, const std::string& info, const std::string& myCall,
                           bool uiF0, double now, std::string* line);
    AprsGateStats stats() const;
    void reset();                  // forgets what was sent (a new connection); the counters stay
    std::string summary() const;   // "gated 12, duplicates 3, rate-limited 0, not gateable 40"
private:
    mutable std::mutex mu_;
    AprsGateConfig cfg_;
    AprsGateStats st_;
    std::unordered_map<std::string, double> seen_;                 // dedup key -> time sent
    std::deque<double> all_;                                       // upload times in the last minute
    std::map<std::string, std::deque<double>> perStation_;
    double lastPrune_ = 0;
};

} // namespace dect2

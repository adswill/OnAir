// P25 Phase 1 receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One entry of the call log: a voice call heard on this channel, a voice grant seen on a control channel, or a data packet
struct P25Call {
    int carrierId = 0; double carrierHz = 0;   // whole-span decoding: the carrier it came from (id 0: single channel), its frequency relative to the span centre
    int64_t wallTime = 0;        // Unix time of its start
    double startSec = 0, durSec = 0;
    bool active = false;
    int kind = 0;                // 0 voice, 1 voice grant (control channel), 2 data packet
    int src = -1, dst = -1;      // unit ID; talkgroup (group) or unit (unit to unit); -1 not known yet
    bool group = true;
    bool emergency = false;
    bool encrypted = false;      // the call is encrypted (reported; decrypted only when the user loaded a matching key, see `decrypt`)
    int algId = 0x80, keyId = 0;
    std::string decrypt;         // status text: ""/clear, "encrypted: add your key", "decrypted with key <name>", "key mismatch"
    int nac = -1;
    int channel = -1;            // grants: the channel number (identifier and channel)
    double freqHz = 0;           // grants: its frequency, when the identifier update was heard
    bool lateEntry = false;      // voice: joined without the header
    bool endSeen = false;        // voice: ended with a terminator (else the signal was lost)
    uint32_t voiceFrames = 0;    // IMBE frames received
    uint32_t clearFrames = 0;    // of which handed to the codec (never for an encrypted call)
    bool voice = false;          // IMBE decoded to sound
    std::string text;
    float berPct = 0;
};

struct P25Site {
    bool valid = false;
    int wacn = -1, sysId = -1, rfss = -1, site = -1, nac = -1;
    int channel = -1;            // its control channel
    double freqHz = 0;
    int ssc = 0;                 // system service class
    int64_t wallTime = 0;
};

struct P25IdenRow {
    bool valid = false, vu = false;
    bool tdma = false;           // IDEN_UP_TDMA (0x33): the channel number counts time slots, `slots` per carrier
    int slots = 1;
    double baseHz = 0, spacingHz = 0, txOffsetHz = 0, bwHz = 0;
};

struct P25Talkgroup {
    int tg = 0;
    uint32_t grants = 0, calls = 0;
    int lastSrc = -1;
    int64_t lastWall = 0;
    double lastFreqHz = 0;
    bool encrypted = false, emergency = false, active = false;
};

// A control channel message that decoded (the trunking follower's input). seq is process-wide and grows with every TSBK of every receiver
struct P25TsbkRec {
    uint64_t seq = 0;
    int nac = -1, opcode = 0, mfid = 0;
    uint64_t args = 0;
    double sigSec = 0;           // the receiver's signal time
};

struct P25Telemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 frames seen recently, 2 a call on air
    double cfoHz = 0;            // carrier offset measured on the frame sync
    float snrDb = 0;             // symbol SNR at the decision points
    float levelDb = -200;        // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;       // NIDs, TSBKs and link controls that decoded
    uint64_t blocksBad = 0;      // and that did not
    // details
    double timeSec = 0, inputRate = 0;
    float deviationHz = 0;       // the +3 symbol's deviation as measured
    bool inverted = false;
    int nac = -1;
    bool control = false;        // a control channel (TSBKs) is heard
    uint64_t frames = 0, nidBad = 0, hdu = 0, ldu1 = 0, ldu2 = 0, tdu = 0, tdulc = 0, pdu = 0;
    uint64_t tsbkOk = 0, tsbkBad = 0, lcOk = 0, lcBad = 0, esOk = 0, esBad = 0, hduOk = 0, hduBad = 0;
    uint64_t voiceFrames = 0, voiceDecoded = 0, lateEntries = 0;
    float berPct = 0;            // NID bit errors before the BCH
    bool imbe = false;           // mbelib found
    std::string imbePath;
    P25Site site;                // the system this control channel belongs to
    std::vector<P25Site> adjacent;
    P25IdenRow idens[16];
    std::vector<P25Talkgroup> talkgroups;   // most recently active first
    P25Call now;
    std::vector<P25Call> log;    // newest first, at most kP25LogMax
    std::vector<std::string> events;   // newest first: decoded control messages as text
    std::vector<P25TsbkRec> tsbks;     // the last decoded TSBKs, oldest first, at most kP25TsbkMax
};

constexpr size_t kP25LogMax = 300;
constexpr size_t kP25TgMax = 200;
constexpr size_t kP25TsbkMax = 512;   // about 6 s of a busy control channel (40-80 TSBK/s): a stalled screen must not lose grants (the follower counts a gap if it still does)

inline std::string p25Id(int v) { return v < 0 ? std::string("?") : std::to_string(v); }

inline std::string p25Summary(const P25Telemetry& t) {
    char b[200];
    const P25Call& n = t.now;
    if (n.active) {
        snprintf(b, sizeof b, "%s -> %s%s%s, %.1f s", p25Id(n.src).c_str(), n.group ? "TG " : "", p25Id(n.dst).c_str(), n.encrypted ? " (encrypted)" : "", n.durSec);
        return b;
    }
    if (t.frames == 0) return "searching for P25";
    if (t.control && t.site.valid) {
        snprintf(b, sizeof b, "control channel, system %03X site %d-%d, %zu talkgroups", t.site.sysId, t.site.rfss, t.site.site, t.talkgroups.size());
        return b;
    }
    snprintf(b, sizeof b, "NAC %03X, %zu calls heard", t.nac < 0 ? 0 : t.nac, t.log.size());
    return b;
}

} // namespace dect2

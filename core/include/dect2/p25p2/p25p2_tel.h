// P25 Phase 2 receiver telemetry, shared by the receiver and the UI. RxTelemetry::p25p2, family 180, engine standard 182, active standard 181.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "dect2/p25/p25_tel.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One call on one of the two logical channels (time slots), or a grant seen
struct P25p2Call {
    int slot = 0;                // logical channel 1 or 2 (0: a grant, not on this channel)
    int64_t wallTime = 0;
    double startSec = 0, durSec = 0;
    bool active = false;
    int kind = 0;                // 0 voice, 1 voice channel grant (control message)
    int src = -1, dst = -1;      // unit ID; talkgroup (group) or unit (unit to unit); -1 not known yet
    bool group = true;
    bool emergency = false;
    bool encrypted = false;      // reported only: nothing is decrypted, the voice of such a call never reaches a codec
    bool cryptoKnown = false;    // false: joined late and no message has said yet (voice is held back until it does)
    int algId = 0x80;
    int keyId = 0;               // from the encryption sync (ESS) of the voice bursts
    int channel = -1;            // grants: the channel number
    double freqHz = 0;           // grants: its frequency, when the identifier update was heard
    bool lateEntry = false;
    bool endSeen = false;        // ended with an END_PTT (else the signal was lost)
    uint32_t voiceFrames = 0, clearFrames = 0;
    bool voice = false;          // AMBE+2 decoded to sound
    std::string text;
};

struct P25p2Telemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 locked on the sync, 2 a call on air
    double cfoHz = 0;            // carrier offset measured on the sync
    float snrDb = 0;             // symbol SNR measured on the sync
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0;       // MAC PDUs that decoded
    uint64_t blocksBad = 0;
    // details
    double timeSec = 0, inputRate = 0;
    bool locked = false;
    int slotNow = -1;            // slot of the superframe (0..11) being received
    uint64_t syncs = 0, slots = 0, slotsBad = 0, ischBad = 0, unlocks = 0;
    uint64_t v4 = 0, v2 = 0, sacch = 0, facch = 0, rsCorrected = 0;
    uint64_t voiceFrames = 0, voiceDecoded = 0, encryptedSkipped = 0;
    uint64_t ischExact = 0, ischCorrected = 0, duidBad = 0, essOk = 0, essBad = 0, realigns = 0;
    int wacn = 0, sysId = 0, colorCode = 0;   // the descrambler seed in use (all 0: not descrambled)
    uint64_t followTag = 0;                   // P25p2Receiver::startFollow: the reports since that reset
    double voiceSec[2] = {-10, -10};          // signal time of the last voice burst on slot 1 / slot 2
    bool codec = false;          // an AMBE+2 decoder (plug-in "ambe2" or mbelib) is available
    std::string codecPath;
    int listenSlot = 0, audioSlot = 0;   // the choice (0 first heard, 1, 2) and the slot actually playing
    int nac = -1;                // colour code of the last END_PTT
    P25Site site;
    std::vector<P25Site> adjacent;
    P25IdenRow idens[16];
    std::vector<P25Talkgroup> talkgroups;   // most recently active first
    P25p2Call now[2];            // slot 1 and slot 2
    std::vector<P25p2Call> log;  // newest first
    std::vector<std::string> events;
};

constexpr size_t kP25p2LogMax = 300;

inline std::string p25p2Summary(const P25p2Telemetry& t) {
    char b[200];
    for (int i = 0; i < 2; i++) {
        const P25p2Call& n = t.now[i];
        if (n.active) {
            snprintf(b, sizeof b, "slot %d: %s -> %s%s%s", i + 1, p25Id(n.src).c_str(), n.group ? "TG " : "", p25Id(n.dst).c_str(), n.encrypted ? " (encrypted)" : "");
            return b;
        }
    }
    if (!t.locked) return "searching for P25 Phase 2";
    if (t.site.valid) {
        snprintf(b, sizeof b, "locked, system %03X site %d-%d, %zu talkgroups", t.site.sysId, t.site.rfss, t.site.site, t.talkgroups.size());
        return b;
    }
    snprintf(b, sizeof b, "locked, %zu calls heard", t.log.size());
    return b;
}

} // namespace dect2

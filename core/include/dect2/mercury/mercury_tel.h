// Mercury receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kMercuryFramesMax = 400;
constexpr size_t kMercuryStationsMax = 100;
constexpr size_t kMercurySessionsMax = 40;
constexpr size_t kMercuryFilesMax = 40;
constexpr size_t kMercuryTextMax = 6000;
constexpr int kMercuryModes = 12;      // every mode (mercury_phy.h OfdmModeId: the ten OFDM modes, MFSK, MFSK500)

struct MercuryFrameLog {
    int64_t utc = 0;             // seconds
    double timeSec = 0;          // signal seconds (the burst's start)
    double airSec = 0;           // the burst's length
    int mode = -1;               // OfdmModeId
    std::string modeName;        // "DATAC16"
    std::string kind;            // "CALL", "ACCEPT", "CQ", "DISCONNECT", "POLL", "REQ", "STATUS", "DATA", "MESSAGE", "FILE", "FRAME", "burst"
    int sessionId = -1;          // ARQ session id (7 bits) or broadcast file session (1 .. 31)
    int sessionKey = -1;         // index of the session in MercuryTelemetry::sessions (-1 none)
    int dir = -1;                // in a session: 0 sent by the caller, 1 by the callee, -1 not known
    std::string from, to;        // callsigns as the protocol carries them ("CRC 1A2B" when only the CRC is on the air)
    std::string detail;
    float snrDb = 0;             // estimate in 3 kHz
    double cfoHz = 0;
    bool fecOk = false;          // LDPC converged
    int ldpcIters = 0;
    int rawErrors = -1;          // channel bit errors the LDPC corrected
    bool crcOk = false;          // the CRC matched (plain, or with the session's seed)
    bool seeded = false;         // the CRC carried a session seed
    uint16_t seed = 0;
    bool encrypted = false;      // payload of an encrypted session: never decoded
    int payloadLen = 0;          // bytes of the frame (without the CRC)
    std::string hex;             // the first bytes of the frame
};

struct MercuryDirection {        // one direction of a session: who sends its data
    uint64_t dataFrames = 0, ctlFrames = 0;
    uint64_t blocksOk = 0, blocksFailed = 0;
    uint64_t bytes = 0;          // application bytes reassembled
    int lastRung = -1;           // the rung its last data frame came on
    int askedRung = -1, askedFrames = 0;   // what the other side's last poll asked it for
    float reportedSnrDb = -1000; // how the other side says it hears this one
    double cfoHz = 0;            // this sender's frequency offset as heard here
    bool cfoKnown = false;
    std::string text;            // the reassembled bytes, printable (the newest kMercuryTextMax)
};

struct MercurySession {
    int key = 0;                 // stable index
    int id = -1;                 // session id (7 bits), -1 when only its seed was heard
    std::string caller, callee;  // the callee as "CRC 1A2B" until a heard callsign matches the CRC
    uint16_t calledCrc = 0, nonce = 0, seed = 0;
    int bwHz = 0, startLevel = -1;
    bool ctlDeaf = false;
    bool seedKnown = false;      // the seed: computed from the connect, or learned from the frames' CRC
    bool seedFromFrames = false;
    bool encrypted = false;      // the connect said so (a reserved bit of the ACCEPT, or a later revision): payload never shown
    std::string state;           // "calling", "connected", "disconnected", "heard (connect not heard)"
    int64_t firstUtc = 0, lastUtc = 0;
    uint64_t frames = 0;
    MercuryDirection d[2];       // [0] the caller's data to the callee, [1] the callee's to the caller
};

struct MercuryStation {
    std::string call;
    uint64_t count = 0;
    int64_t firstSeen = 0, lastSeen = 0;
    std::string last;            // the last frame kind heard from it
    std::string heardTo;         // the station it called last
    int bwHz = 0;
};

struct MercuryFile {
    int id = 0;                  // stable index (the receiver's fileBytes())
    int session = 0;             // broadcast session id (1 .. 31)
    std::string name;            // the bundle's name, or broadcast_YYYYMMDD_HHMMSS.bin
    bool bundle = false;
    uint32_t size = 0;           // object bytes announced
    uint64_t symbols = 0;        // symbols received
    uint64_t needed = 0;         // source symbols of the object
    bool done = false;
    std::string kind;            // "text", "image", "binary"
    std::string preview;         // the first characters of a text file
    std::string mode;            // the mode it came on
    int64_t firstUtc = 0, doneUtc = 0;
};

struct MercuryTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing, 1 a burst is being received, 2 frames decoded lately
    double cfoHz = 0;            // of the last burst
    float snrDb = 0;             // of the last burst
    float levelDb = -200;        // input power, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;       // frames decoded (LDPC and CRC)
    uint64_t blocksBad = 0;      // bursts acquired that did not decode
    // details
    double timeSec = 0, inputRate = 0;
    uint64_t bursts = 0, plainFrames = 0, seededFrames = 0, fecFailed = 0, broadcastFrames = 0, encryptedFrames = 0;
    uint64_t perMode[kMercuryModes] = {};
    int lastMode = -1;
    std::vector<float> constellation;    // re, im pairs of the last burst's equalised symbols
    std::vector<MercuryFrameLog> frames; // newest first
    std::vector<MercurySession> sessions;// most recent first
    std::vector<MercuryStation> stations;
    std::vector<MercuryFile> files;
    std::string messages;                // broadcast messages, one per line, the newest kMercuryTextMax characters
    std::vector<float> levelHist;        // input level per second, dB
};

inline std::string mercurySummary(const MercuryTelemetry& t) {
    char b[160];
    if (t.blocksOk) {
        snprintf(b, sizeof b, "Mercury: %llu frames, %zu sessions, %zu files", (unsigned long long)t.blocksOk, t.sessions.size(), t.files.size());
        return b;
    }
    if (t.bursts) return "Mercury: bursts heard, none decoded yet";
    return "Mercury: no signal";
}

} // namespace dect2

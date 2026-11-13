// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT receiver telemetry. RxTelemetry::kaft, engine standard 127, active standard 126 (family 125).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// A file that was received whole and passed the hash check of the session.
struct KaftFile {
    std::string name;              // as announced by the sender (cleaned: no path)
    std::string path;              // where it was saved; empty when no folder was chosen (then the file is not kept)
    uint64_t bytes = 0;
    double atSec = 0;              // stream time when it completed
    bool saved = false;
    std::string note;              // "saved", "no folder chosen: not saved", or the reason it could not be written
};

struct KaftTelemetry {
    uint64_t seq = 0;
    // 0 no input, 1 searching, 2 receiving (a frame header passed its CRC within the last 5 s)
    int state = 0;
    double inputRate = 0, timeSec = 0;
    bool dataValid = false;        // blocks have passed their CRC (the engine's "data valid" flag)
    // the last frame heard
    int width = -1;                // 0 1 MHz, 1 500 kHz, 2 200 kHz, -1 none yet
    int profile = -1;              // profile id 0 .. 9 of the last data frame
    float esN0Db = 0;
    float snrDb = 0;               // the same value (the engine's common field)
    float cfoHz = 0;
    std::string callsign;
    double lastFrameAgeSec = 0;
    // counts
    uint64_t framesData = 0, framesCtrl = 0, framesAck = 0;
    uint64_t framesBad = 0;        // data frames in which at least one block failed
    uint64_t blocksOk = 0, blocksBad = 0;
    double throughputBps = 0;      // file data bits per second of the blocks that passed, averaged over the last 10 s
    uint64_t profileFrames[10] = {};   // data frames per profile
    // the session being received (one at a time, from the OPEN message)
    // 0 none, 1 announced (OPEN heard, no data yet), 2 receiving, 3 complete and verified, 4 hash check failed or the sender gave up
    int session = 0;
    uint16_t sessionId = 0;
    std::string peer;              // the sender's callsign from OPEN
    std::string fileName;
    uint64_t fileBytes = 0;        // size of the data that is sent (after compression, which is not built in: equal to the file)
    uint64_t haveBytes = 0;        // bytes of it received so far
    uint64_t resumedBytes = 0;     // bytes that were already held when this session was announced (a resumed transfer)
    int heldSessions = 0;          // other unfinished transfers kept for a resume
    // The profile ladder: the profile the frames come at, and the profile the receiver's stepper would ask the sender for. There is no return link
    // in this build, so the request is shown, never sent.
    int requestedProfile = -1;
    std::vector<KaftFile> files;   // newest first, up to 32
    std::string saveFolder;        // the folder files are saved to (empty: none chosen)
};

inline std::string kaftSummary(const KaftTelemetry& t) {
    char b[240];
    if (t.state == 0) return "KAFT: waiting for samples";
    if (t.state == 1) return "KAFT: searching";
    if (t.session == 2 && t.fileBytes) {
        snprintf(b, sizeof b, "KAFT: %s %.0f%%", t.fileName.c_str(), 100.0 * (double)t.haveBytes / (double)t.fileBytes);
        return b;
    }
    snprintf(b, sizeof b, "KAFT: receiving, Es/N0 %.0f dB", (double)t.esN0Db);
    return b;
}

} // namespace dect2

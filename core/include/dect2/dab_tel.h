// DAB / DAB+ data structures shared by the receiver and the UI: ensemble contents and live telemetry.
#pragma once
#include "ring.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct DabSubchannel {
    int id = 0;
    int start = 0;        // first capacity unit (CU) inside the CIF
    int size = 0;         // number of CUs
    bool eep = true;      // equal error protection (false: unequal, short form)
    int option = 0;       // EEP: 0 = A profiles, 1 = B profiles
    int level = 0;        // EEP: 0..3 = protection level 1..4; UEP: protection level 1..5 (stored as 0..4)
    int uepIndex = 0;     // UEP table index (short form)
    int bitrate = 0;      // kbit/s, 0 = unknown
};

struct DabComponent {
    int tmid = 0;         // transport mechanism: 0 MSC stream audio, 1 MSC stream data, 3 MSC packet data
    int ascty = -1;       // audio service component type: 0 = MPEG-1/2 Layer II (DAB), 63 = HE-AAC (DAB+)
    int dscty = -1;
    int subId = -1;
    bool primary = true;
};

struct DabService {
    uint32_t sid = 0;
    std::string label;
    std::vector<DabComponent> comps;
    std::string dls;      // dynamic label ("now playing"), when the station sends one
    // The first audio stream component, or null
    const DabComponent* audio() const {
        for (const auto& c : comps) if (c.tmid == 0) return &c;
        return nullptr;
    }
    bool dabPlus() const { const DabComponent* a = audio(); return a && a->ascty == 63; }
};

struct DabEnsemble {
    bool valid = false;           // ensemble id and at least one service seen
    uint16_t eid = 0;
    std::string label;
    int64_t utc = 0;              // broadcast time (seconds since 1970), 0 = not received
    std::map<int, DabSubchannel> subs;
    std::map<uint32_t, DabService> services;
};

struct DabAudioStats {
    int sub = -1;                 // selected sub-channel, -1 = none
    bool dabPlus = false;
    int bitrate = 0;
    std::string codec;            // "HE-AAC v2", "AAC-LC", "MP2" ...
    int sampleRate = 0, channels = 0;
    bool decoding = false;        // audio is being produced
    uint64_t frames = 0;          // logical frames received
    uint64_t superframesOk = 0, superframesBad = 0;
    uint64_t rsCorrected = 0;     // bytes fixed by Reed-Solomon
    uint64_t auOk = 0, auBad = 0; // access units with a good / bad CRC
    uint64_t pcmFrames = 0;
    int bufferedMs = 0;
    int underruns = 0;
};

struct DabTelemetry {
    uint64_t seq = 0;
    bool sync = false;            // frame sync
    int state = 0;                // 0 searching, 1 syncing (found the null symbol), 2 locked
    float cirPeak = 0;            // peak / median of the phase reference correlation (about 100+ on a good signal)
    double cfoHz = 0;             // carrier frequency offset
    double snrDb = 0;             // estimated from the DQPSK clusters
    double secSinceSync = 0;
    uint64_t frames = 0;          // DAB frames (96 ms) processed
    uint64_t fibOk = 0, fibBad = 0;     // FIB CRC results since start
    int ficRecentOk = 0;          // FIBs with good CRC in the last frame (out of 12)
    bool ensemble = false;
    std::string ensembleLabel;
    int services = 0;
    std::vector<cf32> constellation;    // differential cells of one data symbol (clusters at the 4 diagonals)
    std::vector<float> cir;             // channel impulse response magnitude (phase reference correlation), 2048 points
    DabAudioStats audio;
};

} // namespace dect2

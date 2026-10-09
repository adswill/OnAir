// CDR receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "ring.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct CdrAudioInfo {            // one audio stream as the multiplex sub-frame header describes it (GY/T 268.2 Table 6)
    int algo = -1;               // audio algorithm code (its values are left to later documents; CDR sound is DRA+)
    int bitrate = 0;             // signalled bit rate, bit/s (0: not sent)
    int sampleRate = 0;          // Hz (0: not sent)
    int channelsCode = 0;        // Table 8: 1 mono, 2 two channels, 3 5.1
    std::string language;        // three letters, empty when not sent
};

struct CdrServiceInfo {
    uint16_t id = 0;             // service identifier (from the service multiplex configuration table)
    int smfId = 0;               // the service multiplex frame that carries it
    int subIndex = -1;           // its multiplex sub-frame in that frame
    bool seen = false;           // its sub-frame has been decoded at least once
    bool audio = false, data = false;
    std::vector<CdrAudioInfo> streams;
    double kbps = 0;             // bytes of its sub-frame per logical frame, as a rate
    int audioUnits = 0;          // audio units in its last sub-frame
    int audioBytes = 0;
    std::vector<int> dataTypes;  // data unit types seen (Table 12)
    int dataBytes = 0;
    std::string text;            // the last data broadcast unit, when it is printable UTF-8 text
    uint64_t subOk = 0, subBad = 0;   // sub-frame headers with good / bad CRC
};

struct CdrTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 searching for a beacon, 1 beacon found (synchronised), 2 system information decoded, 3 multiplex decoded
    double cfoHz = 0;            // carrier error
    float snrDb = 0;
    float levelDb = -200;        // power of the whole input, dBFS (-200 = nothing measured yet)
    bool dataValid = false;      // something has been decoded
    uint64_t blocksOk = 0;       // LDPC code words of the service data: decoded / failed
    uint64_t blocksBad = 0;
    // set-up
    double timeSec = 0;          // signal seconds since the start (now)
    double inputRate = 0;

    // ---- physical layer (GY/T 268.1)
    int tm = 0;                  // transmission mode 1..3 (0: not known yet)
    int sm = 0;                  // spectrum mode index
    int ni = 0;
    int innerKhz = 0, outerKhz = 0;   // the digital signal covers innerKhz .. outerKhz either side of the centre
    float syncMetric = 0;        // beacon correlation of the last sub-frame, 0..1
    double timingDriftPpm = 0;   // sample clock offset seen in the beacon timing
    uint64_t subframes = 0;      // sub-frames demodulated
    // system information channel (of the last sub-frame with a good CRC)
    bool siValid = false;
    int frame = -1, subframe = -1, alloc = 0, sdiMod = -1, msdMod = -1, hier = 0, rate = -1, rateLo = -1, nominalKhz = -1;
    bool uniform = true, multiFreq = false;
    int nextFreqCode = 0;
    uint64_t siOk = 0, siBad = 0;
    // service description channel: control multiplex frames
    uint64_t sdcOk = 0, sdcBad = 0;
    uint64_t tablesOk = 0, tablesBad = 0;
    std::vector<int> otherTables;          // control tables that are not decoded (ESG ...)
    // service data channel
    uint64_t muxOk = 0, muxBad = 0;        // service multiplex frame headers
    uint64_t logicalFrames = 0;
    float ldpcIterations = 0;              // average of the last logical frame
    int codewords = 0;                     // per logical frame
    int capacityBytes = 0;                 // service multiplex frame size

    // ---- multiplex (GY/T 268.2)
    std::string network;                   // network name
    std::string country;
    uint64_t networkId = 0;
    std::vector<double> freqsMhz;
    int nitUpdate = -1, smctUpdate = -1;
    std::vector<CdrServiceInfo> services;
    std::vector<std::string> notes;        // what the receiver cannot decode in this signal

    // ---- plots
    std::vector<cf32> siConst, sdcConst, mscConst;
    std::vector<float> chanDb;             // |H| of one OFDM symbol, per active carrier
    std::vector<float> chanKhz;            // their frequencies
    std::string status;
};

inline const char* cdrStateText(int s) {
    return s >= 3 ? "decoding the multiplex" : s == 2 ? "reading the system information" : s == 1 ? "synchronised" : "searching";
}

inline std::string cdrSummary(const CdrTelemetry& t) {
    char b[200];
    if (t.state == 0) snprintf(b, sizeof b, "CDR: searching, level %.1f dBFS", t.levelDb);
    else snprintf(b, sizeof b, "CDR: %s, mode %d, spectrum %d, SNR %.1f dB, %zu services, LDPC %llu ok %llu bad", cdrStateText(t.state), t.tm, t.sm,
                  t.snrDb, t.services.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    return b;
}

} // namespace dect2

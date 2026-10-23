// PSK demodulator (family 202) telemetry and settings, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <complex>

namespace dect2 {

constexpr size_t kPskdConstMax = 1024;   // constellation points kept
constexpr size_t kPskdEyeSyms = 48;      // eye diagram traces (16 points each)
constexpr size_t kPskdBitsMax = 16384;   // recent bits shown
constexpr size_t kPskdLogMax = 60;

struct PskdSettings {
    int order = 0;              // 0 automatic, 1 BPSK, 2 QPSK, 3 OQPSK, 4 8PSK (pskd::Order)
    double baud = 0;            // 0: found by the analyser
    double alpha = -2;          // -2 automatic, -1 rectangular pulses, 0..1 root-raised cosine roll-off
    double spanHz = 0;          // the search span around the dial (0: automatic, 200 kHz or what the radio gives)
    bool snapStandard = true;   // a rate within 0.6 % of a standard one is taken as that rate
    int diff = 0;               // pskd::Diff: 0 none, 1 DPSK, 2 NRZ-M, 3 NRZ-S / NRZI
    int scrambler = 0;          // pskd::Scrambler: 0 none, 1 G3RUH, 2 CCSDS
    std::string sync;           // sync word as bits or hex ("1ACFFC1D"); resolves the phase ambiguity; empty: none
    bool invert = false;        // invert the output bits
    bool followDoppler = true;  // keep the channel filter centred on the carrier as it moves (satellites)
    bool saveBits = false;      // append the decoded bits, packed into bytes, to savePath
    std::string savePath;
};

struct PskdRate { double baud = 0, conf = 0; std::string how; };
struct PskdOrderTrial { int order = 0; double evm = 0; };

struct PskdTelemetry {
    uint64_t seq = 0;
    int state = 0;              // 0 searching, 1 a signal (no lock), 2 locked
    double cfoHz = 0;           // carrier from the dial
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0;      // sync words found
    uint64_t blocksBad = 0;
    // details
    double timeSec = 0, inputRate = 0, spanHz = 0, chanRate = 0;
    std::string status = "waiting for samples";
    // the analyser's last result
    bool signal = false, rateFound = false;
    double baud = 0, baudRaw = 0, baudConf = 0; bool baudSnapped = false;
    int order = 0; double orderConf = 0;
    std::string shape; double alpha = 0; bool rect = false;
    double bw3Hz = 0, occHz = 0;
    double dopplerHzPerSec = 0;
    double evm = 0, merDb = 0, esn0Db = 0, ebn0Db = 0, bitRate = 0;
    double line2 = 0, line4 = 0, line8 = 0, lineEnv = 0;
    std::vector<PskdRate> rates;
    std::vector<PskdOrderTrial> orderEvm;
    std::vector<float> envSpecDb; double envHzPerBin = 0, envLineHz = 0;
    std::vector<float> powSpecDb; int powM = 2; double powHzPerBin = 0, powLineHz = 0;
    uint64_t analyses = 0;
    // live demodulation
    double lockMetric = 0;
    uint64_t symbols = 0, bitsTotal = 0, syncs = 0;
    int rotation = 0; bool conj = false; int railShift = 0; bool ambiguityResolved = false;
    std::vector<std::complex<float>> constellation;
    std::vector<float> eye;     // kPskdEyeSyms x 16 points of the I rail
    std::vector<uint8_t> bits;  // the newest bits, oldest first
    uint64_t bitsBase = 0;      // index of bits[0] in the whole stream
    std::vector<uint64_t> syncAt;   // stream bit index just after each recent sync word
    std::string savingTo; uint64_t bytesSaved = 0;
    std::vector<std::string> log;
    PskdSettings cfg;
};

inline const char* pskdOrderName(int o) {
    switch (o) { case 1: return "BPSK"; case 2: return "QPSK"; case 3: return "OQPSK"; case 4: return "8PSK"; default: return "auto"; }
}

inline std::string pskdSummary(const PskdTelemetry& t) {
    char b[200];
    if (t.state == 2) {
        std::snprintf(b, sizeof b, "PSK: %s %.0f Bd, Eb/N0 %.0f dB, %s", pskdOrderName(t.order), t.baud, t.ebn0Db,
                      t.syncs ? "sync words found" : "locked");
        return b;
    }
    if (t.state == 1) return "PSK: signal found, analysing";
    return "PSK: searching";
}

} // namespace dect2

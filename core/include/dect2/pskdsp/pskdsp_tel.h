// PSK demodulator telemetry: RxTelemetry::pskd, active standard 203 (family 202, engine standard 204), poll slot 163.
#pragma once
#include "dect2/pskdsp/pskdsp.h"
#include "dect2/pskdsp/pskdsp_modes.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct PskdTelemetry {
    uint64_t seq = 0, blocksOk = 0, blocksBad = 0;   // blocksOk: sync words found
    int state = 0;                  // 0 waiting for samples, 1 searching, 2 demodulating a locked signal
    double cfoHz = 0;               // carrier from the dial
    float snrDb = 0;
    bool dataValid = false;
    double inputRate = 0, centerHz = 0, timeSec = 0, searchHz = 0;
    // the blind analysis (the last one finished)
    bool analysing = false, haveAnalysis = false;
    std::string status;
    std::vector<std::string> log;
    bool signal = false, rateFound = false, analysisLocked = false;
    double carrierHz = 0, bwHz = 0, baud = 0, baudRaw = 0; bool baudSnapped = false;
    int order = 0;                  // pskd::Order
    double alpha = 0.35; bool rect = false; std::string shape;
    double evm = 0, merDb = 0, esn0Db = 0, ebn0Db = 0, bitRate = 0, dopplerHzPerSec = 0, analysisSnrDb = 0;
    double line2 = 0, line4 = 0, line8 = 0, lineEnv = 0;
    std::vector<std::pair<double, double>> rates;     // candidate baud, confidence (best first, at most 6)
    std::vector<std::pair<int, double>> orderEvm;     // pskd::Order, EVM
    std::vector<float> envSpecDb; double envHzPerBin = 0;
    std::vector<float> powSpecDb; int powM = 2; double powHzPerBin = 0;
    // the live demodulator
    double liveBaud = 0, liveEvm = 0, liveLock = 0, liveFreqHz = 0;
    std::vector<float> constellation;                 // interleaved I, Q of the latest symbols (at most 512 points)
    std::vector<float> spectrumDb;                    // dB over the floor, the search span centred on the dial
    double specLoHz = 0, specStepHz = 0;
    // the bits
    uint64_t bits = 0, syncs = 0;
    int rotation = 0; bool conj = false, resolved = false;
    std::vector<std::string> rows;                    // hex dump of the latest bits, 16 bytes a row
    std::vector<std::string> frames;                  // after each sync word (when one is set): hex, at most 12
    std::vector<uint8_t> recent;                      // the latest bits, one per element (at most 8192)
};

inline std::string pskdSummary(const PskdTelemetry& t) {
    if (t.state == 0) return "PSK demodulator: waiting for samples";
    if (t.state == 1 || t.baud <= 0) return "PSK demodulator: " + (t.status.empty() ? std::string("searching") : t.status);
    char b[160];
    snprintf(b, sizeof b, "PSK demodulator %s %.6g baud, Eb/N0 %.1f dB, %llu bits", pskd::orderName((pskd::Order)t.order), t.baud, t.ebn0Db, (unsigned long long)t.bits);
    return b;
}

} // namespace dect2

// NavIC (IRNSS) L5 SPS receiver, ISRO "IRNSS Signal-in-Space ICD for SPS" version 1.1 (2017), section numbers below are of that document.
// The 1023 chip Gold codes at 1.023 Mcps (section 4.1, Table 7), BPSK data at 50 symbols/s, 600 symbol subframes (16 symbol sync word EB90,
// 584 convolutionally coded and block interleaved symbols, section 5), CRC-24Q. Acquires, tracks and decodes the navigation message; shows the
// satellites, time of week and week number. Position fixes are not attempted: NavIC is receivable around India (Lithuania is outside its service area),
// and nothing here has been tried on a real signal.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kNavicMaxPrn = 14;
constexpr double kNavicL5Hz = 1176.45e6;
constexpr double kNavicFs = 4.096e6;   // the rate everything runs at
constexpr int kNavicSymbolsPerSubframe = 600, kNavicCoded = 584, kNavicBits = 292;

// The 1023 chips of a PRN (1..14), 0/1: L5 from Table 7 (s = true: the S band). False for another PRN.
bool navicChips(int prn, uint8_t* chips, bool sBand = false);
// The first ten chips as the ICD's octal column (first chip = leading digit, one bit)
unsigned navicFirst10Octal(int prn, bool sBand = false);

// Build / check a subframe (bits[292], 0/1: TLM, TOWC, ... CRC, tail)
void navicSetCrcTail(uint8_t* bits292);
bool navicCrcOk(const uint8_t* bits292);
void navicEncodeSubframe(const uint8_t* bits292, uint8_t* symbols600);   // sync word, coding, interleaving
// Decode 584 soft symbols following a sync word (positive favours 0); returns true when the CRC passes
bool navicDecodeSymbols(const float* soft584, uint8_t* bits292);

struct NavicFields { uint32_t towc = 0; bool alert = false, autonav = false; int subframeId = 0; int wn = -1; int msgId = -1; int prnId = -1; };
void navicParse(const uint8_t* bits292, NavicFields& f);

struct NavicSat {
    int prn = 0;
    int state = 0;              // 1 carrier lock, 2 bit sync, 3 frames decoded
    float cn0 = 0;
    double dopplerHz = 0;
    uint32_t frames = 0, bad = 0;
    int lastSubframe = 0;
    uint32_t towc = 0;
    int wn = -1;
    bool alert = false;
};

struct NavicTelemetry {
    uint64_t seq = 0;
    int state = 0;              // 0 searching, 1 tracking, 2 messages
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    double timeSec = 0, inputRate = 0;
    std::vector<NavicSat> sats;
    int searchPrn = 0;
};

inline std::string navicSummary(const NavicTelemetry& t) {
    char b[96];
    snprintf(b, sizeof b, "NavIC: %zu tracked, %llu subframes", t.sats.size(), (unsigned long long)t.blocksOk);
    return b;
}

class NavicReceiver {
public:
    NavicReceiver();
    ~NavicReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double) {}
    void setCenterHz(double) {}
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(NavicTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning navicTuning();
std::unique_ptr<ModeSynth> makeNavicSynth(const SynthConfig& cfg, double sampleRate);

// A test signal: nSats satellites at the given C/N0 with Dopplers across +-6 kHz. For tests and the mode's generator.
struct NavicGenConfig { double rate = kNavicFs; int nSats = 3; double cn0Db = 45; double dopplerHz = 3000; uint32_t seed = 3; };
class NavicGenerator {
public:
    explicit NavicGenerator(const NavicGenConfig& c);
    ~NavicGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2

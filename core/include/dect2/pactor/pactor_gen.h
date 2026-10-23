// PACTOR-I test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A script of events (a packet or a control signal at a given time, in the cycle grid of 1.25 s) rendered as continuous-phase FSK
// (200 Hz shift, 100 or 200 baud) around an audio centre tone, mixed up as the upper sideband of a radio, in white noise.
//
// SynthConfig (makePactorSynth): snrDb is the tone SNR in 2.5 kHz, cfoHz the radio's tuning error, modeOpt[0] 0 ARQ script, 1 unproto (FEC).
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/pactor/pactor_proto.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct PactorGenEvent {
    double startSec = 0;
    std::vector<uint8_t> bits;   // as sent (pactor::packetBits / csBits)
    int baud = 100;
    bool invert = false;         // swapped tone polarity (the real system toggles it every cycle)
};

struct PactorGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the dial frequency sits in the samples (-tuneOffsetHz for the radio, 0 for a recording)
    double audioCentreHz = 1500; // middle of the two tones, in the sideband audio
    double cfoHz = 0;
    double snrDb = 20;           // tone SNR in 2.5 kHz; >= 99: no noise
    double noiseSigma = 0.05;
    double clockPpm = 0;
    double amp = 0.4;
    bool repeat = true;
    double pauseSec = 3.0;
    double lengthSec = 0;        // script length; 0: after the last event
    std::vector<PactorGenEvent> events;
    uint32_t seed = 1;
};

class PactorGenerator {
public:
    explicit PactorGenerator(const PactorGenConfig& c);
    ~PactorGenerator();
    void generate(cf32* out, size_t n);
    double scriptSeconds() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Script helpers. cycleStart() is the time of cycle k; the polarity toggles with every cycle.
struct PactorScriptBuilder {
    double t0 = 0.5;
    int cycle = 0;
    std::vector<PactorGenEvent> ev;
    bool inv() const { return cycle & 1; }
    double ts() const { return t0 + cycle * pactor::kCycleSec; }
    void packet(const pactor::Packet& p, bool fastBaud) { ev.push_back({ts(), pactor::packetBits(p), fastBaud ? 200 : 100, inv()}); }
    void cs(int n) { ev.push_back({ts() + pactor::kCsStartSec, pactor::csBits(n), 100, inv()}); }
    void next() { cycle++; }
    double end() const { return ts() + 0.5; }
};
pactor::Packet pactorTextPacket(const std::string& text, bool huffman, uint8_t header, int count, bool fast, bool bk = false, bool qrt = false);
std::vector<PactorGenEvent> pactorTestScript(bool fec);
std::unique_ptr<ModeSynth> makePactorSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2

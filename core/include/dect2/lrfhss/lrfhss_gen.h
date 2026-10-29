// LR-FHSS test signal: frames of the LoRaWAN LR-FHSS data rates (lrfhss.h coding chain), each hop a GMSK burst at 488.28125 bit/s (BT 1,
// modulation index 0.5: +-122 Hz) on its carrier of the hopping sequence, over complex white noise; carrier offset, transmitter clock error,
// several frames, some overlapping in time. Nothing is transmitted: the samples only exist in memory or in a file.
// Assumptions not in Semtech's drivers (the radio does them): a 1 bit is the upper tone, the phase runs on across a hop, the first hop has one
// unmodulated symbol before the bits (the SX126x hop table gives it 115 symbols), hops switch at the block boundaries of the bit stream.
// Synthetic tests only; not checked against a real transmitter.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/lrfhss/lrfhss.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct LrfhssGenFrame {
    double mhz = 868.1;          // the OCW centre
    lrfhss::Params p;
    int hopSeqId = 0;
    int deviceOffset = 0;        // -4..3 (3.9 kHz grid) or -26..25 (25.4 kHz grid)
    std::vector<uint8_t> payload;   // the PHY payload (a LoRaWAN frame: lorawan::build)
    double gapSec = 0.3;         // silence after it
    double snrOffDb = 0;         // level against the generator's amp
    double cfoHz = 0;
    double startSec = -1;        // >= 0: fixed start time (frames overlap); else after the previous frame
};

// Renders frames into a buffer (adds to it; no noise). Times are seconds of the output stream.
class LrfhssRenderer {
public:
    LrfhssRenderer(double rate, double dialMhz, double dialOffsetHz, double sroPpm, uint32_t seed);
    ~LrfhssRenderer();
    // f.startSec is ignored here: the frame starts at startSec, with amplitude amp
    void add(const LrfhssGenFrame& f, double startSec, double amp, double extraCfoHz);
    void render(cf32* out, size_t n, double t0);   // samples t0 .. t0 + n / rate; frames that ended are dropped
    size_t active() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
// Seconds from the start of a frame (the unmodulated first symbol) to the end of its last bit
double lrfhssFrameSeconds(const LrfhssGenFrame& f);

struct LrfhssGenConfig {
    double rate = 1e6;
    double dialMhz = 868.3;      // the frequency the samples are centred on, apart from dialOffsetHz
    double dialOffsetHz = 0;     // where the dial sits in the samples
    double ebN0Db = 20;          // Eb/N0 of a frame at amp (snrOffDb 0); >= 99: no noise
    double cfoHz = 0;
    double sroPpm = 0;
    double amp = 0.3;
    double leadSec = 0.05;
    bool loop = false;
    uint32_t seed = 1;
    std::vector<LrfhssGenFrame> frames;
};

class LrfhssGenerator {
public:
    explicit LrfhssGenerator(const LrfhssGenConfig& c);
    ~LrfhssGenerator();
    void generate(cf32* out, size_t n);
    double endSec() const;       // when the last scheduled frame ends
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
// End of the schedule (seconds) without looping
double lrfhssSceneSeconds(const std::vector<LrfhssGenFrame>& v, double leadSec = 0.05);

} // namespace dect2

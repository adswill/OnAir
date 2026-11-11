// CP16 test signal and transmitter (cp16_proto.h). The test signal only exists in memory or in a file; the transmitter is the TX session's generator.
//
// The carriers are made at 16 kHz complex (cp16::Modulator) and interpolated to the output rate (cubic), with the sample clock error, then moved to
// the dial's place in the band and given the tuning error.
//
// Through the engine (SynthConfig, mode 882):
//   modeOpt[0]  speed: index into cp16::kCharSecChoices (0: 2 s a character)
//   modeOpt[1]  spacing: index into cp16::kSpacingChoices (0: 15 Hz)
//   modeOpt[2]  1 = lower sideband
//   modeOpt[3]  text: 0 Chinese with a callsign, 1 letters and digits only
//   modeVal[0]  audio frequency of the lowest carrier, Hz (0: 1000)
// snrDb: the signal's power (time average) over the noise in 2500 Hz. cfoHz: tuning error. sroPpm: the transmitter's clock error (the rows and the
// carriers both scale with it). The message is repeated after 2 s of silence.
//
// Transmit (makeCp16TxGenerator): TxRequest::text (empty: the demo text), options "cp16.char" (seconds a character, 2), "cp16.spacing" (Hz, 15),
// "cp16.base" (Hz, 1000), "cp16.lsb" (0 / 1). The text is sent once, after 0.5 s of silence; then the generator has finished. The signal is the
// complex baseband of an SSB transmitter: the carriers at +audio Hz (upper sideband) or -audio Hz (lower) from the dial. Nothing about mode,
// frequency or power is limited here.
#pragma once
#include "dect2/cp16/cp16_proto.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/tx/tx.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

const char* cp16DemoText(int which);   // 0 Chinese with a callsign, 1 letters and digits

struct Cp16GenOptions {
    cp16::Params params;
    std::string text;
    double rate = 250000;
    double dialOffsetHz = 0;     // where the dial sits in the output band
    double cfoHz = 0;
    double sroPpm = 0;
    double snrDb = 200;          // >= 150: no noise
    double ampPerCarrier = 0.055;   // with noise: replaced by the level the SNR asks for
    double leadSec = 0.5, gapSec = 2.0;
    bool repeat = true;
    uint32_t seed = 1;
};

class Cp16Generator {
public:
    explicit Cp16Generator(const Cp16GenOptions& o);
    ~Cp16Generator();
    void generate(cf32* out, size_t n);
    bool finished() const;       // repeat off: the text and the tail have been made
    double messageSec() const;   // one pass of the text
    const std::vector<uint32_t>& cells() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeCp16Synth(const SynthConfig& cfg, double sampleRate);
std::unique_ptr<TxGenerator> makeCp16TxGenerator(const TxRequest& r, double rateHz, std::string& err);

} // namespace dect2

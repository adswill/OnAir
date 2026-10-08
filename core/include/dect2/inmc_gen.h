// Inmarsat-C test signal: the generator the synthetic source plays and inmctool writes to a file. Nothing is transmitted.
//
// What it makes: the NCS common channel of the Indian Ocean Region: continuous BPSK at 1200 symbols/s (root raised cosine, roll-off 1),
// frames of 8.64 s with the unique word, interleaver, rate 1/2 code and scrambler as the receiver expects them. Every frame starts with a
// bulletin board packet (the frame number grows by one) and a signalling channel packet, followed by up to three EGC packets of a loop of five
// messages: a NAVAREA IX warning, a METAREA IX gale warning, a SAR notice, a weather forecast for the Gulf of Oman and the Arabian Sea
// (long: it needs several frames), and a FleetNET message in ITA2. The text is made up; the area address bytes of the EGC packets follow the
// assumption described in inmc_pkt.h. Nothing here was compared with a real signal.
//
// The signal sits at -tuneOffsetHz (-50 kHz) from 0 Hz, so that it lands on the user's frequency when the radio is tuned 50 kHz above it.
//
// SynthConfig::modeOpt and modeVal carry this mode's options. Zero always means the default.
//   modeOpt[0]  random seed (0 = 1): message numbers and area bytes
//   modeOpt[1]  1: send the symbols with inverted polarity (the receiver must find out from the unique word)
//   modeOpt[2]  frame number of the first frame (0 = 1000)
//   modeVal[0]  Eb/N0 in dB (0 = derived from SynthConfig::snrDb as snrDb - 20, which is 10 dB at the default of 30)
//   modeVal[1]  carrier drift in Hz per second (0 = none)
//   modeOpt[3]  more channels in the same capture, 0 to 3 (default 0): LES TDM channels at -20 kHz, -90 kHz and +30 kHz from the centre (the main one is
//               at -50 kHz), each with its own messages, station, carrier offset (+1200, -2300, +700 Hz extra) and frame timing, at the same Eb/N0
//   modeVal[2], modeVal[3]  unused
// SynthConfig::cfoHz is the carrier offset, sroPpm the clock error of the sample clock. Eb/N0 counts the information bits (rate 1/2 code:
// Eb/N0 = Es/N0 + 3 dB) with the noise in the whole sampled band; the total level is set to an rms of 0.2.
#pragma once
#include "inmc_code.h"
#include "inmc_pkt.h"
#include "mode_synth.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct InmcGenConfig {
    double rate = 2e6;
    uint32_t seed = 1;
    double ebn0Db = 10;
    bool noiseless = false;      // no noise at all (tests)
    double cfoHz = 0;
    double driftHzS = 0;
    double sroPpm = 0;
    double offsetHz = -50000;    // where the channel sits in the baseband
    bool invert = false;
    uint32_t firstFrame = 1000;
    double rms = 0.2;
    int extraChannels = 0;       // more channels at fixed offsets (see above)
    // used for the extra channels
    int les = 44, channelType = 1;
    double sigScale = 0;         // > 0: use this signal scale and add no noise
    uint32_t startSkip = 0;      // start this many symbols into the first frame
};

// What the loop of EGC messages contains (for tests)
struct InmcTestMessage {
    uint16_t id = 0;
    uint8_t service = 0;
    int priority = 0;
    int presentation = 0;
    std::vector<uint8_t> address;
    std::string text;
};

// Builds the frames: the content of the loop.
class InmcFrameSource {
public:
    explicit InmcFrameSource(uint32_t seed = 1, uint32_t firstFrame = 1000, int les = 44, int channelType = 1);
    void next(uint8_t frame[inmc::kFrameBytes]);        // the next frame's 640 bytes
    uint32_t frameNumber() const { return frame_; }     // number of the frame next() builds next
    const std::vector<InmcTestMessage>& messages() const { return msgs_; }
private:
    uint32_t frame_;
    int les_, channelType_;
    std::vector<InmcTestMessage> msgs_;
    std::vector<std::vector<uint8_t>> packets_;         // every packet of every message, in sending order
    size_t cursor_ = 0;
};

class InmcSynth : public ModeSynth {
public:
    virtual const InmcFrameSource& source() const = 0;     // the main channel
    virtual size_t extraCount() const { return 0; }
    virtual const InmcFrameSource& extraSource(size_t) const { return source(); }   // the other channels
};

std::unique_ptr<InmcSynth> makeInmcGenerator(const InmcGenConfig& cfg);
std::unique_ptr<ModeSynth> makeInmcSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2

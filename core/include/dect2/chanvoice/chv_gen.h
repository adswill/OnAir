// Test signals for the channelised voice receiver. Nothing is transmitted: the samples only exist in memory or in a file.
// A scene is a list of transmissions on absolute frequencies (FM, AM, USB, LSB, with speech-like audio, an optional CTCSS tone or DCS code and an optional
// MDC-1200 burst before the voice) plus noise, placed in a complex baseband span around a centre frequency; the wireless microphone scene is a list of
// wide FM carriers. Audio comes from ToneScene (tones/tones_gen.h): 1.0 is 5 kHz of FM deviation.
#pragma once
#include "dect2/chanvoice/chv_mdc.h"
#include "dect2/chanvoice/chv_plan.h"
#include "dect2/common/ring.h"
#include <memory>
#include <vector>

namespace dect2 {
namespace chv {

struct GenTx {
    double mhz = 0;               // the channel (carrier) frequency
    Demod mod = Demod::Nfm;       // Nfm, Am, Usb, Lsb, Wfm (wide FM: wireless microphone)
    double startSec = 0, durSec = 2;
    double amp = 0.3;             // carrier (or SSB peak) amplitude, full scale of the span = 1
    double devScale = 1.0;        // FM: deviation as a fraction of 5 kHz (0.5 for a 12.5 kHz channel); Wfm: peak deviation in Hz is 40 kHz x this
    double depth = 0.8;           // AM modulation depth
    double cfoHz = 0;             // carrier frequency error
    double ctcssHz = 0;           // sub-audible tone under the voice (0 none), 0.08 of full scale = 400 Hz
    int dcsCode = -1; bool dcsInverted = false;
    bool speech = true;           // speech-like audio; false: the tones below
    std::vector<double> tones = {1000.0};   // the test tones of a non-speech transmission (Hz), each 0.3 / n of full scale
    bool mdc = false; MdcPacket mdcPacket;   // an MDC-1200 burst at the start, voice after it
    unsigned seed = 1;
};

struct GenScene {
    double rate = 1e6;
    double centreMhz = 27.185;
    double durSec = 4;
    double noiseSigma = 0.002;    // rms per component
    std::vector<GenTx> tx;
};

std::vector<cf32> makeChanVoiceScene(const GenScene& s);

// The demonstration scene of a service (what the built-in test source plays, looped): the plan's default centre, a few channels with calls,
// tones, an MDC-1200 burst or microphones, region = the plan's region index. snrDb: carrier to noise in 10 kHz.
GenScene chanVoiceDemoScene(Service s, int region, double rate, double snrDb);

} // namespace chv
} // namespace dect2

namespace dect2 {
class ModeSynth;
struct SynthConfig;
}
namespace dect2 {
// SynthConfig: snrDb = carrier to noise in 10 kHz; modeOpt[0] = the plan's region index. Engine standards 342 .. 348 (families 340 .. 346).
std::unique_ptr<ModeSynth> makeChanVoiceSynth(const SynthConfig& cfg, double sampleRate, chv::Service s);
}

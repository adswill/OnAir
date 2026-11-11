// Voiceband detectors and decoders of the W1A batch (families 800 - 802). Receive only. Public sources only (docs/W1A-3.9.md):
//   Packet   HF packet radio: AX.25 frames in 300 Bd AFSK, tones 1600 / 1800 Hz (200 Hz shift), NRZI, bit stuffing, CRC-16. Full decode: the frames
//            are read with the repo's own AFSK receiver and AX.25 parser (the receiver the APRS HF side uses).
//   Invert   Analog voice scrambler, full-band frequency inversion (f -> fc - f). Detection from the long-term speech spectrum, an estimate of fc
//            and the descrambled audio. Split-band inversion and rolling-code scramblers are not covered.
//   Elt      Emergency locator transmitter on 121.5 MHz: AM carrier (m >= 0.85) with a tone swept downwards over at least 700 Hz inside 1600 - 300 Hz
//            at 2 - 4 sweeps per second (UK CAA specification text, ICAO Annex 10 Vol III). Detection only: the sweep and its rate.
// Input: complex baseband at kRate (12 kHz). Packet: the upper sideband, audio frequency f at +f Hz. Invert: a narrow-FM channel at 0 Hz.
// Elt: an AM channel at 0 Hz.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::w1a {

constexpr double kRate = 12000.0;
enum Kind { Packet = 0, Invert = 1, Elt = 2 };
constexpr int kKinds = 3;

struct Param { std::string key, value; };

struct Status {
    int state = 0;              // 0 searching, 1 signal found, 2 decoded / confirmed
    std::string mode;           // one line: what was found
    std::vector<Param> params;  // measured parameters, in display order
    uint64_t good = 0, bad = 0;
    double cfoHz = 0;           // see W1aTelemetry::cfoHz
    double snrDb = 0;
    std::vector<float> spec;    // Invert: long-term spectrum (dB), 0 .. 4.8 kHz, 50 Hz steps
};

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual void reset() = 0;
    virtual void push(const cf32* x, size_t n) = 0;
    virtual Status status() const = 0;
    void setTextSink(std::function<void(const std::string&)> cb) { text_ = std::move(cb); }
    void setEventSink(std::function<void(const std::string&)> cb) { event_ = std::move(cb); }
    // Invert: the descrambled audio (12 kHz mono) as it is produced; also the forced carrier (Hz, 0 = automatic)
    void setAudioSink(std::function<void(const float*, size_t)> cb) { audio_ = std::move(cb); }
    virtual void setForced(double fcHz) { (void)fcHz; }
protected:
    void text(const std::string& s) { if (text_) text_(s); }
    void event(const std::string& s) { if (event_) event_(s); }
    void audio(const float* x, size_t n) { if (audio_) audio_(x, n); }
private:
    std::function<void(const std::string&)> text_, event_;
    std::function<void(const float*, size_t)> audio_;
};

std::unique_ptr<Decoder> makeDecoder(int kind);

// ---- test generators (12 kHz complex) ----
struct GenOpts {
    uint32_t seed = 1;
    double leadSec = 0.3, tailSec = 0.3;   // silence around a packet burst
    // Packet
    int frames = 4;                         // frames sent in the burst
    double markHz = 1600, spaceHz = 1800;
    // Invert
    double seconds = 12;                    // length of the speech-like signal
    double invertHz = 3000;                 // 0 = clear (not inverted)
    double deviationHz = 2500;              // NFM peak deviation (full-scale audio)
    // Elt
    double sweepHz = 3.0;                   // sweeps per second
    double dutyPct = 60;                    // how much of a sweep period carries the tone
    double highHz = 1600, lowHz = 300;      // sweep limits
    double depth = 0.9;                     // AM modulation factor
};
std::vector<cf32> genPacket(const GenOpts& o);              // USB audio at +f Hz
std::vector<float> genSpeechLike(const GenOpts& o);         // 12 kHz audio, 300 - 3000 Hz, peak about 0.5 (simulated speech)
std::vector<float> invertAudio(const std::vector<float>& x, double fcHz);   // f -> fc - f (the scrambler; the same operation descrambles)
std::vector<cf32> genInvert(const GenOpts& o);              // inverted speech-like audio on a narrow-FM carrier at 0 Hz
std::vector<cf32> genElt(const GenOpts& o);                 // AM carrier at 0 Hz with the swept tone
// Test impairments: carrier offset, white noise for an SNR in a 3 kHz band (>= 90: none), sample clock error in ppm
std::vector<cf32> impair(const std::vector<cf32>& x, double cfoHz, double snrDb3k, double ppm, uint32_t seed);

const char* kindName(int k);

}  // namespace dect2::w1a

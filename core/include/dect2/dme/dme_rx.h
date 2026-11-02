// DME (distance measuring equipment) receiver: the ground beacon answers an aircraft interrogation with pulse pairs (ICAO Annex 10 Vol I, DME/N). The pulses are 3.5 us
// wide at half amplitude; the two pulses of a pair are 12 us apart on the X channel and 30 us apart on the Y channel (the aircraft's interrogations are 12 us on X and 36 us on Y,
// so a receiver tuned to the interrogation frequency sees 36 us for Y). The receiver works on the detected envelope (|IQ| or AM demodulated video) at a rate of at least
// 2 Msps: it finds the pulses with a peak-relative threshold, pairs them by spacing, classifies X and Y, measures the pair rate, and reads the beacon identification:
// the ident is Morse (dot about 0.1 s, dash three dots, a gap of one dot between elements and three dots between letters) sent as regular pulse pairs at 1350 pairs per second
// (the replies and squitter of the beacon are random, so a run of pairs at exactly that period is the keyed ident). A beacon sends its ident at least every 40 s.
// Sources: ICAO Annex 10 as summarised in Rohde & Schwarz application note 1EF91 (DME/N pulse shape, 12 / 30 us spacing, 1350 pp/s ident, Morse timing) and the public
// descriptions of the channel plan; credited in THIRD_PARTY.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace dect2::dme {

enum class Channel : uint8_t { None, X, Y };
const char* channelName(Channel c);

constexpr double kPulseWidthUs = 3.5;      // nominal width of one reply pulse
constexpr double kPulseTolUs = 1.5;        // accepted width is 3.5 +- 1.5 us
constexpr double kSpacingXUs = 12.0;       // X channel pulse spacing
constexpr double kSpacingYUs = 30.0;       // Y channel reply pulse spacing
constexpr double kSpacingYInterrogationUs = 36.0;   // Y channel interrogation spacing (an aircraft, seen on the interrogation frequency)
constexpr double kSpacingTolUs = 1.5;      // accepted spacing is the nominal value +- 1.5 us
constexpr double kIdentRatePps = 1350.0;   // pair rate while the ident tone is keyed (a regular pulse train)
constexpr double kIdentTolUs = 45.0;       // accepted deviation of the interval between ident pairs from 1/1350 s (740.7 us)
constexpr double kSearchRatePps = 2700.0;  // pair rate of the normal reply stream

struct Telemetry {
    Channel channel = Channel::None;   // the channel most pairs of the last second used
    double pairRatePps = 0.0;          // pulse pairs per second over the last second
    int pairsX = 0, pairsY = 0;        // pairs counted since the start (Y: 30 us replies and 36 us interrogations)
    int pairsYInterrogation = 0;       // of pairsY, the 36 us ones: an aircraft interrogating, so the radio is on the interrogation frequency
    double identIntervalUs = 0;        // mean interval of the last keyed run (about 740.7)
    double levelDb = -200.0;           // envelope peak level, dB
    bool identKeyed = false;           // the ident tone is on now
    std::string ident;                 // decoded ident letters (a space between words), empty until one letter is out
};

class Receiver {
public:
    // sampleRate: envelope samples per second (at least 2e6)
    void configure(double sampleRate);
    void reset();
    // Appends the envelope samples: linear amplitude, any scale (the threshold follows the peak).
    void feed(const float* env, size_t n);

    const Telemetry& telemetry() const { return t_; }
    double timeSec() const { return sampleCount_ / (fs_ > 0 ? fs_ : 1.0); }

private:
    void onPulse(double startSec);
    void onPair(double startSec);
    void endKey();
    void closeBin();
    void decodeLetter();
    void updateTelemetry(double now);

    double fs_ = 0.0;
    double decay_ = 0.0;          // per-sample decay of the peak follower (time constant 0.2 s)
    double peak_ = 0.0;
    bool inPulse_ = false;
    uint64_t startSample_ = 0;
    uint64_t sampleCount_ = 0;

    // pair pairing: the last accepted pulse waits for its partner
    bool havePrev_ = false;
    double prevStartSec_ = 0.0;

    // pair history for the rate and the channel (last second)
    std::deque<std::pair<double, Channel>> pairs_;

    // ident: runs of pairs at the 1350 pp/s period are key-down; their length gives dots and dashes, the silence between them the gaps
    uint64_t binSamples_ = 0;
    uint64_t binEnd_ = 0;
    int binPairs_ = 0;
    bool keyed_ = false;
    bool havePair_ = false;
    double lastPairSec_ = 0, keyStartSec_ = 0, lastRegSec_ = 0, offStartSec_ = 0;
    int regular_ = 0;             // consecutive intervals at the ident period
    double intervalSum_ = 0;
    bool letterFlushed_ = false;
    bool wordFlushed_ = false;
    std::string code_;            // dots and dashes of the letter being received

    Telemetry t_;
};

// Morse decoding helper used by the ident: the letter for a dot-dash group, '?' when unknown.
char morseLetter(const std::string& code);

// The DME channel of a reply frequency (ICAO Annex 10 channel plan, 962 - 1213 MHz in 1 MHz steps: every reply frequency belongs to exactly one of the 252 channels 1X..126Y).
// Returns the channel number 1..126 and sets y (true: a Y channel); 0 when the frequency is not a DME reply frequency.
int channelFromReplyMHz(double mhz, bool& y);
// The interrogation frequency (MHz) of channel n (1024 + n)
inline double interrogationMHz(int n) { return 1024.0 + n; }

} // namespace dect2::dme

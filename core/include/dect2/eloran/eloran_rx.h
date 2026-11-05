// Loran-C detector (the eLoran data channel is not decoded: see the notes at the end of this header; its 9th pulse position states are here).
// Works on complex baseband with the 100 kHz carrier at 0 Hz and the band limited to about +-11 kHz, sampled at fs >= 250 kHz.
//
// Signal model, from public sources only:
//  * carrier 100 kHz, pulse envelope t^2 exp(-2t/65 us) (65 us time constant, peak 65 us after the start);
//  * a station sends a group of 8 pulses spaced 1000 us; the master adds a 9th pulse 2000 us after the 8th;
//  * the group repeats every GRI (a multiple of 10 us, written GRI/10, for example 9960 = 99600 us);
//  * the carrier phase of each pulse is 0 or 180 degrees, group types A and B alternate (the pattern repeats every two GRIs):
//      master    A ++--+-+-   B +--+++++
//      secondary A +++++--+   B +-+-++--
//    (A of both and the rule "even pulses keep their sign, odd pulses flip between A and B" are in the cited sources; the B rows follow from that rule).
//  * not established from public text, so not used: the coding delays of any chain, the phase of the master's 9th pulse.
// Public sources: https://www.jproc.ca/hyperbolic/loran_c_sigchar.html (carrier, 1000 us spacing, 8/9 pulses, 180 degree coding, A/B alternation),
// https://patents.google.com/patent/US4392138 (even pulses keep their sign, odd pulses flip between A and B, master A pulse 3 = -1),
// https://www.mdpi.com/1424-8220/20/8/2329 (envelope t^2 exp(-2t/65 us)), https://www.sigidwiki.com/wiki/Eurofix (data channel summary).
#pragma once
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace dect2::loran {

constexpr double kTauUs = 65.0;            // envelope time constant
constexpr double kPulseSpacingUs = 1000.0;
constexpr int kGroupPulses = 8;
constexpr double kEnvLengthUs = 260.0;     // template length: the tail beyond this is under 2 percent of the peak

// The chain that uses a GRI (group repetition interval in microseconds, 99600 for GRI 9960), from the public chain lists; nullptr when it is not a known chain.
const char* chainName(int griUs);

// eLoran 9th pulse data channel: every station sends one extra pulse 1000 us after its eighth pulse, position modulated with one of 32 states (5 bits per GRI): state 8m + n is
// delayed by 50.625 m + 1.25 n microseconds (m 0..3, n 0..7), always with the positive phase code (Lo et al., IEEE TAES 43(2), 2007, equation 2 and table III).
// A message is 24 states (RS(24,9) over 5-bit symbols, 120 bits): the Reed-Solomon constants and the message layouts of the Loran Data Channel standard are not public text,
// so the states are measured and shown but the frame is not decoded.
double ninthPulseDelayUs(int state);          // ideal delay of state 0..31
int ninthPulseState(double delayUs);          // the nearest state of a measured delay (carrier phase and envelope together), -1 outside 0 .. 165 us

// phase pattern of the 8 pulses ('+' = 0 degrees, '-' = 180 degrees); code 'A' or 'B'
const char* phasePattern(bool master, char code);
// the pulse envelope at t microseconds after the pulse start (peak value 1 at kTauUs)
double pulseEnvelope(double tUs);

struct Station {
    bool master = false;       // the 9th pulse was seen
    bool codeMaster = false;   // the phase code is a master code
    double absUs = 0;          // position of the first pulse inside the GRI (the pulse start), microseconds
    double toaUs = 0;          // arrival relative to the master (0 for the master), microseconds, 0 <= toa < GRI
    float levelDb = -200;      // peak carrier amplitude, dBFS
    float marginDb = 0;        // detection strength over the threshold, dB
    char code = '?';           // phase code of the newest group, 'A' or 'B'
    float codeScore = 0;       // 0..1, how much of the pulse energy the code explains
    int groupsUsed = 0;
    bool ninth = false;        // energy at the 9th pulse position (2000 us after the 8th)
    double cfoHz = 0;          // the tuning error of the carrier, measured from the rotation between the pulses of a group (within +-250 Hz)
    int hypA = 0;              // which pattern the even groups of the window carry (0 = A), used for the averaged pulse
};

struct Result {
    bool found = false;
    int griUs = 0;             // microseconds (multiple of 10)
    double zScore = 0;         // strength of the group repetition
    bool masterFound = false;
    std::vector<Station> stations;   // master first, then by arrival
    std::vector<float> pulseEnv;     // averaged pulse envelope (linear amplitude) around the first station
    double envStartUs = 0, envStepUs = 0;
};

class Detector {
public:
    explicit Detector(double fs);
    // x: baseband, n samples (about 2 s). hintGriUs: try this GRI first (0 = scan 40000..99990 us)
    Result analyze(const std::complex<float>* x, size_t n, int hintGriUs = 0) const;
    double fs() const { return fs_; }
private:
    double fs_;
};

// Not implemented: the eLoran data channel. The public text found (patents and a signal wiki) only says that Eurofix shifts the last six
// pulses of the group by +-1 us (729 patterns, 128 balanced ones carry 7 bits per GRI, 210-bit Reed-Solomon messages). The pattern table and
// the code polynomial are not in it, so no data is decoded.

}  // namespace dect2::loran

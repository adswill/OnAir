// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal identifier: what is this carrier? From a stretch of complex samples it measures the centre, the -3 / -20 / -26 dB and 99 % widths
// (specx::measure on its own spectrum), continuous or bursty (duty, burst length, the spacing of the burst starts), the modulation class (AM, FM,
// constant envelope, 2- or 4-level FSK, GMSK, BPSK / QPSK / pi/4-DQPSK / 8PSK from the |x|^n lines, QAM, OFDM from the flat top and the cyclic
// prefix correlation, LoRa chirps, pulses), the symbol rate (cyclostationary line of |x|^2, of the FM discriminator or of x^n) and, for FSK, the
// deviation levels. Then it scores the table of OnAir's modes (sig_known.cpp: what each one looks like) against the measurement, looks for the frame
// sync of the land-mobile systems and POCSAG / AIS where that is cheap, lets the frequency database and the band plan move the scores a little,
// and turns the scores into calibrated probabilities with a reject option ("unknown"). Besides those it measures the spectral shape (roll-off,
// tones and their spacing, the comb of an OFDM or MFSK spectrum) and, for linear modulation, a constellation guess at the symbol instants; and it
// verifies the best few candidates with their own detectors (frame sync, symbol clock lock at the mode's exact rate, the cyclic prefix at the
// mode's symbol length, chirp width, burst timing, deviation). The table of modes is the hand-written one (sig_known.cpp), the one measured on
// the modes' own test signals (sig_known_gen.cpp) and, for the modes in neither, rows made from the mode list (ModeTuning). identify() is a pure,
// deterministic function; Capture collects samples from the engine's sample path and runs it on its own thread (docs/IDENTIFIER.md says how it
// works, what the numbers mean and how well it does).
#pragma once
#include "dect2/channels/memory_bank.h"
#include "dect2/common/ring.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {
namespace sigid {

enum Mod {
    ModUnknown = 0, ModNoise, ModCarrier, ModAm, ModFm, ModFsk2, ModFsk4, ModGmsk, ModBpsk, ModQpsk, ModPi4Dqpsk, ModPsk8, ModQam,
    ModOfdm, ModChirp, ModPulse, ModCount
};
const char* modName(int m);   // "4FSK", "pi/4-DQPSK", ...

// The shape of the spectrum across the occupied band
enum Shape { ShapeUnknown = 0, ShapeFlat, ShapeRounded, ShapeTones, ShapeLine, ShapeCount };
const char* shapeName(int s);   // "flat top", "rounded (filtered)", "discrete tones", "a line"

struct Features {
    bool valid = false;
    std::string why;               // when not valid: what was wrong ("no signal above the floor", ...)
    double centreHz = 0;           // the signal's centre, from the samples' centre (the middle of its 99 % band)
    double rfHz = 0;               // the same in RF (Options::rfHz + centreHz)
    double bw3Hz = 0, bw20Hz = 0, bw26Hz = 0, obwHz = 0;   // widths at -3, -20, -26 dB and the 99 % occupied bandwidth
    double snrDb = 0, powerDb = -200;
    bool flatTop = false;
    double duty = 1;               // share of the time the signal is on (1: continuous)
    int bursts = 0;                // bursts seen (0 for a continuous signal)
    double burstMs = 0;            // median length of a burst
    double burstPeriodMs = 0;      // median start-to-start spacing of the bursts when they come regularly (a TDMA frame or slot), else 0
    double captureSec = 0;
    double envKurt = 0;            // E|s|^4 / (E|s|^2)^2 with the noise taken out: 1 constant envelope, ~1.3 filtered PSK, 2 Gaussian (OFDM)
    double carrierDb = 0;          // a line at the centre over the signal's own spectrum (AM, an unmodulated carrier)
    double lineDb[4] = {0, 0, 0, 0};    // the strongest line of (x/|x|)^1, ^2, ^4, ^8 near the centre, over the median, dB
    double lineHz[4] = {0, 0, 0, 0};    // and where it sits from the centre (x^n: n times the carrier's offset; MSK, pi/4: +-Rs/2 n-fold)
    int mod = ModUnknown;
    double symRate = 0;            // symbols per second, 0 = none found
    double symLineDb = 0;          // how far its line stands out
    int symSrc = 0;                // 0 none, 1 |x|^2, 2 FM discriminator, 3 x^n lines, 4 chirp length, 5 OFDM symbol
    int fskLevels = 0;             // 2 or 4 for FSK
    double devHz[4] = {0, 0, 0, 0};   // FSK levels, lowest first, around the centre (4FSK: -outer, -inner, +inner, +outer)
    double devSpread = 0;          // rms scatter of the FSK samples around their levels, Hz
    double ofdmTuSec = 0, ofdmTgSec = 0, ofdmCorr = 0;   // OFDM: useful symbol, guard, cyclic prefix correlation
    double chirpBwHz = 0; int loraSf = 0;                 // LoRa
    bool fmPilot = false;          // a 19 kHz pilot in the FM discriminator (stereo broadcast)
    // spectral shape (a Welch spectrum of the signal at about 1/1000 of its width)
    int shape = ShapeUnknown;
    double rolloff = -1;           // linear modulation: excess bandwidth, 99 % width / symbol rate - 1 (-1 not measured)
    int tones = 0;                 // distinct lines standing out of the band (MFSK tones, wide-shift FSK, pilots, a carrier)
    double toneSpacingHz = 0;      // their common spacing (0: fewer than two, or no common spacing)
    double combHz = 0, combStrength = 0;   // the strongest periodicity of the spectrum across the band (OFDM pilots, an MFSK grid): spacing, 0..1
    double ofdmSpacingHz = 0, ofdmGuard = 0;   // OFDM: carrier spacing 1 / Tu, guard fraction Tg / Tu
    // constellation (linear modulation with a symbol rate: matched filter, best timing phase, the symbols)
    int constOrder = 0;            // the order the symbols look like: 2, 4, 8 (PSK), 16, 64 (QAM); 0 not measured
    int ampRings = 0;              // amplitude levels of the symbols (1 PSK, 3 for 16-QAM, more for larger QAM)
    double phaseCoh[3] = {0, 0, 0};   // |mean of u^M| at the symbol instants (u = s / |s|) for M = 2, 4, 8: 1 is a clean M-PSK
};

// one row per measured feature, for the "why" view and the text form: (what, value), English
std::vector<std::pair<std::string, std::string>> featureRows(const Features& f);

enum VerifyState { VerifyNotTried = 0, VerifyConfirmed = 1, VerifyFailed = -1, VerifyInconclusive = 2 };

struct Candidate {
    std::string name;              // "DMR", "NXDN96", ...
    std::string modeId;            // ModeTuning::id of the mode that decodes it ("" none)
    std::string subId;             // SubMode id inside that mode ("" none)
    std::string family;            // "Digital voice", "Aviation", ... (the group the accuracy table is kept by)
    double score = 0;              // 0 .. 100, the raw evidence
    double confidence = 0;         // 0 .. 1, calibrated on the app's own test signals (docs/IDENTIFIER.md): the share of this and only this being right
    std::vector<std::string> evidence;   // one line per rule that moved the score: "symbol rate 4800 Bd within 0.4 % (+25)"
    int syncHits = 0;              // frame syncs found (0 not looked for or none)
    bool syncTried = false;
    std::string why;               // what matched: "4FSK 4800 Bd, +-1944 Hz, 12.5 kHz"
    int verify = VerifyNotTried;   // the verify step (Options::verify): the mode's own detectors on the samples
    std::vector<std::string> verifyNotes;   // what each detector found: "symbol clock locks at 4800 Bd (+0.02 %, 23 dB)"
    int source = 0;                // where its signature comes from: 0 hand-written, 1 measured on its test signal, 2 the mode list only
    int sig = -1;                  // its row of the signature table (signatureInfo)
};

enum Verdict { VerdictUnknown = 0, VerdictAmbiguous, VerdictLikely };   // nothing fits well / several fit about the same / one stands out

struct Result {
    Features f;
    std::vector<Candidate> cands;  // best first; only those that score something
    std::string summary;           // "4FSK, 4800 Bd, +-648 / +-1944 Hz, 9.6 kHz wide, continuous"
    int verdict = VerdictUnknown;  // the calibrated reject option: Unknown means "I do not know", whatever the candidates say
    std::string headline;          // "4FSK, 4800 baud, 12.5 kHz wide. Most likely P25 Phase 1 or NXDN96, 71 % sure."  (English; the app builds its own)
    std::vector<std::string> priors;     // what the frequency database and the band plan said about this frequency
    double ms = 0;                 // time identify() took
};

struct Options {
    double rfHz = 0;               // the RF frequency of the samples' centre (the band of a candidate counts)
    double loHz = 0, hiHz = 0;     // the region, from the samples' centre; lo == hi: the strongest signal in the samples
    bool usePriors = true;         // the frequency database and the band plans move the scores a little (never a signal that does not fit)
    bool verify = true;            // run the detectors of the best few candidates (a few ms) and let what they find move the scores
    int verifyTop = 3;             // how many
};

Result identify(const cf32* x, size_t n, double rateHz, const Options& o = Options());

// How long to capture at a sample rate: 0.6 s, longer when the rate is low (sparse bursts need more time and it costs little), at most 2 s and
// 3 M samples. The app's capture and the test use the same rule.
inline double captureSeconds(double rateHz) { return rateHz <= 0 ? 0.6 : rateHz * 2.0 <= 3e6 ? 2.0 : 3e6 / rateHz < 0.6 ? 0.6 : 3e6 / rateHz; }

// Signals in a stretch of samples, as the spectrum shows them: each one's region (from the samples' centre). regionAt() is the one a click
// at clickOffHz (from the samples' centre) lands on; when none is there, a channel of fallbackWidthHz around the click. findRegions() lists
// them strongest first (at most maxN), the input of "Identify all signals".
struct Region { double loHz = 0, hiHz = 0, centreHz = 0, bwHz = 0, snrDb = 0, powerDb = 0; };
Region regionAt(const cf32* x, size_t n, double rateHz, double clickOffHz, double fallbackWidthHz = 25e3);
std::vector<Region> findRegions(const cf32* x, size_t n, double rateHz, size_t maxN = 16, double thresholdDb = 8);

// What a click on "Open in <mode>" and "Add to memory" use: the Memory of a candidate at rfHz (name, frequency, the mode id the Memory and the Scanner use, the
// measured width, and the options the measurement settles, such as the channel width of DTMB and ISDB-T)
Memory toMemory(const Candidate& c, const Features& f);
// The whole result as plain text (English), for the clipboard and for bug reports: the measurement, the ranked candidates, the evidence
std::string toText(const Result& r);

// What the calibration says: the table of modes the identifier knows (name, mode id, sub id, family) and how many there are
// source: 0 hand-written (sig_known.cpp), 1 measured on the mode's own test signal (sig_known_gen.cpp), 2 made from the mode list (ModeTuning) only
struct SigInfo { const char* name; const char* modeId; const char* subId; const char* family; int source; };
size_t signatureCount();
const SigInfo& signatureInfo(size_t i);

// Collects samples from the engine (push() from the analysis thread: a copy while armed, nothing otherwise) and identifies them on a
// thread of its own. start() again restarts it.
class Capture {
public:
    Capture();
    ~Capture();
    // the region [centreOffHz -+ widthHz / 2] from the samples' centre, rfHz the RF of that centre; seconds of signal (at most 4 M samples)
    void start(double rateHz, double centreOffHz, double widthHz, double rfHz, double seconds = 0.6);
    void cancel();
    bool armed() const { return armed_.load(std::memory_order_relaxed); }
    void push(const cf32* x, size_t n);
    int state() const;             // 0 idle, 1 collecting, 2 working it out, 3 done (result ready)
    double progress() const;       // 0..1 while collecting
    bool result(Result& out, uint64_t& seq) const;   // true when a result newer than seq is there (seq updated)
    // "Identify all signals": the same capture, but every signal in the span is found and identified (at most maxN)
    void startAll(double rateHz, double rfHz, double seconds = 0.6, size_t maxN = 12);
    struct Found { Region region; Result res; };
    bool resultAll(std::vector<Found>& out, uint64_t& seq) const;
    double regionCentre() const;   // the centreOffHz of the last start()
    // Signal Lab (Tools > Signal Lab): with keepSamples(true) the samples of the last single-signal capture are kept after it was
    // identified (at most 4 M samples, 32 MB), so the lab can analyse the very signal the user selected. Off by default.
    void keepSamples(bool on);
    struct Kept { std::vector<cf32> x; double rateHz = 0, centreOffHz = 0, widthHz = 0, rfHz = 0; uint64_t seq = 0; };
    bool keptSamples(Kept& out, uint64_t& seq) const;   // true when a capture newer than seq is there (seq updated)
    uint64_t keptSeq() const;                            // the number of the capture kept (0: none), cheap
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::atomic<bool> armed_{false};
};

} // namespace sigid
} // namespace dect2

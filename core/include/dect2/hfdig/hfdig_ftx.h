// FT8, FT4, FT2 and WSPR decoder of the HF digital receiver (hfdig_rx.h): gets the 8 kHz upper sideband audio, cuts it into the UTC
// time slots of each mode (FT8 15 s, FT4 7.5 s, FT2 3.75 s, WSPR 2 minutes from even minutes) and decodes every signal in the
// 200 .. 3000 Hz passband of each slot, several at once: sync search over the whole band and +-2.5 s (FT2 +-1 s) of time offset (DT),
// a fine time / frequency fit of each candidate, soft decisions, LDPC (FT8 / FT4 / FT2) or Fano (WSPR) decoding, then the decoded
// signals are subtracted from the audio and the slot is searched again (FT8 three passes, the others two).
// The protocols are those of WSJT-X (K1JT et al., GPL-3): FT8 8-GFSK 6.25 Hz, 79 symbols of 0.16 s; FT4 4-GFSK 20.83 Hz, 105 symbols of
// 48 ms; FT2 (WSJT-X Improved 3.1.0) the FT4 frame at twice the speed, 41.67 Hz, 24 ms symbols, 3.75 s periods; all with the 77-bit
// messages, CRC-14 and the (174,91) LDPC code. WSPR 4-FSK 1.4648 Hz, 162 symbols of 0.683 s, K=32 r=1/2 convolutional code, 50-bit
// messages (call, grid, power; types 2 and 3). See core/src/hfdig/hfdig_ftx_int.h for the sources.
// Time: the slots come from the system clock (UTC) unless a start time is given (a recording whose start is known), or from the
// signals themselves (slot search, when the start time of a recording is unknown). The DT of the decodes shows the clock error.
// Decoding runs on a thread of its own, so feedAudio() never waits for it.
//
// Test audio (hfdig_gen.h, makeFtxTestAudio): modeOpt[0] = 3 FT8, 4 FT4, 5 WSPR, 6 FT2: the stations of ftxTestStations() of that
// mode, every slot, from sample 0 = the start of a slot. modeOpt[1] = 1: mirrored (a lower sideband transmission). modeOpt[2] = 1:
// in step with the system clock (a live demonstration: the slots of the audio are the UTC slots).
// modeVal[0]: added to every station's audio frequency (Hz). The station at 0 dB has the amplitude 0.5 of the shell's convention.
// This header, hfdig_ftx*.cpp, hfdig_wspr.cpp and app/hfdig/hfdig_ftx_ui.cpp belong to these modes alone.
#pragma once
#include "hfdig_rx.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kFtxModes = 11;                // 0 FT8, 1 FT4, 2 FT2, 3 WSPR, 4 JT65, 5 JT9, 6 Q65, 7 FST4, 8 MSK144, 9 FST4W, 10 JT4
constexpr size_t kFtxKeep = 1000;            // decodes kept in the telemetry
const char* ftxModeName(int mode);           // "FT8", "FT4", "FT2", "WSPR", "JT65", "JT9", "Q65", "FST4", "MSK144", "FST4W", "JT4"
double ftxPeriod(int mode);                  // 15, 7.5, 3.75, 120 s, 60, 60, 60 (Q65 default), 60 (FST4), 15 (MSK144), 120 (FST4W)

struct FtxDecode {
    double slotUtc = 0;      // the start of the slot (UTC seconds since 1970; with a given start time, its clock)
    int mode = 0;
    float snrDb = 0;         // in 2500 Hz, as WSJT-X
    float dt = 0;            // s, the start against the nominal one (0.5 s into the slot, WSPR 1 s)
    float hz = 0;            // audio frequency of the lowest tone (mirrored: of tone 0, the highest)
    std::string msg;
    bool mirrored = false;   // heard upside down (a lower sideband transmission)
    bool cq = false;
    std::string call, grid;  // the calling station and its locator when the message has them
    int dbm = -1;            // WSPR: the power sent
    int pass = 0;            // 0 first pass, 1 .. after subtracting the stronger signals
    double dialHz = 0;       // the dial frequency of the slot it was heard on (whole-span decoding: several slots at once); 0: not known
    int how = 0;             // FT8 / FT4 / FT2: 0 decoded from this slot alone, 1 by adding the soft bits of the same signal in earlier slots, 2 with the calls of
                             // a conversation heard on the band taken as known (a-priori)
    bool early = false;      // decoded from the first part of the slot (the end of the signal still to come); replaced by the full decode of the slot
};

struct HfdigFtxTelemetry {
    uint64_t audioSamples = 0;
    std::vector<FtxDecode> decodes;           // oldest first, the last kFtxKeep
    uint64_t total = 0;                       // decodes since the last reset
    uint64_t perMode[kFtxModes] = {};
    uint64_t slots[kFtxModes] = {};           // slots decoded
    int lastSlotCount[kFtxModes] = {};        // decodes in the last slot
    double lastSlotUtc[kFtxModes] = {};
    float decodeMs[kFtxModes] = {};           // time the last slot took to decode
    bool enabled[kFtxModes] = {true, true, true, true, true, true, true, true, true, true, true};
    int sub[kFtxModes] = {};                  // JT65 0 A 1 B 2 C; JT4 0 A .. 6 G; Q65 0 A .. 4 E
    double period[kFtxModes] = {};            // T/R period in use, s
    int timeMode = 0;                         // 0 system clock, 1 the given start time, 2 slot search: looking, 3 slot search: found
    double nowUtc = 0;                        // the time of the newest audio (the slot clock)
    float clockErr = 0;                       // median DT of the recent decodes, s
    float clockTrim = 0;                      // the automatic correction in use: added to the computer clock, s (docs/EASE-3.8.md)
    float dtSpread = 0;                       // their spread (half the 16 .. 84 % range), s
    int clockN = 0;                           // decodes it is measured on
    bool clockWarn = false;                   // |clockErr| > 1 s
    int pending = 0;                          // slots waiting to be decoded
    uint64_t slotsDropped = 0;                // slots given up because decoding fell behind
};

class HfdigFtx : public HfdigDecoder {
public:
    HfdigFtx();
    ~HfdigFtx() override;
    void reset() override;                            // keeps the settings and the decodes
    void feedAudio(const float* x, size_t n) override;
    void telemetry(HfdigFtxTelemetry& out) const;    // any thread

    // settings: any thread
    void setEnabled(int mode, bool on);
    bool enabled(int mode) const;
    // JT65 sub-mode (0 A, 1 B, 2 C), Q65 sub-mode (0 A .. 4 E) and the T/R period of Q65 (15 / 30 / 60 / 120 / 300 s), FST4 and FST4W
    // (15 / 30 / 60 / 120 / 300 / 900 / 1800 s) and MSK144 (15 / 30 s); the others have one (any thread; sub < 0 or period <= 0 keep the old)
    void setModeOption(int mode, int sub, double periodSec);
    void setEarlyDecode(bool on);                    // FT8 / FT4: a first decode a few seconds before the slot ends (default on; a one-slot-at-a-time decoder only)
    bool earlyDecode() const;
    void setMirrored(bool on);                       // also look for upside-down signals (default on)
    bool mirrored() const;
    void clearDecodes();
    void setDecodes(std::vector<FtxDecode> d);       // a loaded results file (any thread; reset() keeps the decodes)
    // The UTC time of the first sample fed after the next reset (a recording with a known start); NaN: the system clock (default).
    void setStartTime(double utc);
    // The start time is unknown: find the slots from the signals (the first decodes set the clock).
    void setSlotSearch(bool on);
    // tests: a clock instead of the system clock (UTC seconds), and waiting until the slots queued so far are decoded
    void setWallClock(std::function<double()> fn);
    void waitIdle();
    // tests: decode the slots already complete in the audio fed so far, with what is there
    void flushSlots();

    // whole-span decoding (hfdig_wide.cpp): one decoder per dial slot; the decodes of the slot decoders go to the main one
    void setDialHz(double hz);                       // stamped on every decode (FtxDecode::dialHz)
    void setSpotShiftHz(double hz);                  // added to the audio frequency of a PSK Reporter spot (the slot's dial minus the radio's)
    void setForwardTo(HfdigFtx* main);               // every decoded slot is also added to main (addForeign), which is the one that lists it
    void addForeign(const std::vector<FtxDecode>& d, int mode, double slotUtc, float decodeMs);
    void copyClockFrom(HfdigFtx& o);                 // the same time source (start time, slot search, wall clock)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<HfdigFtx> makeFtxDecoder();

// The stations of the test audio of a mode (makeFtxTestAudio)
struct FtxTestStation { const char* msg; double hz; double relDb; double dt; };
std::vector<FtxTestStation> ftxTestStations(int mode);
// One transmission of a mode as audio (rate Hz): adds msg at audio frequency hz, amplitude amp, starting `startSec` into out
// (the nominal start of the slot plus DT). False when the message cannot be sent.
bool ftxGridToLatLon(const std::string& grid, double& lat, double& lon);   // the middle of a 4- or 6-character locator
// With the sub-mode (JT65 A B C, Q65 A .. E) and period of Q65 / FST4 / FST4W / MSK144 (0: the defaults); mirrored is not supported by those modes
bool ftxAddTransmissionEx(int mode, const std::string& msg, double hz, double amp, double rate, double startSec, bool mirrored, int sub, double period,
                          std::vector<float>& out);
bool ftxAddTransmission(int mode, const std::string& msg, double hz, double amp, double rate, double startSec, bool mirrored,
                        std::vector<float>& out);

class HfdigTestAudio;
// One station repeating `msg` every slot at `hz`. utcAlign: start where the system clock is in the slot (a live transmission).
std::unique_ptr<HfdigTestAudio> makeFtxTxAudio(int mode, const std::string& msg, double hz = 1500, bool mirrored = false, bool utcAlign = true);

} // namespace dect2

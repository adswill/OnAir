// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure logic of the multi-band monitor (no DSP, no dependencies): the band table, dial frequencies, coverage of a capture, best centre,
// needed rate, preference mask and the CPU guard. See hfdig_multiband.h.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {

constexpr int kMbBands = 11;                 // 160 80 60 40 30 20 17 15 12 10 6 m
constexpr int kMbModes = 4;                  // FT8 FT4 FT2 WSPR (kFtx mode numbers 0 1 2 3)
struct MbBand { const char* name; double loHz, hiHz; };
const MbBand& mbBand(int band);
// The dial frequency of the mode in the band (Hz), 0 when there is none (FT4 has no 160 m / 80 m... dial in the table, FT2 only on 20 m)
double mbDialHz(int band, int mode);

// The band (0 .. kMbBands-1) a frequency lies in (a little below the lower edge counts: WSPR dials sit 1.5 kHz under their sub-band); -1: none
int mbBandOf(double hz);

struct MbSel { int band; int mode; };        // band 0 .. kMbBands-1, mode 0 .. 3
inline bool operator==(const MbSel& a, const MbSel& b) { return a.band == b.band && a.mode == b.mode; }
// A selection as a bit mask (band * 4 + mode) for the preferences
uint64_t mbMask(const std::vector<MbSel>& s);
std::vector<MbSel> mbFromMask(uint64_t m);

constexpr double kMbAudioHz = 3300.0;        // the 3 kHz of a slot's audio must lie inside the span
constexpr double kMbUsableFrac = 0.80;       // the usable part of a capture (the filter edges of the radio are not)

struct MbCover {
    MbSel sel;
    double dialHz = 0;
    bool covered = false;
    double neededSpanHz = 0;                 // when not covered: the capture width that would hold it together with the centre (>= 2 |dial - centre| + 6.6 kHz) / usable
    double offsetHz = 0;                     // dial - centre
};
// Which selections the capture (centre, sample rate) holds. Selections without a dial frequency are listed as not covered, dialHz 0.
std::vector<MbCover> mbCoverage(double centreHz, double rateHz, const std::vector<MbSel>& sels);
// The centre that covers the most selections (ties: the lowest centre); `covered` gets the count. The frequency is the middle of the window.
double mbBestCentre(double rateHz, const std::vector<MbSel>& sels, int* covered = nullptr);
// The sample rate that would hold all the selections at one centre (what to tell the user: "needs 12 MHz")
double mbNeededRate(const std::vector<MbSel>& sels);

// CPU guard: feed the measured processing time against the audio time of the same block; when the load stays above the limit the guard
// asks for one selection (the least useful) to be dropped; when it stays low it allows trying again.
class MbCpuGuard {
public:
    explicit MbCpuGuard(double limit = 0.85, int blocksToAct = 8) : limit_(limit), n_(blocksToAct) {}
    // true: drop one more (the load has been over the limit for blocksToAct blocks in a row)
    bool update(double busySec, double blockSec);
    double load() const { return load_; }
    bool tooSlow() const { return slow_; }
    void reset() { load_ = 0; over_ = 0; slow_ = false; }
private:
    double limit_, load_ = 0;
    int n_, over_ = 0;
    bool slow_ = false;
};

} // namespace dect2

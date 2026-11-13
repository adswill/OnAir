// Narrow-band digital mode guess (4.0): "what is this signal" from the numbers a spectrum / waterfall measurement gives, so the user clicks a trace and
// the decoder opens already set (mode, baud, shift). Pure code. Input: occupied bandwidth, number of distinct tones, their spacing, symbol rate,
// whether the carrier goes completely off between symbols (on-off keying), and the period of a transmit slot when the signal repeats on a clock.
// Output: candidates, best first, with a 0..1 score and the settings to apply. Only a guess: the decoder's own lock decides.
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace dect2 {

struct HamSigFeatures {
    double bwHz = 0;
    int tones = 1;
    double toneSpacingHz = 0;
    double symbolRateBd = 0;     // 0 = not measured
    bool onOff = false;
    double slotS = 0;            // 0 = continuous
};
struct HamModeGuess { std::string mode, settings; double score = 0; };

inline std::vector<HamModeGuess> hamGuessMode(const HamSigFeatures& f) {
    std::vector<HamModeGuess> v;
    auto tooClose = [](double a, double b, double tol) { return std::fabs(a - b) <= tol * b; };
    auto add = [&](const char* m, const std::string& s, double sc) { if (sc > 0.05) v.push_back({m, s, std::min(1.0, sc)}); };
    char b[96];
    // CW: keyed carrier, narrow
    if (f.onOff && f.tones <= 1 && f.bwHz <= 400) add("CW", f.symbolRateBd > 0 ? (std::snprintf(b, sizeof b, "~%.0f WPM", f.symbolRateBd * 1.2), b) : "", 0.9 - f.bwHz / 2000.0);
    // FT8 / FT4 / WSPR / JT65 / JS8: slotted
    if (f.slotS > 0 && !f.onOff) {
        if (tooClose(f.slotS, 15, 0.08) && f.tones == 8 && tooClose(f.toneSpacingHz, 6.25, 0.1)) add("FT8", "15 s slots", 0.95);
        if (tooClose(f.slotS, 15, 0.08) && f.tones == 8 && tooClose(f.toneSpacingHz, 6.25, 0.1) && f.symbolRateBd > 0 && tooClose(f.symbolRateBd, 6.25, 0.1)) add("JS8", "15 s slots, JS8 normal", 0.4);
        if (tooClose(f.slotS, 7.5, 0.08) && f.tones == 4 && tooClose(f.toneSpacingHz, 20.83, 0.1)) add("FT4", "7.5 s slots", 0.95);
        if (tooClose(f.slotS, 120, 0.04) && f.tones == 4 && tooClose(f.toneSpacingHz, 1.4648, 0.15)) add("WSPR", "2 min slots", 0.95);
        if (tooClose(f.slotS, 60, 0.04) && f.tones == 65 && f.bwHz < 300) add("JT65", "1 min slots", 0.85);
    }
    // PSK31 family: one carrier, no on-off, narrow
    if (!f.onOff && f.tones <= 1 && f.slotS == 0) {
        if (tooClose(f.symbolRateBd, 31.25, 0.06) || (f.symbolRateBd == 0 && f.bwHz >= 40 && f.bwHz <= 80)) add("PSK31", "BPSK 31.25 Bd", 0.9 - std::fabs(f.bwHz - 62) / 200.0);
        if (tooClose(f.symbolRateBd, 62.5, 0.06)) add("PSK63", "BPSK 62.5 Bd", 0.9);
        if (tooClose(f.symbolRateBd, 125, 0.06)) add("PSK125", "BPSK 125 Bd", 0.9);
    }
    // RTTY: two tones at a standard shift and a standard rate
    if (f.tones == 2 && !f.onOff) {
        const double sh[] = {170, 200, 425, 450, 850};
        double bestS = 0; for (double s : sh) if (tooClose(f.toneSpacingHz, s, 0.08)) bestS = s;
        const double br[] = {45.45, 50, 56.88, 75, 100, 110, 150, 300};
        double bestB = 0; for (double r : br) if (f.symbolRateBd > 0 && tooClose(f.symbolRateBd, r, 0.05)) bestB = r;
        if (bestS > 0) { std::snprintf(b, sizeof b, "shift %.0f Hz%s%.2f Bd", bestS, bestB > 0 ? ", " : ", rate ", bestB > 0 ? bestB : 45.45); add("RTTY", b, bestB > 0 ? 0.95 : 0.7); }
    }
    // MFSK family: many tones (Olivia, MFSK16 ...)
    if (!f.onOff && f.slotS == 0 && f.tones >= 4) {
        if (f.tones == 16 && tooClose(f.symbolRateBd, 15.625, 0.06) && f.bwHz < 400) add("MFSK16", "16 tones, 15.625 Bd", 0.9);
        const int ol[][2] = {{4, 250}, {8, 250}, {8, 500}, {16, 500}, {16, 1000}, {32, 1000}, {32, 2000}, {8, 1000}, {4, 500}, {4, 125}, {8, 125}, {16, 250}};
        for (const auto& o : ol) if (f.tones == o[0] && tooClose(f.bwHz, o[1], 0.2)) { std::snprintf(b, sizeof b, "Olivia %d/%d", o[0], o[1]); add("Olivia", b, 0.75); break; }
        if (f.tones == 4 && f.bwHz < 120 && f.symbolRateBd == 0) add("DominoEX / THOR", "", 0.3);
    }
    std::stable_sort(v.begin(), v.end(), [](const HamModeGuess& a, const HamModeGuess& c) { return a.score > c.score; });
    return v;
}

} // namespace dect2

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-S/S2 rain-fade and margin log (3.9, docs/SMART5-tv-3.9.md item 13). Every second the demodulator's Es/N0 and MODCOD go in;
// out come the margin above the quasi-error-free threshold of that MODCOD, the clear-sky level (learned), each fade event (start,
// length, depth) and every ACM / VCM MODCOD change.
// Thresholds: DVB-S2 ideal Es/N0 at QEF for normal FECFRAMEs, EN 302 307-1 table 13; DVB-S Eb/N0 after Viterbi for QEF,
// EN 300 421 table 4 (4.5 / 5.0 / 5.5 / 6.0 / 6.4 dB), converted to Es/N0 with the RS(204,188) rate.
#pragma once
#include <deque>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

// S2 MODCOD numbers 1 - 28 (PL header): 1 - 11 QPSK 1/4 ... 9/10, 12 - 17 8PSK, 18 - 23 16APSK, 24 - 28 32APSK. Returns NaN if unknown.
double s2QefEsN0(int modcod);
// DVB-S: code rate as numerator/denominator (1/2, 2/3, 3/4, 5/6, 7/8). Returns NaN if unknown.
double s1QefEsN0(int num, int den);
const char* s2ModcodName(int modcod);

struct FadeEvent { double start = 0, length = 0, depthDb = 0, minMarginDb = 0; bool lostLock = false; };
struct ModcodChange { double t = 0; int from = 0, to = 0; };

struct SatFadeParams { double fadeDb = 1.0; double minSec = 5; double clearWindowSec = 1800; };

class SatFadeLog {
public:
    explicit SatFadeLog(SatFadeParams p = {}) : p_(p) {}
    // qefDb: the threshold for the present MODCOD (s2QefEsN0 / s1QefEsN0); locked false while the demodulator has no lock
    void sample(double t, double esn0Db, int modcod, double qefDb, bool locked = true);
    double clearSkyDb() const { return clear_; }
    double marginDb() const { return margin_; }
    double fadeDb() const { return clear_ - last_; }
    bool fading() const { return inFade_; }
    const std::vector<FadeEvent>& fades() const { return fades_; }
    const std::vector<ModcodChange>& modcodChanges() const { return changes_; }
    std::string text() const;
private:
    SatFadeParams p_;
    std::deque<std::pair<double, double>> hist_;   // (t, Es/N0) for the clear-sky estimate
    double clear_ = 0, last_ = 0, margin_ = 0;
    int modcod_ = -1;
    bool inFade_ = false;
    FadeEvent cur_;
    std::vector<FadeEvent> fades_;
    std::vector<ModcodChange> changes_;
};

} // namespace smart5
} // namespace dect2

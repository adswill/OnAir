// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/audio/audio_eq.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {

const float kEqBandHz[kEqBands] = {31.5f, 63.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f};

static constexpr double kPi = 3.14159265358979323846;

bool EqParams::flat() const {
    if (std::fabs(bass) >= 0.05f || std::fabs(treble) >= 0.05f) return false;
    for (float g : band) if (std::fabs(g) >= 0.05f) return false;
    return true;
}

void EqParams::clamp() {
    auto c = [](float v) { return std::isfinite(v) ? std::max(-kEqMaxDb, std::min(kEqMaxDb, v)) : 0.f; };
    bass = c(bass); treble = c(treble);
    for (float& g : band) g = c(g);
}

const char* eqPresetName(EqPreset p) {
    switch (p) {
    case EqPreset::Flat: return "Flat";
    case EqPreset::Voice: return "Voice";
    case EqPreset::Music: return "Music";
    case EqPreset::Hearing: return "Hearing-friendly";
    case EqPreset::BassBoost: return "Bass boost";
    case EqPreset::Bright: return "Bright";
    default: return "";
    }
}

EqParams eqPresetParams(EqPreset p) {
    EqParams e;
    auto set = [&](float bass, float treble, std::initializer_list<float> b) { e.bass = bass; e.treble = treble; int i = 0; for (float g : b) e.band[(size_t)i++] = g; };
    switch (p) {
    case EqPreset::Voice:     set(-3, 0,  {-12, -9, -4, -1, 0, 2, 4, 3, 0, -3}); break;    // no rumble, presence up
    case EqPreset::Music:     set(3, 3,   {3, 3, 2, 0, -1, -1, 0, 2, 3, 3}); break;        // a gentle smile
    case EqPreset::Hearing:   set(-2, 4,  {-6, -4, -2, 0, 1, 2, 4, 6, 6, 3}); break;       // lifts the 2 to 8 kHz consonants
    case EqPreset::BassBoost: set(6, 0,   {5, 5, 4, 2, 0, 0, 0, 0, 0, 0}); break;
    case EqPreset::Bright:    set(0, 5,   {0, 0, 0, 0, 0, 1, 2, 3, 4, 4}); break;
    default: break;
    }
    return e;
}

EqPreset eqPresetOf(const EqParams& p) {
    for (int i = 0; i < (int)EqPreset::Count; i++) {
        const EqParams q = eqPresetParams((EqPreset)i);
        bool same = std::fabs(p.bass - q.bass) <= 0.5f && std::fabs(p.treble - q.treble) <= 0.5f;
        for (size_t k = 0; k < (size_t)kEqBands && same; k++) same = std::fabs(p.band[k] - q.band[k]) <= 0.5f;
        if (same) return (EqPreset)i;
    }
    return EqPreset::Count;
}

namespace {
struct Coef { double b0, b1, b2, a1, a2; };

Coef norm(double b0, double b1, double b2, double a0, double a1, double a2) { return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0}; }

Coef peaking(double f, double q, double gDb, double rate) {
    const double A = std::pow(10.0, gDb / 40.0), w = 2 * kPi * f / rate, c = std::cos(w), al = std::sin(w) / (2 * q);
    return norm(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}
Coef shelf(bool high, double f, double gDb, double rate) {   // slope S = 1
    const double A = std::pow(10.0, gDb / 40.0), w = 2 * kPi * f / rate, c = std::cos(w), s = std::sin(w);
    const double al = s / 2 * std::sqrt((A + 1 / A) * (1 / 1.0 - 1) + 2), sq = 2 * std::sqrt(A) * al;
    if (!high) return norm(A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq),
                           (A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq);
    return norm(A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq),
                (A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq);
}
}  // namespace

void ToneEq::configure(const EqParams& pIn, float rate) {
    EqParams p = pIn; p.clamp();
    if (have_ && rate == rate_ && p == cur_) return;
    have_ = true; rate_ = rate; cur_ = p;
    const double r = rate, top = 0.45 * r;
    auto put = [&](int i, bool on, Coef c) {
        used_[i] = on;
        if (!on) { q_[i] = Bq(); return; }
        q_[i].b0 = c.b0; q_[i].b1 = c.b1; q_[i].b2 = c.b2; q_[i].a1 = c.a1; q_[i].a2 = c.a2;   // the state stays: a gain change does not click
    };
    put(0, std::fabs(p.bass) >= 0.05f && 120.0 < top, shelf(false, 120.0, p.bass, r));
    put(1, std::fabs(p.treble) >= 0.05f && 6000.0 < top, shelf(true, 6000.0, p.treble, r));
    for (int i = 0; i < kEqBands; i++) {
        const double f = kEqBandHz[i];
        put(2 + i, std::fabs(p.band[(size_t)i]) >= 0.05f && f < top, peaking(f, 1.41, p.band[(size_t)i], r));
    }
}

float ToneEq::tick(float x) {
    double v = x;
    for (int i = 0; i < kN; i++) {
        if (!used_[i]) continue;
        Bq& b = q_[i];
        const double y = b.b0 * v + b.z1;
        b.z1 = b.b1 * v - b.a1 * y + b.z2;
        b.z2 = b.b2 * v - b.a2 * y;
        v = y;
    }
    return (float)v;
}

void ToneEq::reset() { for (Bq& b : q_) b.z1 = b.z2 = 0; }

double ToneEq::responseDb(double hz) const {
    if (!have_) return 0;
    const double w = 2 * kPi * hz / rate_;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2 * w);
    std::complex<double> h = 1;
    for (int i = 0; i < kN; i++) {
        if (!used_[i]) continue;
        const Bq& b = q_[i];
        h *= (b.b0 + b.b1 * z1 + b.b2 * z2) / (1.0 + b.a1 * z1 + b.a2 * z2);
    }
    return 20 * std::log10(std::max(1e-12, std::abs(h)));
}

}  // namespace dect2

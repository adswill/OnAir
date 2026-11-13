// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Audio control chain (audio_control.h, docs/AUDIO.md). Own code from textbook algorithms: RBJ "Audio EQ Cookbook" biquads, Goertzel tone
// detection, NLMS adaptive line enhancer (B. Widrow et al.), FIR Hilbert frequency shifter, Golay(23,12) DCS words.
#include "dect2/audio/audio_control.h"
#include "dect2/audio/audio_cleanup.h"
#include "dect2/tones/tones_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace dect2 {
namespace actl {
namespace {
constexpr float kPi = 3.14159265358979f;
inline float dbToLin(float db) { return std::pow(10.f, db * 0.05f); }
inline float linToDb(float x) { return 20.f * std::log10(std::max(x, 1e-9f)); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

// ---- biquads ---------------------------------------------------------------------------------------------------------------------------
static BiquadCoef norm(float b0, float b1, float b2, float a0, float a1, float a2) {
    BiquadCoef c; const float i = 1.f / a0;
    c.b0 = b0 * i; c.b1 = b1 * i; c.b2 = b2 * i; c.a1 = a1 * i; c.a2 = a2 * i;
    return c;
}
BiquadCoef bqLowpass(float hz, float q, float rate) {
    const float w = 2 * kPi * clampf(hz, 10.f, 0.47f * rate) / rate, cs = std::cos(w), al = std::sin(w) / (2 * q);
    return norm((1 - cs) / 2, 1 - cs, (1 - cs) / 2, 1 + al, -2 * cs, 1 - al);
}
BiquadCoef bqHighpass(float hz, float q, float rate) {
    const float w = 2 * kPi * clampf(hz, 5.f, 0.47f * rate) / rate, cs = std::cos(w), al = std::sin(w) / (2 * q);
    return norm((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + al, -2 * cs, 1 - al);
}
BiquadCoef bqNotch(float hz, float q, float rate) {
    const float w = 2 * kPi * clampf(hz, 20.f, 0.47f * rate) / rate, cs = std::cos(w), al = std::sin(w) / (2 * std::max(q, 0.3f));
    return norm(1, -2 * cs, 1, 1 + al, -2 * cs, 1 - al);
}
BiquadCoef bqPeaking(float hz, float q, float gainDb, float rate) {
    const float A = std::pow(10.f, gainDb / 40.f), w = 2 * kPi * clampf(hz, 20.f, 0.47f * rate) / rate, cs = std::cos(w), al = std::sin(w) / (2 * q);
    return norm(1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
}
BiquadCoef bqLowShelf(float hz, float gainDb, float rate) {
    const float A = std::pow(10.f, gainDb / 40.f), w = 2 * kPi * clampf(hz, 20.f, 0.47f * rate) / rate, cs = std::cos(w), sn = std::sin(w);
    const float tsa = 2 * std::sqrt(A) * (sn * 0.70710678f);   // 2 sqrt(A) alpha, alpha = sn/sqrt(2)
    return norm(A * ((A + 1) - (A - 1) * cs + tsa), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - tsa),
                (A + 1) + (A - 1) * cs + tsa, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - tsa);
}
BiquadCoef bqHighShelf(float hz, float gainDb, float rate) {
    const float A = std::pow(10.f, gainDb / 40.f), w = 2 * kPi * clampf(hz, 20.f, 0.47f * rate) / rate, cs = std::cos(w), sn = std::sin(w);
    const float tsa = 2 * std::sqrt(A) * (sn * 0.70710678f);
    return norm(A * ((A + 1) + (A - 1) * cs + tsa), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - tsa),
                (A + 1) - (A - 1) * cs + tsa, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - tsa);
}
float bqMagnitudeDb(const BiquadCoef& c, float hz, float rate) {
    const float w = 2 * kPi * hz / rate;
    const float c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    const float nr = c.b0 + c.b1 * c1 + c.b2 * c2, ni = -(c.b1 * s1 + c.b2 * s2);
    const float dr = 1 + c.a1 * c1 + c.a2 * c2, di = -(c.a1 * s1 + c.a2 * s2);
    return 10.f * std::log10(std::max((nr * nr + ni * ni) / std::max(dr * dr + di * di, 1e-20f), 1e-20f));
}

// ---- EQ --------------------------------------------------------------------------------------------------------------------------------
const float kEqHz[kEqBands] = {120.f, 400.f, 1000.f, 2500.f, 6000.f};
const char* eqPresetName(EqPreset p) {
    static const char* const n[] = {"Flat", "Voice", "Music", "CW", "Custom"};
    return n[(int)p < 5 ? (int)p : 4];
}
void eqPresetGains(EqPreset p, float g[kEqBands]) {
    static const float t[4][kEqBands] = {{0, 0, 0, 0, 0}, {-9, -2, 2, 4, -3}, {4, 1, -1, 1, 4}, {-12, -6, 6, 0, -9}};
    if ((int)p < 4) for (int i = 0; i < kEqBands; i++) g[i] = t[(int)p][i];
}
void eqDesign(const float g[kEqBands], float rate, BiquadCoef out[kEqBands]) {
    out[0] = bqLowShelf(kEqHz[0], g[0], rate);
    for (int i = 1; i < 4; i++) out[i] = bqPeaking(kEqHz[i], 1.0f, g[i], rate);
    out[4] = bqHighShelf(kEqHz[4], g[4], rate);
}
float eqResponseDb(const float g[kEqBands], float hz, float rate) {
    BiquadCoef c[kEqBands]; eqDesign(g, rate, c);
    float d = 0; for (int i = 0; i < kEqBands; i++) d += bqMagnitudeDb(c[i], hz, rate);
    return d;
}

// ---- squelch ---------------------------------------------------------------------------------------------------------------------------
bool SquelchCore::update(float m, float dtMs) {
    if (closeDb > openDb) closeDb = openDb;
    if (!open) {
        if (m >= openDb) { open = true; hangLeftMs = hangMs; }
    } else if (m >= closeDb) {
        hangLeftMs = hangMs;                      // still strong enough: the hang starts over
    } else {
        hangLeftMs -= dtMs;
        if (hangLeftMs <= 0) { open = false; hangLeftMs = 0; }
    }
    return open;
}

const float kCtcssHz[50] = {67.0f, 69.3f, 71.9f, 74.4f, 77.0f, 79.7f, 82.5f, 85.4f, 88.5f, 91.5f, 94.8f, 97.4f, 100.0f, 103.5f, 107.2f, 110.9f, 114.8f,
    118.8f, 123.0f, 127.3f, 131.8f, 136.5f, 141.3f, 146.2f, 151.4f, 156.7f, 159.8f, 162.2f, 165.5f, 167.9f, 171.3f, 173.8f, 177.3f, 179.9f, 183.5f,
    186.2f, 189.9f, 192.8f, 196.6f, 199.5f, 203.5f, 206.5f, 210.7f, 218.1f, 225.7f, 229.1f, 233.6f, 241.8f, 250.3f, 254.1f};

void ToneDetector::prepare(float rate, float toneHz) {
    rate_ = rate; toneHz_ = toneHz; block_ = (int)(rate * 0.2f); count_ = 0; present_ = false;
    const float f[3] = {toneHz, std::max(20.f, toneHz - 12.f), toneHz + 12.f};
    for (int i = 0; i < 3; i++) { c_[i] = 2 * std::cos(2 * kPi * f[i] / rate); s1_[i] = s2_[i] = 0; }
}
bool ToneDetector::process(const float* x, int n) {
    if (toneHz_ <= 0) return false;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) { const float s = x[i] + c_[k] * s1_[k] - s2_[k]; s2_[k] = s1_[k]; s1_[k] = s; }
        if (++count_ >= block_) {
            float p[3];
            for (int k = 0; k < 3; k++) { p[k] = s1_[k] * s1_[k] + s2_[k] * s2_[k] - c_[k] * s1_[k] * s2_[k]; s1_[k] = s2_[k] = 0; }
            const float amp = 2.f * std::sqrt(std::max(p[0], 0.f)) / (float)block_;
            present_ = amp > 0.004f && p[0] > 6.f * 0.5f * (p[1] + p[2]);
            count_ = 0;
        }
    }
    return present_;
}

// ---- DCS -------------------------------------------------------------------------------------------------------------------------------
uint32_t dcsWord(int octalCode) {
    // One table for the whole program (#479): this used to build its own word with the marker bits and the Golay polynomial the wrong
    // way round, so a transmitted D023N was read as D411N by a real radio. tones::dcsWord matches the radios (023 = 0x763813).
    return tones::dcsWord(octalCode);
}
static int pop23(uint32_t v) { return __builtin_popcount(v & 0x7FFFFF); }
void DcsDetector::prepare(float rate, int octalCode) {
    rate_ = rate; lp1_ = lp2_ = mean_ = 0; phase_ = 0; quarter_ = 0; holdLeft_ = 0; bitCount_ = 0; present_ = false;
    for (int i = 0; i < 4; i++) { hist_[i] = 0; lastHit_[i] = -1000; run_[i] = 0; }
    inc_ = 134.4 / rate; holdMax_ = (int)(rate * 0.5f);
    lpA_ = 1.f - std::exp(-2 * kPi * 200.f / rate); meanA_ = 1.f - std::exp(-2 * kPi * 3.f / rate);
    const uint32_t w = dcsWord(octalCode);
    for (int r = 0; r < 23; r++) {
        const uint32_t rot = ((w << r) | (w >> (23 - r))) & 0x7FFFFF;
        pat_[r] = rot; pat_[23 + r] = ~rot & 0x7FFFFF;
    }
}
bool DcsDetector::process(const float* x, int n) {
    for (int i = 0; i < n; i++) {
        lp1_ += lpA_ * (x[i] - lp1_); lp2_ += lpA_ * (lp1_ - lp2_);
        mean_ += meanA_ * (lp2_ - mean_);
        phase_ += inc_;
        if (phase_ >= 1.0) phase_ -= 1.0;
        const int qn = std::min(3, (int)(phase_ * 4));
        if (qn != quarter_) {
            quarter_ = qn;   // sampler qn takes its bit now: one bit per bit period, four phases of the unknown bit clock
            hist_[qn] = ((hist_[qn] >> 1) | ((lp2_ > mean_ ? 1u : 0u) << 22)) & 0x7FFFFF;
            int best = 99;
            for (int k = 0; k < 46; k++) best = std::min(best, pop23(hist_[qn] ^ pat_[k]));
            if (best <= 2) {   // every rotation of the word matches at every bit: a real signal gives one hit per bit for as long as it lasts
                run_[qn] = (bitCount_ - lastHit_[qn] == 1) ? run_[qn] + 1 : 1; lastHit_[qn] = bitCount_;
                if (run_[qn] >= 24) holdLeft_ = holdMax_;
            }
            if (qn == 0) bitCount_++;
        }
        if (holdLeft_ > 0) holdLeft_--;
    }
    present_ = holdLeft_ > 0;
    return present_;
}

// ---- AGC / meter -----------------------------------------------------------------------------------------------------------------------
void Agc::prepare(float rate) { rate_ = rate; env_ = 0; gain_ = 1; }
void Agc::setMode(AgcMode m, float targetDb) {
    mode_ = m; target_ = dbToLin(clampf(targetDb, -40.f, -2.f));
    const float tAtt = m == AgcMode::Fast ? 0.002f : 0.02f, tRel = m == AgcMode::Fast ? 0.12f : 1.5f;
    att_ = 1.f - std::exp(-1.f / (tAtt * rate_)); rel_ = 1.f - std::exp(-1.f / (tRel * rate_));
}
void Agc::process(float* st, int frames) {
    if (mode_ == AgcMode::Off) return;
    const float eAtt = 1.f - std::exp(-1.f / (0.0005f * rate_)), eRel = 1.f - std::exp(-1.f / (0.03f * rate_));
    for (int i = 0; i < frames; i++) {
        const float l = std::fabs(st[2 * i]), r = std::fabs(st[2 * i + 1]), lv = l > r ? l : r;
        env_ += (lv > env_ ? eAtt : eRel) * (lv - env_);
        float want = gain_;
        if (env_ > floor_) want = clampf(target_ / env_, 0.03f, maxGain_);   // below the floor (noise between transmissions): hold, do not boost
        gain_ += (want < gain_ ? att_ : rel_) * (want - gain_);
        st[2 * i] *= gain_; st[2 * i + 1] *= gain_;
    }
}

void Meter::process(const float* st, int frames) {
    float pk = 0, e = 0; uint32_t cl = 0;
    for (int i = 0; i < 2 * frames; i++) { const float a = std::fabs(st[i]); pk = a > pk ? a : pk; e += st[i] * st[i]; cl += a >= 0.999f; }
    peak = pk; rms2 = e / (float)std::max(1, 2 * frames); clipped += cl;
}
float Meter::peakDb() const { return linToDb(peak); }
float Meter::rmsDb() const { return 10.f * std::log10(std::max(rms2, 1e-18f)); }

void spectrumDb(const float* x, int n, float* out) {
    n = std::min(4096, n);
    static thread_local float re[4096], im[4096];
    for (int i = 0; i < n; i++) { re[i] = x[i] * (0.5f - 0.5f * std::cos(2 * kPi * (i + 0.5f) / n)); im[i] = 0; }
    for (int i = 1, j = 0; i < n; i++) {   // bit reversal
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(re[i], re[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2 * kPi / len, wr = std::cos(ang), wi = std::sin(ang);
        for (int s = 0; s < n; s += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                const int a = s + k, b = a + len / 2;
                const float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
                const float nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
    const float sc = 4.f / (float)n;   // a full-scale sine reads about 0 dBFS (Hann gain 0.5)
    for (int i = 0; i < n / 2; i++) out[i] = linToDb(std::sqrt(re[i] * re[i] + im[i] * im[i]) * sc);
}

// ---- settings --------------------------------------------------------------------------------------------------------------------------
AudioControlSettings audioControlDefaults(int profile) {
    AudioControlSettings s;
    // enum AudioProfile { Off, Generic, AM, NFM, WFM, SSB, CW, DigitalVoice }: the audio bandwidth that suits it (the chain itself stays off)
    switch (profile) {
        case 2: s.hp = s.lp = true; s.hpHz = 100; s.lpHz = 5000; break;
        case 3: s.hp = s.lp = true; s.hpHz = 300; s.lpHz = 3000; break;
        case 5: s.hp = s.lp = true; s.hpHz = 300; s.lpHz = 2700; s.hpSlope = s.lpSlope = 24; break;
        case 6: s.hp = s.lp = true; s.hpHz = 400; s.lpHz = 900; s.hpSlope = s.lpSlope = 24; s.eqPreset = EqPreset::CW; eqPresetGains(EqPreset::CW, s.eqDb); break;
        case 7: s.hp = s.lp = true; s.hpHz = 300; s.lpHz = 3400; break;
        default: break;
    }
    return s;
}

static const char* kStereoN[] = {"stereo", "mono", "swap", "left", "right"};
std::string audioControlToString(const AudioControlSettings& s) {
    char b[1024];
    snprintf(b, sizeof b,
             "on=%d;st=%d;hp=%d;hpf=%g;hps=%d;lp=%d;lpf=%g;lps=%d;eq=%d;eqp=%d;eq0=%g;eq1=%g;eq2=%g;eq3=%g;eq4=%g;nt=%d;ntf=%g;ntq=%g;an=%d;de=%d;agc=%d;agt=%g;"
             "sh=%g;cw=%g;sk=%d;so=%g;sy=%g;sg=%g;stl=%g;ct=%g;dc=%d;ro=%d;vol=%g;mu=%d;lat=%d;buf=%d",
             s.on, (int)s.stereo, s.hp, s.hpHz, s.hpSlope, s.lp, s.lpHz, s.lpSlope, s.eq, (int)s.eqPreset, s.eqDb[0], s.eqDb[1], s.eqDb[2], s.eqDb[3], s.eqDb[4],
             s.notch, s.notchHz, s.notchQ, s.autoNotch, (int)s.deemph, (int)s.agc, s.agcTargetDb, s.shiftHz, s.cwToneHz, (int)s.sqKind, s.sqOpenDb, s.sqHystDb,
             s.sqHangMs, s.sqTailMs, s.ctcssHz, s.dcsCode, s.recordOnlyOpen, s.volume, s.muted, s.latencyMs, s.bufferMs);
    std::string out = b;
    if (!s.device.empty()) { std::string d = s.device; for (char& c : d) if (c == ';' || c == '=') c = '_'; out += ";dev=" + d; }
    return out;
}
AudioControlSettings audioControlFromString(const std::string& str, const AudioControlSettings& base) {
    AudioControlSettings s = base;
    std::istringstream is(str);
    std::string tok;
    while (std::getline(is, tok, ';')) {
        const size_t e = tok.find('=');
        if (e == std::string::npos) continue;
        const std::string k = tok.substr(0, e), v = tok.substr(e + 1);
        const float f = (float)atof(v.c_str());
        const int i = atoi(v.c_str());
        auto en = [&](int x, int hi) { return std::max(0, std::min(hi, x)); };
        if (k == "on") s.on = i; else if (k == "st") s.stereo = (StereoMode)en(i, 4);
        else if (k == "hp") s.hp = i; else if (k == "hpf") s.hpHz = clampf(f, 20.f, 8000.f); else if (k == "hps") s.hpSlope = i >= 24 ? 24 : 12;
        else if (k == "lp") s.lp = i; else if (k == "lpf") s.lpHz = clampf(f, 200.f, 20000.f); else if (k == "lps") s.lpSlope = i >= 24 ? 24 : 12;
        else if (k == "eq") s.eq = i; else if (k == "eqp") s.eqPreset = (EqPreset)en(i, 4);
        else if (k.size() == 3 && k.compare(0, 2, "eq") == 0 && k[2] >= '0' && k[2] <= '4') s.eqDb[k[2] - '0'] = clampf(f, -18.f, 18.f);
        else if (k == "nt") s.notch = i; else if (k == "ntf") s.notchHz = clampf(f, 50.f, 12000.f); else if (k == "ntq") s.notchQ = clampf(f, 1.f, 60.f);
        else if (k == "an") s.autoNotch = i; else if (k == "de") s.deemph = (Deemph)en(i, 2);
        else if (k == "agc") s.agc = (AgcMode)en(i, 2); else if (k == "agt") s.agcTargetDb = clampf(f, -40.f, -2.f);
        else if (k == "sh") s.shiftHz = clampf(f, -1500.f, 1500.f); else if (k == "cw") s.cwToneHz = f <= 0 ? 0.f : clampf(f, 300.f, 1500.f);
        else if (k == "sk") s.sqKind = (SquelchKind)en(i, 4); else if (k == "so") s.sqOpenDb = clampf(f, -100.f, 40.f);
        else if (k == "sy") s.sqHystDb = clampf(f, 0.f, 30.f); else if (k == "sg") s.sqHangMs = clampf(f, 0.f, 3000.f);
        else if (k == "stl") s.sqTailMs = clampf(f, 0.f, 400.f); else if (k == "ct") s.ctcssHz = f;
        else if (k == "dc") s.dcsCode = i; else if (k == "ro") s.recordOnlyOpen = i;
        else if (k == "vol") s.volume = clampf(f, 0.f, 2.f); else if (k == "mu") s.muted = i;
        else if (k == "lat") s.latencyMs = std::max(20, std::min(2000, i)); else if (k == "buf") s.bufferMs = std::max(0, std::min(2000, i));
        else if (k == "dev") s.device = v;
    }
    (void)kStereoN;
    return s;
}
std::string audioPickDevice(const std::vector<std::string>& present, const std::string& wanted) {
    if (wanted.empty()) return "";
    for (const auto& d : present) if (d == wanted) return wanted;
    return "";   // gone (unplugged, renamed): the system default
}

}  // namespace actl

// ---- config ----------------------------------------------------------------------------------------------------------------------------
AudioControlConfig& AudioControlConfig::global() { static AudioControlConfig c; return c; }
void AudioControlConfig::set(const actl::AudioControlSettings& s) {
    while (lock_.test_and_set(std::memory_order_acquire)) {}
    s_ = s; recordOnlyOpen.store(s.recordOnlyOpen, std::memory_order_relaxed); gen_.fetch_add(1, std::memory_order_release);
    lock_.clear(std::memory_order_release);
}
actl::AudioControlSettings AudioControlConfig::get() const {
    while (lock_.test_and_set(std::memory_order_acquire)) {}
    actl::AudioControlSettings s = s_;
    lock_.clear(std::memory_order_release);
    return s;
}

// ---- the chain -------------------------------------------------------------------------------------------------------------------------
using namespace actl;

struct AudioControl::Impl {
    int rate = 48000;
    uint32_t gen = 0xFFFFFFFFu;
    AudioControlSettings s;
    // filters (two sections of 12 dB/oct for 24)
    BiquadCoef hpC[2], lpC[2], ntC, eqC[kEqBands], toneHp;
    int hpN = 0, lpN = 0;
    bool ntOn = false, eqOn = false, toneHpOn = false;
    BiquadState hpS[2][2], lpS[2][2], ntS[2], eqS[2][kEqBands], toneS[2];
    float deA = 0, deY[2] = {0, 0};
    // auto notch (NLMS line enhancer on the mid signal)
    static constexpr int kT = 32, kD = 24, kHist = 8192;
    std::vector<float> an;
    float anW[kT] = {};
    int anPos = kT + kD;
    // clarifier: FIR Hilbert + oscillator
    static constexpr int kH = 63;
    float hil[kH] = {};
    float hbuf[2][2 * 64] = {};
    int hpos = 0;
    double phase = 0;
    float shift = 0;
    // squelch
    SquelchCore sq;
    ToneDetector ctcss; DcsDetector dcs;
    float ctcssCfg = -1; int dcsCfg = -1;
    BiquadCoef spHp, spLp, nzHp1, nzHp2;
    BiquadState spS[4], nzS[2];
    float snrDb = -120;
    std::vector<float> tail;   // stereo ring
    int tailPos = 0, tailLen = 0;
    float gate = 1.f;
    Agc agc;
    Meter meter;
    float wet = 0;
    float l[AudioControl::kMaxChunk], r[AudioControl::kMaxChunk], m[AudioControl::kMaxChunk];
    float agcBuf[AudioControl::kMaxChunk * 2];
    float afRing[1024] = {};
    int afN = 0;
    float zeroCrossAcc = 0;

    void prepare(int sr) {
        rate = sr;
        an.assign(kHist, 0.f);
        std::memset(anW, 0, sizeof anW); anPos = kT + kD;
        for (int i = 0; i < kH; i++) {   // Hamming-windowed Hilbert, odd taps only
            const int k = i - kH / 2;
            hil[i] = (k % 2 != 0) ? (2.f / (kPi * (float)k)) * (0.54f + 0.46f * std::cos(2 * kPi * (float)k / (float)kH)) : 0.f;
        }
        std::memset(hbuf, 0, sizeof hbuf); hpos = 0; phase = 0;
        tail.assign((size_t)(0.5f * (float)sr) * 2 + 2, 0.f); tailPos = 0; tailLen = 0;
        agc.prepare((float)sr);
        gen = 0xFFFFFFFFu;
        reset();
    }
    void reset() {
        std::memset(hpS, 0, sizeof hpS); std::memset(lpS, 0, sizeof lpS); std::memset(ntS, 0, sizeof ntS); std::memset(eqS, 0, sizeof eqS);
        std::memset(toneS, 0, sizeof toneS); std::memset(spS, 0, sizeof spS); std::memset(nzS, 0, sizeof nzS);
        deY[0] = deY[1] = 0; sq.reset(); gate = 1.f; wet = 0;
        std::fill(tail.begin(), tail.end(), 0.f);
        std::memset(anW, 0, sizeof anW); std::fill(an.begin(), an.end(), 0.f); anPos = kT + kD;
        std::memset(hbuf, 0, sizeof hbuf);
        agc.env_ = 0; agc.gain_ = 1;
    }
    void design() {
        const float fr = (float)rate;
        hpN = s.hp ? (s.hpSlope >= 24 ? 2 : 1) : 0; lpN = s.lp ? (s.lpSlope >= 24 ? 2 : 1) : 0;
        if (hpN == 1) hpC[0] = bqHighpass(s.hpHz, 0.7071f, fr); else if (hpN == 2) { hpC[0] = bqHighpass(s.hpHz, 0.5412f, fr); hpC[1] = bqHighpass(s.hpHz, 1.3066f, fr); }
        if (lpN == 1) lpC[0] = bqLowpass(s.lpHz, 0.7071f, fr); else if (lpN == 2) { lpC[0] = bqLowpass(s.lpHz, 0.5412f, fr); lpC[1] = bqLowpass(s.lpHz, 1.3066f, fr); }
        ntOn = s.notch; if (ntOn) ntC = bqNotch(s.notchHz, s.notchQ, fr);
        eqOn = s.eq;
        if (eqOn) { float g[kEqBands]; std::memcpy(g, s.eqDb, sizeof g); if (s.eqPreset != EqPreset::Custom) eqPresetGains(s.eqPreset, g); eqDesign(g, fr, eqC); }
        toneHpOn = s.sqKind == SquelchKind::Ctcss || s.sqKind == SquelchKind::Dcs;
        if (toneHpOn) toneHp = bqHighpass(260.f, 0.7071f, fr);
        const float tau = s.deemph == Deemph::Us50 ? 50e-6f : 75e-6f;
        deA = s.deemph == Deemph::Off ? 0.f : 1.f - std::exp(-1.f / (tau * fr));
        agc.setMode(s.agc, s.agcTargetDb);
        shift = s.shiftHz + (s.cwToneHz > 0 ? s.cwToneHz - kCwNominalHz : 0.f);
        sq.openDb = s.sqOpenDb; sq.closeDb = s.sqOpenDb - s.sqHystDb; sq.hangMs = s.sqHangMs;
        if (s.sqKind == SquelchKind::Ctcss && s.ctcssHz != ctcssCfg) { ctcss.prepare(fr, s.ctcssHz); ctcssCfg = s.ctcssHz; }
        if (s.sqKind == SquelchKind::Dcs && s.dcsCode != dcsCfg) { dcs.prepare(fr, s.dcsCode); dcsCfg = s.dcsCode; }
        spHp = bqHighpass(300.f, 0.7071f, fr); spLp = bqLowpass(3000.f, 0.7071f, fr);
        nzHp1 = bqHighpass(5500.f, 0.5412f, fr); nzHp2 = bqHighpass(5500.f, 1.3066f, fr);
        tailLen = std::min((int)(s.sqTailMs * 0.001f * fr), (int)(tail.size() / 2) - 1);
    }
};

AudioControl::AudioControl() : p_(new Impl), cfg_(&AudioControlConfig::global()) {}
AudioControl::~AudioControl() = default;
void AudioControl::prepare(int sr) { p_->prepare(std::max(8000, sr)); }
void AudioControl::reset() { p_->reset(); }

const float* AudioControl::process(const float* in, int frames, float* scratch, bool* active, bool* openOut) {
    Impl& P = *p_;
    if (frames > kMaxChunk) frames = kMaxChunk;
    const uint32_t g = cfg_->generation();
    if (g != P.gen) { P.s = cfg_->get(); P.gen = g; P.design(); }
    const AudioControlSettings& s = P.s;
    const bool want = s.on;
    if (!want && P.wet <= 0.f) {
        open_.store(true, std::memory_order_relaxed);
        if (active) *active = false;
        if (openOut) *openOut = true;
        return in;
    }
    float *L = P.l, *R = P.r, *M = P.m;
    for (int i = 0; i < frames; i++) { L[i] = in[2 * i]; R[i] = in[2 * i + 1]; }
    // 1. stereo / mono / swap
    switch (s.stereo) {
        case StereoMode::Mono: for (int i = 0; i < frames; i++) L[i] = R[i] = 0.5f * (L[i] + R[i]); break;
        case StereoMode::Swap: for (int i = 0; i < frames; i++) std::swap(L[i], R[i]); break;
        case StereoMode::Left: for (int i = 0; i < frames; i++) R[i] = L[i]; break;
        case StereoMode::Right: for (int i = 0; i < frames; i++) L[i] = R[i]; break;
        default: break;
    }
    for (int i = 0; i < frames; i++) M[i] = 0.5f * (L[i] + R[i]);
    // 2. squelch measurement on the untouched sound
    bool sqOpen = true;
    float measure = -120.f;
    if (s.sqKind != SquelchKind::Off) {
        const float dtMs = 1000.f * (float)frames / (float)P.rate;
        if (s.sqKind == SquelchKind::Level || (s.sqKind == SquelchKind::Snr && P.rate < 16000)) {
            float e = 0; for (int i = 0; i < frames; i++) e += M[i] * M[i];
            measure = 10.f * std::log10(std::max(e / (float)std::max(1, frames), 1e-14f));
            sqOpen = P.sq.update(measure, dtMs);
        } else if (s.sqKind == SquelchKind::Snr) {
            float es = 0, en = 0;
            for (int i = 0; i < frames; i++) {
                float x = bqRun(P.spHp, P.spS[0], M[i]); x = bqRun(P.spLp, P.spS[1], x); es += x * x;
                float y = bqRun(P.nzHp1, P.nzS[0], M[i]); y = bqRun(P.nzHp2, P.nzS[1], y); en += y * y;
            }
            const float bwS = 2700.f, bwN = std::max(500.f, 0.47f * (float)P.rate - 5500.f);
            const float ratio = (es / bwS + 1e-14f) / (en / bwN + 1e-14f);
            measure = 10.f * std::log10(ratio);
            sqOpen = P.sq.update(measure, dtMs);
        } else if (s.sqKind == SquelchKind::Ctcss) {
            P.ctcss.process(M, frames);
            measure = P.ctcss.present() ? 100.f : -100.f;
            P.sq.openDb = 0; P.sq.closeDb = -1; P.sq.hangMs = s.sqHangMs;
            sqOpen = P.sq.update(measure, dtMs);
        } else {
            P.dcs.process(M, frames);
            measure = P.dcs.present() ? 100.f : -100.f;
            P.sq.openDb = 0; P.sq.closeDb = -1; P.sq.hangMs = s.sqHangMs;
            sqOpen = P.sq.update(measure, dtMs);
        }
    }
    // 3. filters
    for (int c = 0; c < 2; c++) {
        float* x = c ? R : L;
        for (int n = 0; n < P.hpN; n++) for (int i = 0; i < frames; i++) x[i] = bqRun(P.hpC[n], P.hpS[c][n], x[i]);
        for (int n = 0; n < P.lpN; n++) for (int i = 0; i < frames; i++) x[i] = bqRun(P.lpC[n], P.lpS[c][n], x[i]);
        if (P.toneHpOn) for (int i = 0; i < frames; i++) x[i] = bqRun(P.toneHp, P.toneS[c], x[i]);
        if (P.ntOn) for (int i = 0; i < frames; i++) x[i] = bqRun(P.ntC, P.ntS[c], x[i]);
    }
    // 4. auto notch
    if (s.autoNotch) {
        constexpr float mu = 0.004f;
        float cross = 0, pe = 0, ee = 0; float prev = 0;
        for (int i = 0; i < frames; i++) {
            const float mid = 0.5f * (L[i] + R[i]);
            P.an[P.anPos] = mid;
            const float* u = &P.an[P.anPos - Impl::kD - (Impl::kT - 1)];
            float p = 0, nrm = 1e-6f;
            for (int k = 0; k < Impl::kT; k++) { p += P.anW[k] * u[k]; nrm += u[k] * u[k]; }
            const float e = mid - p, f = mu * e / nrm;
            for (int k = 0; k < Impl::kT; k++) P.anW[k] += f * u[k];
            L[i] -= p; R[i] -= p;
            if ((p > 0) != (prev > 0)) cross += 1;
            prev = p; pe += p * p; ee += mid * mid;
            if (++P.anPos >= Impl::kHist) {
                std::memmove(P.an.data(), &P.an[P.anPos - (Impl::kD + Impl::kT)], (Impl::kD + Impl::kT) * sizeof(float));
                P.anPos = Impl::kD + Impl::kT;
            }
        }
        cfg_->statNotchHz.store(pe > 0.25f * ee && frames > 0 ? 0.5f * cross * (float)P.rate / (float)frames : 0.f, std::memory_order_relaxed);
    } else cfg_->statNotchHz.store(0.f, std::memory_order_relaxed);
    // 5. clarifier / CW pitch
    if (std::fabs(P.shift) > 0.5f) {
        const double inc = 2 * 3.14159265358979 * P.shift / P.rate;
        for (int i = 0; i < frames; i++) {
            const int pos = P.hpos;
            P.hbuf[0][pos] = P.hbuf[0][pos + 64] = L[i]; P.hbuf[1][pos] = P.hbuf[1][pos + 64] = R[i];
            float hl = 0, hr = 0;
            const float* bl = &P.hbuf[0][pos + 64 - (Impl::kH - 1)];   // oldest first
            const float* br = &P.hbuf[1][pos + 64 - (Impl::kH - 1)];
            for (int k = 0; k < Impl::kH; k += 2) { hl += P.hil[k] * bl[Impl::kH - 1 - k]; hr += P.hil[k] * br[Impl::kH - 1 - k]; }
            const float dl = P.hbuf[0][pos + 64 - Impl::kH / 2], dr = P.hbuf[1][pos + 64 - Impl::kH / 2];
            const float cs = (float)std::cos(P.phase), sn = (float)std::sin(P.phase);
            L[i] = dl * cs - hl * sn; R[i] = dr * cs - hr * sn;
            P.phase += inc; if (P.phase > 6.283185307179586) P.phase -= 6.283185307179586; else if (P.phase < -6.283185307179586) P.phase += 6.283185307179586;
            P.hpos = (pos + 1) & 63;
        }
    }
    // 6. EQ
    if (P.eqOn) for (int c = 0; c < 2; c++) {
        float* x = c ? R : L;
        for (int b = 0; b < kEqBands; b++) {
            const BiquadCoef& k = P.eqC[b];
            if (std::fabs(k.b0 - 1) < 1e-4f && std::fabs(k.b1 - k.a1) < 1e-4f && std::fabs(k.b2 - k.a2) < 1e-4f) continue;   // flat band
            for (int i = 0; i < frames; i++) x[i] = bqRun(k, P.eqS[c][b], x[i]);
        }
    }
    // 7. de-emphasis
    if (P.deA > 0) for (int c = 0; c < 2; c++) {
        float* x = c ? R : L; float y = P.deY[c];
        for (int i = 0; i < frames; i++) { y += P.deA * (x[i] - y); x[i] = y; }
        P.deY[c] = y;
    }
    // interleave into scratch
    for (int i = 0; i < frames; i++) { scratch[2 * i] = L[i]; scratch[2 * i + 1] = R[i]; }
    // 8. AGC
    P.agc.process(scratch, frames);
    cfg_->statAgcDb.store(P.agc.mode_ == AgcMode::Off ? 0.f : P.agc.gainDb(), std::memory_order_relaxed);
    // 9. squelch gate with the tail delay
    if (s.sqKind != SquelchKind::Off) {
        const float step = 1.f / (0.005f * (float)P.rate), tgt = sqOpen ? 1.f : 0.f;
        const int cap = (int)P.tail.size() / 2;
        for (int i = 0; i < frames; i++) {
            float a = scratch[2 * i], b = scratch[2 * i + 1];
            if (P.tailLen > 0) {
                const int rd = (P.tailPos - P.tailLen + cap) % cap;
                P.tail[2 * P.tailPos] = a; P.tail[2 * P.tailPos + 1] = b;
                a = P.tail[2 * rd]; b = P.tail[2 * rd + 1];
                P.tailPos = (P.tailPos + 1) % cap;
            }
            P.gate += P.gate < tgt ? std::min(step, tgt - P.gate) : -std::min(step, P.gate - tgt);
            scratch[2 * i] = a * P.gate; scratch[2 * i + 1] = b * P.gate;
        }
    } else P.gate = 1.f;
    // crossfade in and out of the chain
    {
        const float step = 1.f / (0.01f * (float)P.rate), tgt = want ? 1.f : 0.f;
        if (P.wet != tgt || tgt < 1.f) {
            for (int i = 0; i < frames; i++) {
                P.wet += P.wet < tgt ? std::min(step, tgt - P.wet) : -std::min(step, P.wet - tgt);
                for (int c = 0; c < 2; c++) { const float d = in[2 * i + c]; scratch[2 * i + c] = d + P.wet * (scratch[2 * i + c] - d); }
            }
        }
    }
    // 10. meters and the AF strip
    P.meter.process(scratch, frames);
    cfg_->statPeakDb.store(P.meter.peakDb(), std::memory_order_relaxed);
    cfg_->statRmsDb.store(P.meter.rmsDb(), std::memory_order_relaxed);
    cfg_->statClips.store(P.meter.clipped, std::memory_order_relaxed);
    cfg_->statSqMeasure.store(measure, std::memory_order_relaxed);
    cfg_->statSqOpen.store(sqOpen, std::memory_order_relaxed);
    if (cfg_->afWanted.load(std::memory_order_relaxed)) {
        for (int i = 0; i < frames; i++) {
            P.afRing[P.afN++] = 0.5f * (scratch[2 * i] + scratch[2 * i + 1]);
            if (P.afN == 1024) {
                float db[512];
                spectrumDb(P.afRing, 1024, db);
                for (int b = 0; b < AudioControlConfig::kAfBins; b++) {
                    float mx = -120.f; for (int k = 0; k < 4; k++) mx = std::max(mx, db[b * 4 + k]);
                    cfg_->afDb[b].store(mx, std::memory_order_relaxed);
                }
                cfg_->afRate.store(P.rate, std::memory_order_relaxed);
                P.afN = 0;
            }
        }
    }
    open_.store(sqOpen, std::memory_order_relaxed);
    if (active) *active = true;
    if (openOut) *openOut = sqOpen;
    return scratch;
}

}  // namespace dect2

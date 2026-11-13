// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec 4: profiles, bit layout and the 8 kHz analysis-by-synthesis core (internal).
#pragma once
#include "dect2/kaalvoice/kaal_voice.h"
#include "kv_internal.h"
#include <string>
#include <vector>

namespace dect2 {
namespace kaalvoice {
namespace detail {

constexpr int kLA = 40;                     // core look-ahead, 5 ms at 8 kHz
constexpr int kPitMin = 20, kPitMax = 147;  // pitch lag range (54 .. 400 Hz)
constexpr int kNI = 8;                      // fractional-delay interpolation taps per side
constexpr int kUp = 4;                      // interpolation phases (quarter samples)
constexpr int kExcHist = 168;               // past excitation kept (>= kPitMax + kNI + 1)
constexpr int kMaxSub = 16;
constexpr int kMaxN8 = 320;                 // 40 ms at 8 kHz
constexpr int kDownDelay = 46, kUpDelay = 47, kSplitDelay = kDownDelay + kUpDelay;   // 16 kHz samples
constexpr int kHbM = 80;                    // high-band MDCT hop (5 ms at 16 kHz)
constexpr int kHbBands = 6;
constexpr int kHbLo = 36;                   // first high-band bin (3.6 kHz)
extern const int kHbEdge[kHbBands + 1];     // bin edges of the high-band envelope bands
constexpr int kSidBits = 40;

enum class FK : uint8_t { Lsf, Interp, Lag0, LagD, Gp, Gc, Fcb, HbG, HbS, HbT };

struct Field {
    FK kind;
    uint8_t bits, a, b;   // total bits; the top `a` bits are class A, the next `b` class B, the rest class C
};

struct HbProf {
    bool on = false;
    int gainBits = 5;
    int shapeMode = 0;    // 0: tilt (3 bits), 1: tilt + curvature (3 + 2), 2: per band 2 bits, 3: per band 3 bits
    int timeBits = 0;     // per 5 ms block: 0 (smooth) or 2
};

struct Prof {
    int fs = 16000, frameMs = 20;
    int N8 = 160;             // core frame at 8 kHz
    int L = 40, S = 4;        // subframe length and count
    int halves = 1;           // 20 ms halves per frame (absolute lag + high band per half)
    int lsfBits[kP] = {};
    int lsfTotal = 0;
    int interpBits = 0;       // 40 ms frames: mid-frame LSF interpolation choice
    int lagRes = 2;           // 1: integer lags, 2: half-sample lags
    int lag0Bits = 8, lagDBits = 4;
    int gpBits = 4, gcBits = 5;
    int fcbBits[kMaxSub] = {}, fcbK[kMaxSub] = {};
    HbProf hb;
    // decoder postfilter
    double pfNum = 0.55, pfDen = 0.70, pfLtp = 0.5;
    double lsfRho = 0.6;      // LSF inter-frame prediction
    int totalBits = 0, padBits = 0;
    int classBits[3] = {0, 0, 0};
    std::vector<Field> fields;
};

// Tier = the side-information allocation for one 20 ms budget range (exposed for the tuning tool).
struct Tier {
    int minBits20;            // applies from this budget per 20 ms upwards
    int L;
    int lsfTotal;
    int lagRes, lag0Bits, lagDBits;
    int gpBits, gcBits;
    HbProf hb;
    double pfNum, pfDen, pfLtp;
};
const Tier* defaultTiers(int* n);
bool makeProf(const Config& c, Prof& P, std::string* why, const Tier* override = nullptr);

// Quantised parameters of one frame (indices).
struct SubQ { int lag = 0, gp = 0, gc = 0; uint64_t fcb = 0; };
struct HbQ { int gain = 0; int shape[kHbBands] = {}; int time[4] = {}; };
struct FrameQ {
    int lsf[kP] = {};
    int interp = 0;
    SubQ sub[kMaxSub];
    HbQ hb[2];
};

// Visits every field of a frame in bit-stream order: fn(FK kind, int index, uint64_t& value-ref-proxy).
template <class Fn> void walkFrame(const Prof& P, FrameQ& q, Fn&& fn) {
    auto vi = [&](FK k, int i, int& v) { uint64_t t = (uint64_t)(int64_t)v; fn(k, i, t); v = (int)t; };
    for (int i = 0; i < kP; i++) vi(FK::Lsf, i, q.lsf[i]);
    if (P.interpBits) vi(FK::Interp, 0, q.interp);
    const int sh = P.S / P.halves;
    for (int h = 0; h < P.halves; h++) {
        for (int s = 0; s < sh; s++) {
            SubQ& u = q.sub[h * sh + s];
            vi(s == 0 ? FK::Lag0 : FK::LagD, h * sh + s, u.lag);
            vi(FK::Gp, h * sh + s, u.gp);
            vi(FK::Gc, h * sh + s, u.gc);
            if (P.fcbBits[h * sh + s] > 0) fn(FK::Fcb, h * sh + s, u.fcb);
        }
        if (P.hb.on) {
            HbQ& b = q.hb[h];
            vi(FK::HbG, h, b.gain);
            const int ns = P.hb.shapeMode == 0 ? 1 : P.hb.shapeMode == 1 ? 2 : kHbBands;
            for (int j = 0; j < ns; j++) vi(FK::HbS, j, b.shape[j]);
            if (P.hb.timeBits)
                for (int t = 0; t < 4; t++) vi(FK::HbT, t, b.time[t]);
        }
    }
}

// Bit packing in importance order (all class A bits, then B, then C, then zero padding); MSB first.
void packFrame(const Prof& P, FrameQ& q, uint8_t* out);
// Unpacks; fields whose bits fall in a bad class get neutral values. fcbBad[s] tells which pulse fields were damaged.
void unpackFrame(const Prof& P, const uint8_t* in, uint32_t bad, FrameQ& q, bool* fcbBad, bool* hbTimeBad);

// LSF quantiser (shared by encoder and decoder).
struct LsfQ {
    static const double* mean();
    // Quantises lsf (radians) predicted from prev (quantised); writes indices and the quantised vector.
    static void quantise(const Prof& P, const double* lsf, const double* prev, int* idx, double* q);
    static void dequantise(const Prof& P, const int* idx, const double* prev, double* q);
};

// ---- the 8 kHz core ----
struct CoreState {   // what encoder and decoder must keep identical
    double lsfPrev[kP];         // quantised LSF of the previous frame (end of frame)
    float exc[kExcHist + kMaxN8];
    float syn[kP];              // synthesis filter memory (past decoded samples)
    double gcHist[4];           // quantised energy corrections (dB) of the last subframes
    double gpPrev;              // quantised pitch gain of the previous subframe (pitch sharpening)
    int lagPrev;                // last lag in lag units (for delta coding)
    void reset();
};

class CoreEncoder {
public:
    explicit CoreEncoder(const Prof& P);
    void reset();
    // in8: N8 new 8 kHz samples. The coded frame lags them by kLA (look-ahead).
    void encode(const float* in8, FrameQ& q);
    // Restart after DTX: both sides reset excitation, gain prediction and set the LSF predictor to `lsf`.
    void restart(const double* lsf);
    double voicing() const { return voicing_; }   // open-loop pitch correlation of the last frame (0..1)
    double residualDb() const { return resDb_; }   // LP residual power of the last frame (dB), for the SID
    const double* lsfUnq() const { return lsfOut_; }   // unquantised end-of-frame LSF of the last frame
    const float* codedFrame() const { return sp_ + kPast; }   // the 8 kHz samples of the frame just coded
    static constexpr int kPast = 80;               // history kept before the frame
private:
    int openLoop(const float* w, int n);
    const Prof& P_;
    CoreState st_;
    double lsfUnqPrev_[kP], lsfOut_[kP] = {};
    float sp_[kPast + kMaxN8 + kLA];
    float wsp_[kPitMax + 1 + kMaxN8];   // weighted speech history + frame (open-loop pitch)
    float wFirMem_[kP], wIirMem_[kP];   // weighting filter memories for the weighted speech
    float errIir_[kP];                  // weighted-synthesis IIR (1/A(z/g2)) memory of the error path
    double phi_[80 * 80];
    double voicing_ = 0, resDb_ = 0, olVoicing_ = 0;
    int olPrev_ = 60;
};

// What the decoder used for one subframe (side information for the enhancer, kv_enhance.cpp).
struct SubInfo {
    float aq[kP + 1];   // quantised LPC
    int Ti;             // integer pitch lag (8 kHz samples)
    double lag;         // pitch lag in samples (half-sample resolution on the higher tiers)
    double gp, gc;      // pitch gain and fixed-codebook gain
    double ecb;         // mean energy per sample of the (sharpened) pulse vector
    bool rnd;           // random pulses (lost frame or damaged class C)
};

class CoreDecoder {
public:
    explicit CoreDecoder(const Prof& P);
    void reset();
    // q == nullptr: lost frame. fcbBad[s]: replace those pulses by noise. Writes N8 samples (postfiltered when pf).
    void decode(const FrameQ* q, const bool* fcbBad, bool pf, float* out);
    void restart(const double* lsf);
    const double* lsfLast() const { return st_.lsfPrev; }
    double lastVoicing() const { return voicing_; }
    int lostRun() const { return lost_; }
    const SubInfo& sub(int s) const { return sub_[s]; }
    const float* excitation() const { return excOut_; }   // the excitation of the last frame (N8 samples)
private:
    void postfilter(const float* a, const float* in, float* out, int n, int lag, double gp);
    const Prof& P_;
    CoreState st_;
    Rng rng_;
    int lost_ = 0;
    double gpLast_ = 0, voicing_ = 0;
    // postfilter state
    float pfRes_[kPitMax + 2 + kMaxN8];
    float pfFir_[kP], pfIir_[kP];
    float pfTilt_ = 0;
    double pfAgc_ = 1.0, mute_ = 1.0;
    SubInfo sub_[kMaxSub];
    float excOut_[kMaxN8];
};

// ---- high band (16 kHz) ----
class HbEncoder {
public:
    explicit HbEncoder(const Prof& P);
    void reset();
    // hb: frameMs * 16 samples of the high-band signal; fills q.hb[0..halves)
    void encode(const float* hb, FrameQ& q);
    double levelDb() const { return lvl_; }   // mean high-band power (dB per bin) of the last frame (SID)
    double tiltDb() const { return tilt_; }   // spectral tilt (dB from the band middle to the top band) of the last frame
private:
    const Prof& P_;
    float hist_[kHbM];
    double lvl_ = 0, tilt_ = 0;
};
class HbDecoder {
public:
    explicit HbDecoder(const Prof& P);
    void reset();
    // q == nullptr: lost (repeat, fading). Adds nothing when the high band is off. out: frameMs * 16 samples.
    void decode(const FrameQ* q, const bool* timeBad, float* out);
    // The same state update as decode(), but returns the band levels (dB per bin) of every 5 ms block instead of synthesising them.
    void levels(const FrameQ* q, const bool* timeBad, double (*L)[kHbBands]);
    void synth(const double (*L)[kHbBands], int nb, float* out);   // noise of those levels, nb 5 ms blocks
    // Comfort noise: flat-ish noise at levelDb (dB per bin) with a tilt.
    void comfort(double levelDb, double tiltDb, float* out, int n16);
    void comfortLevels(double levelDb, double tiltDb, double* L);   // the levels comfort() would use (same state update)
private:
    void synthBlock(const double* bandDb, float* out);
    const Prof& P_;
    Rng rng_;
    float ola_[kHbM];
    double lastDb_[kHbBands];
    bool have_ = false;
    int lost_ = 0;
};

// MDCT (orthonormal, sine window, M = kHbM) used by the high band.
void mdct80(const float* x160, float* X80);
void imdct80(const float* X80, float* y160);

// Pitch-lag code helpers.
inline int lagUnits(int res, double lag) { return (int)(lag * res + 0.5); }

}  // namespace detail
}  // namespace kaalvoice
}  // namespace dect2

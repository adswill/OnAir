// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signals for DCR (ARIB STD-T98 volume 3, 4-level FSK): frames built from the standard, 4-level FSK with the raised cosine pulse (roll-off 0.2) at
// +-315 / +-945 Hz, with the impairments the receiver has to live with. Exists only in the tests; nothing here is reachable from the transmit side of the application.
#pragma once
#include "x40_gen.h"
#include "dect2/nxdn/nxdn_proto.h"
#include "dect2/x40/dcr_plan.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace dcrgen {
using dect2::cf32;
using x40gen::Rng;
inline constexpr double kPi = 3.14159265358979323846;

typedef std::vector<int8_t> Syms;

// ---- the standard's own frame bits (chapter 7 test signals, hexadecimal, after whitening) turned into symbols (a dibit 01 is +3, 00 +1, 10 -1, 11 -3)
inline void hexSyms(const char* hex, Syms& s) {
    for (const char* p = hex; *p; p++) {
        const int v = *p >= 'A' ? *p - 'A' + 10 : *p - '0';
        s.push_back((int8_t)dect2::nxdn::dibitToSym((v >> 2) & 3));
        s.push_back((int8_t)dect2::nxdn::dibitToSym(v & 3));
    }
}

struct SpecSignal { const char *name, *sbSacch, *scSacch, *endSacch, *tch1, *tch2; int uc, comm; };
inline const SpecSignal* specSignals() {
    static const SpecSignal s[4] = {
        {"user code test 1 (1031 Hz tone, UC 1)", "C60DB46E960168D", "C60DB46E960168D", "8E4DB4AD8F1D6FC",
         "4CAADE8B26E4F28288C68A7429A4ECD00822", "CEA2FC018CECDA0AA0EE8A7E2B26CCF88A08", 1, 0},
        {"user code test 2 (no sound, UC 1)", "C60DB46E960168D", "C60DB46E960168D", "8E4DB4AD8F1D6FC",
         "3BEAA15AEB53AA88E9B1CA0BF8695B880243", "B9E283D0415B8200C199CA01FAEB7BA08069", 1, 0},
        {"user code test 3 (1031 Hz tone, UC 511)", "F70DF4DB9A4B280", "F70DF4DB9A4B280", "BF4DF41883572F1",
         "4CAADE8B26E4F28288C68A7429A4ECD00822", "CEA2FC018CECDA0AA0EE8A7E2B26CCF88A08", 511, 0},
        {"private communication test (privacy key 129, UC 511)", "FF0F705A9A1BBAB", "FF0F705A9A1BBAB", "B74F70998307BDA",
         "EA8AACD526C46E1AAE6B36177F4F559B4B03", "C0A8A77326001E8E205EB25A2946C92D4193", 511, 1},
    };
    return s;
}
// SB0, nAudio audio frames, the end frame: symbols from the preamble on (RICH values: SB0 5F7D, audio DDDD, end DF5D)
inline Syms specTransmission(const SpecSignal& g, int nAudio) {
    Syms s;
    for (int i = 0; i < 3; i++) { s.push_back(3); s.push_back(3); s.push_back(-3); s.push_back(-3); }
    hexSyms("5775FD", s);
    hexSyms("CDF59", s);
    hexSyms("5F7D", s); hexSyms(g.sbSacch, s);
    hexSyms("82022028832C2AA08399EAAEF82838802E2B", s);
    hexSyms("000A02822028822AAA202280A88A08A0AA02", s);
    for (int i = 0; i < nAudio; i++) { hexSyms("CDF59", s); hexSyms("DDDD", s); hexSyms(g.scSacch, s); hexSyms(g.tch1, s); hexSyms(g.tch2, s); }
    hexSyms("CDF59", s); hexSyms("DF5D", s); hexSyms(g.endSacch, s); hexSyms(g.tch1, s); hexSyms(g.tch2, s);
    return s;
}

// ---- frames from the field definitions
struct Bits {
    std::vector<uint8_t> v;
    void put(int val, int n) { for (int i = n - 1; i >= 0; i--) v.push_back((uint8_t)((val >> i) & 1)); }
};
inline void sacchAir(int lead, int wr, int type, const uint8_t* pl18, uint8_t* air60) {
    uint8_t info[26];
    info[0] = (uint8_t)lead; info[1] = (uint8_t)((wr >> 1) & 1); info[2] = (uint8_t)(wr & 1);
    for (int i = 0; i < 5; i++) info[3 + i] = (uint8_t)((type >> (4 - i)) & 1);
    for (int i = 0; i < 18; i++) info[8 + i] = pl18[i];
    dect2::nxdn::encode(dect2::nxdn::Ch::Sacch, info, air60);
}
inline void firstUnit(int comm, int uc, int mfr, uint8_t* pl18) {
    Bits b; b.put(comm, 2); b.put(uc, 9); b.put(mfr, 7);
    for (int i = 0; i < 18; i++) pl18[i] = b.v[(size_t)i];
}
// One frame: F, M, D and the three channel blocks (60 + 144 + 144 bits); the symbols are whitened; the sync word first
inline void frame(Syms& s, int F, int M, const uint8_t* sacch60, const uint8_t* t1, const uint8_t* t2) {
    int b8[8];
    { int v[7] = {F, 0, 0, (M >> 2) & 1, (M >> 1) & 1, M & 1, 0}; int n = 0; for (int i = 0; i < 7; i++) { b8[i] = v[i]; n += v[i]; } b8[7] = n & 1; }
    int8_t pay[182];
    for (int k = 0; k < 8; k++) pay[k] = (int8_t)(b8[k] ? -3 : 3);
    uint8_t bits[348];
    for (int i = 0; i < 60; i++) bits[i] = sacch60[i];
    for (int i = 0; i < 144; i++) { bits[60 + i] = t1[i]; bits[204 + i] = t2[i]; }
    for (int k = 0; k < 174; k++) pay[8 + k] = (int8_t)dect2::nxdn::dibitToSym((bits[2 * k] << 1) | bits[2 * k + 1]);
    uint8_t pn[182]; dect2::nxdn::pn9(pn, 182);
    for (int k = 0; k < dect2::nxdn::kFswSym; k++) s.push_back((int8_t)dect2::nxdn::kFsw[k]);
    for (int k = 0; k < 182; k++) s.push_back((int8_t)(pn[k] ? -pay[k] : pay[k]));
}

struct FrameGen {
    Syms s;
    uint32_t rng = 12345;
    int rnd() { rng = rng * 1664525u + 1013904223u; return (int)(rng >> 16); }
    void voiceBits(uint8_t* t) { for (int i = 0; i < 144; i++) t[i] = (uint8_t)(rnd() & 1); }
    void preamble() { for (int i = 0; i < 3; i++) { s.push_back(3); s.push_back(3); s.push_back(-3); s.push_back(-3); } hexSyms("5775FD", s); }
    // synchronous burst: voice message in the SACCH, the call sign (9 digits) in the PICH
    void sb0(int comm, int uc, int mfr, const std::string& csm) {
        preamble();
        uint8_t pl[18], sa[60], pich[144], und[144] = {0}, info[80] = {0};
        firstUnit(comm, uc, mfr, pl); sacchAir(1, 0, 1, pl, sa);
        for (int i = 0; i < 9; i++) { const int d = csm[(size_t)i] - '0'; for (int k = 0; k < 4; k++) info[4 * i + k] = (uint8_t)((d >> (3 - k)) & 1); }
        dect2::nxdn::encode(dect2::nxdn::Ch::Facch1, info, pich);
        frame(s, 0, 4, sa, pich, und);
    }
    void audio(int comm, int uc, int mfr, int n) {
        uint8_t pl[18], sa[60], t1[144], t2[144];
        firstUnit(comm, uc, mfr, pl); sacchAir(1, 0, 1, pl, sa);
        for (int i = 0; i < n; i++) { voiceBits(t1); voiceBits(t2); frame(s, 1, 3, sa, t1, t2); }
    }
    void end(int comm, int uc, int mfr) {
        uint8_t pl[18], sa[60], t1[144], t2[144];
        firstUnit(comm, uc, mfr, pl); sacchAir(1, 0, 30, pl, sa);
        voiceBits(t1); voiceBits(t2); frame(s, 1, 5, sa, t1, t2);
    }
    // a manufacturer-defined message of `units` SACCH units (1 - 4), one unit per frame, repeated `reps` times; the frame's traffic is idle noise bits
    void mfrMessage(int type, int comm, int uc, int mfr, int units, const std::vector<uint8_t>& extra, int reps) {
        for (int r = 0; r < reps; r++)
            for (int u = 0; u < units; u++) {
                uint8_t pl[18] = {0}, sa[60], t1[144], t2[144];
                if (u == 0) firstUnit(comm, uc, mfr, pl);
                else for (int i = 0; i < 18; i++) { const size_t k = (size_t)(18 * (u - 1) + i); pl[i] = k < extra.size() ? extra[k] : 0; }
                sacchAir(u == 0 ? 1 : 0, units - 1 - u, type, pl, sa);
                voiceBits(t1); voiceBits(t2);
                frame(s, 1, 5, sa, t1, t2);
            }
    }
    // data frames: both traffic channels error-corrected (M = 4), 80 information bits each
    void data(int comm, int uc, int mfr, const std::vector<uint8_t>& d80a, const std::vector<uint8_t>& d80b, int n) {
        uint8_t pl[18], sa[60], t1[144], t2[144];
        firstUnit(comm, uc, mfr, pl); sacchAir(1, 0, 1, pl, sa);
        dect2::nxdn::encode(dect2::nxdn::Ch::Facch1, d80a.data(), t1);
        dect2::nxdn::encode(dect2::nxdn::Ch::Facch1, d80b.data(), t2);
        for (int i = 0; i < n; i++) frame(s, 1, 4, sa, t1, t2);
    }
};

// ---- radio signal
struct Imp {
    double fs = 250000;
    double offsetHz = 0;       // the channel's place in the capture (relative to the tuned centre)
    double cfoHz = 0;          // the transmitter's frequency error
    double clockPpm = 0;       // symbol clock error
    double snrDb = 99;         // carrier power over noise power in 6.25 kHz; >= 99: no noise
    double amp = 0.05;
    bool invert = false;       // the discriminator's sign flipped (reversed sidebands)
    double devScale = 1.0;     // outer deviation / 945 Hz
    unsigned seed = 1;
};

// A transmission is rendered at 96 kHz as frequency (NaN = no carrier) and then to the capture rate. `into` adds a channel to an existing capture (several channels).
struct Seg { Syms sym; double gapSec = 0.6; };
inline std::vector<double> shape(const std::vector<Seg>& segs, double leadSec, double devScale, bool invert) {
    const double shapeRate = 96000, sps = 40, unit = 315.0 * devScale;
    std::vector<double> f;
    const double NaNv = std::nan("");
    f.assign((size_t)(leadSec * shapeRate), NaNv);
    const int span = (int)(4 * sps);
    std::vector<double> g((size_t)(2 * span + 1));
    for (int k = -span; k <= span; k++) {
        const double t = (double)k / sps, a = 0.2;
        const double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(kPi * t) / (kPi * t);
        const double d = 1 - 4 * a * a * t * t;
        const double c = std::fabs(d) < 1e-9 ? kPi / 4 : std::cos(kPi * a * t) / d;
        g[(size_t)(k + span)] = sinc * c;
    }
    for (const Seg& sg : segs) {
        const size_t N = sg.sym.size() * (size_t)sps + (size_t)(2 * span);
        std::vector<double> y(N, 0.0);
        for (size_t k = 0; k < sg.sym.size(); k++) {
            const size_t c0 = k * (size_t)sps;
            for (int j = 0; j <= 2 * span; j++) y[c0 + (size_t)j] += sg.sym[k] * unit * g[(size_t)j];
        }
        for (size_t i = (size_t)span; i < N - (size_t)span; i++) f.push_back(invert ? -y[i] : y[i]);
        for (size_t i = 0; i < (size_t)(sg.gapSec * shapeRate); i++) f.push_back(NaNv);
    }
    return f;
}
// add the channel to `x` (complex capture at fs, length given by x.size())
inline void render(std::vector<cf32>& x, const std::vector<double>& f, const Imp& m) {
    const double shapeRate = 96000, step = shapeRate * (1 + m.clockPpm * 1e-6) / m.fs;
    double pos = 0, ph = 0;
    const double off = m.offsetHz + m.cfoHz;
    for (size_t i = 0; i < x.size(); i++) {
        const size_t k = (size_t)pos;
        if (k + 1 >= f.size()) break;
        const double a = f[k], b = f[k + 1];
        if (!std::isnan(a) && !std::isnan(b)) {
            const double fr = pos - (double)k;
            const double fq = a + (b - a) * fr + off;
            ph = std::remainder(ph + 2 * kPi * fq / m.fs, 2 * kPi);
            x[i] += cf32((float)(m.amp * std::cos(ph)), (float)(m.amp * std::sin(ph)));
        }
        pos += step;
    }
}
inline void addNoise(std::vector<cf32>& x, const Imp& m) {
    if (m.snrDb >= 99) return;
    const double nb = m.amp * m.amp / std::pow(10.0, m.snrDb / 10);   // noise power in 6.25 kHz
    const float sg = (float)std::sqrt(nb * m.fs / 6250.0 / 2);
    Rng r(m.seed * 7919u + 3);
    for (auto& v : x) v += cg(r, sg, sg);
}
inline std::vector<cf32> capture(const std::vector<Seg>& segs, const Imp& m, double leadSec = 0.3, double tailSec = 2.0) {
    const std::vector<double> f = shape(segs, leadSec, m.devScale, m.invert);
    std::vector<cf32> x((size_t)((double)f.size() / 96000.0 * m.fs) + (size_t)(tailSec * m.fs), cf32(0, 0));
    render(x, f, m);
    addNoise(x, m);
    return x;
}

// ---- running the receiver and reading what it says
inline dect2::X40Telemetry runDcr(const std::vector<cf32>& x, double fs, double centreHz = 0, double offset = 0) {
    dect2::X40Receiver rx; rx.setVariant(dect2::x40::Dcr); rx.setCenterHz(centreHz); rx.configure(fs); rx.setSignalOffset(offset);
    for (size_t i = 0; i < x.size(); i += 4096) rx.feed(x.data() + i, std::min<size_t>(4096, x.size() - i));
    dect2::X40Telemetry t; rx.telemetry(t, 0);
    return t;
}
inline std::string fact(const dect2::X40Telemetry& t, const char* label) {
    for (auto& f : t.facts) if (f.first == label) return f.second;
    return "";
}
// "N seen, SACCH A good / B bad, ..."
inline void frameCounts(const dect2::X40Telemetry& t, unsigned long long& seen, unsigned long long& sacchGood) {
    seen = sacchGood = 0;
    const std::string s = fact(t, "frames");
    sscanf(s.c_str(), "%llu seen, SACCH %llu good", &seen, &sacchGood);
}
inline unsigned long long voiceCount(const dect2::X40Telemetry& t) {
    unsigned long long n = 0; sscanf(fact(t, "voice frames").c_str(), "%llu", &n); return n;
}
inline int countRows(const dect2::X40Telemetry& t, const char* kind) { int n = 0; for (auto& r : t.rows) if (r.kind == kind) n++; return n; }
inline const dect2::X40Row* findRow(const dect2::X40Telemetry& t, const char* kind, const char* idPart = nullptr, const char* textPart = nullptr) {
    for (auto& r : t.rows) if (r.kind == kind && (!idPart || r.id.find(idPart) != std::string::npos) && (!textPart || r.text.find(textPart) != std::string::npos)) return &r;
    return nullptr;
}

} // namespace dcrgen

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MAC television test signal (see mac_gen.h).
#include "dect2/mac/mac_gen.h"
#include "dect2/mac/mac_proto.h"
#include "dect2/atv/atv_card.h"
#include "dect2/atv/atv_std.h"
#include "../atv/atv_dsp.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include "dect2/common/mathconst.h"

namespace dect2 {

using namespace mac;

namespace {
using dect2::math::kPi;
constexpr double kFm = 2 * kClockHz;   // the FM grid: 40.5 Msps

double kaiserW(double x, double beta) {   // x in [-1, 1]
    if (std::fabs(x) >= 1) return 0;
    return atvdsp::besselI0(beta * std::sqrt(1 - x * x)) / atvdsp::besselI0(beta);
}
double sinc(double x) { return std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

// raised-cosine weight 0..1 over n steps
float rc(int i, int n) { return 0.5f - 0.5f * (float)std::cos(kPi * (i + 0.5) / n); }
} // namespace

struct MacGenerator::Impl {
    MacGenConfig c;
    std::unique_ptr<AtvCard> card;
    std::vector<float> bb;              // the loop at 20.25 MHz, 0..1 V
    std::vector<cf32> loop;             // the loop at the output rate (unit power)
    std::complex<double> loopRot{1, 0};   // the FM phase over one loop
    size_t pos = 0;
    cf32 rot{1, 0};
    uint32_t nrm = 0;
    std::unique_ptr<atvdsp::NoiseSource> noise;
    float sigma = 0;

    explicit Impl(const MacGenConfig& cfg) : c(cfg) {
        c.loopFrames = std::max(1, c.loopFrames);
        AtvFormat f;
        atvMakeFormat(kAtvG, kAtvPal, f);
        card = std::make_unique<AtvCard>(f, c.pattern);
        buildBaseband();
        buildRf();
        noise = std::make_unique<atvdsp::NoiseSource>(c.seed);
        sigma = c.snrDb >= 99 ? 0.f : (float)noiseSigma(c);
    }

    // ------------------------------------------------------------ packets of one frame (82 per subframe)
    struct Counters { int sound = 0, si = 0, ecm = 0, dummy = 0, dg[16] = {}; };
    std::vector<int16_t> tone;          // the whole loop at 32 kHz, 14-bit, pre-emphasised
    size_t toneAt = 0;

    std::vector<SiCommand> networkCmds() const {
        SiCommand n; n.ci = kCiNetwork;
        SiParam nwo; nwo.pi = kPiNwo;
        nwo.field = {0x12, 0x80 | 0x01, 0x91};   // channel 12, +19 deg (position digits), right-hand circular; then the country
        for (char ch : c.country) nwo.field.push_back((uint8_t)ch);
        SiParam nm; nm.pi = kPiNwname;
        for (char ch : c.network) nm.field.push_back((uint8_t)ch);
        SiParam lx; lx.pi = kPiListx;
        const int acc = 1 << 12 | 1 << 10 | kSoundAddress;   // data group 1, in the subframe, packet address of the TV sound
        lx.field = {0x01, 0x01, (uint8_t)(acc >> 8), (uint8_t)acc};
        n.params = {nwo, nm, lx};
        return {n};
    }
    std::vector<SiCommand> tvCmds() const {
        SiCommand t; t.ci = kCiTv;
        SiParam sref; sref.pi = kPiSref;
        sref.field.push_back(0x01);
        for (char ch : c.name) sref.field.push_back((uint8_t)ch);
        SiParam vconf; vconf.pi = kPiVconf;
        const int vsam = c.scrambled ? 0x6 : 0x1;   // controlled access, single-cut line rotation / free access, unscrambled
        vconf.field = {(uint8_t)((c.wide ? 0x30 : 0x20) | vsam)};
        SiParam snd; snd.pi = kPiTvSound;
        const int acc = 1 << 10 | kSoundAddress;
        snd.field = {0x09, (uint8_t)(acc >> 8), (uint8_t)acc};   // language, access coordinates
        if (c.scrambled) {
            SiParam pg; pg.pi = kPiPg;
            SiParam accm; accm.pi = kPiAccm;
            const int a = 0x8000 | 1 << 10 | kEcmAddress;
            accm.field = {(uint8_t)(a >> 8), (uint8_t)a, kCaEuroCrypt};
            pg.group = {accm, vconf, snd};
            t.params = {sref, pg};
        } else t.params = {sref, vconf, snd};
        return {t};
    }

    void framePackets(int k, Counters& ct, std::vector<Packet> pk[2]) {
        for (int s = 0; s < 2; s++) {
            pk[s].assign(kPacketsPerFrame, Packet());
            for (auto& p : pk[s]) { p.address = 1023; p.ci = ct.dummy++ & 3; for (int i = 0; i < kPacketBytes; i++) p.data[i] = (uint8_t)(i * 37 + k); }
        }
        // sound: 20 packets of 64 samples per frame
        uint32_t prs = 0x5A5A1234u ^ (uint32_t)k;
        for (int i = 0; i < 20; i++) {
            Packet& p = pk[0][(size_t)(4 * i + 1)];
            p.address = kSoundAddress; p.ci = ct.sound++ & 3;
            p.data[0] = kPtBc1;
            int16_t x[64];
            for (int j = 0; j < 64; j++) { x[j] = tone[toneAt]; toneAt = (toneAt + 1) % tone.size(); }
            soundEncode(x, false, p.data + 1);
            if (c.scrambled) for (int j = 1; j < kPacketBytes; j++) { prs = prs * 1103515245u + 12345u; p.data[j] ^= (uint8_t)(prs >> 16); }
        }
        // service identification: the network data group and the TV service description, every frame
        auto put = [&](size_t slot, const std::vector<Packet>& g) {
            for (size_t i = 0; i < g.size() && slot + 4 * i < kPacketsPerFrame; i++) { Packet p = g[i]; p.ci = ct.si++ & 3; pk[0][slot + 4 * i] = p; }
        };
        put(2, siDataGroup(0, ct.dg[0]++, networkCmds()));
        put(42, siDataGroup(1, ct.dg[1]++, tvCmds()));
        if (c.scrambled)
            for (size_t slot : {22, 62}) {
                Packet& p = pk[0][slot];
                p.address = kEcmAddress; p.ci = ct.ecm++ & 3;
                for (int j = 0; j < kPacketBytes; j++) { prs = prs * 1103515245u + 12345u; p.data[j] = (uint8_t)(prs >> 16); }
            }
    }

    // ------------------------------------------------------------ the baseband
    void buildBaseband() {
        const int V = c.variant == kD ? kD : kD2;
        const int nFrames = c.loopFrames;
        const size_t lineN = kLineSamples, frameN = lineN * kLines, total = frameN * (size_t)nFrames;
        bb.assign(total, 0.5f);
        // the tone, J.17 pre-emphasised (run twice round the loop so the filter is settled)
        const size_t nTone = (size_t)nFrames * 1280;
        tone.assign(nTone, 0);
        {
            const double a = 8191.0 * std::pow(10.0, c.toneDbfs / 20.0);
            std::vector<double> x(nTone);
            for (size_t i = 0; i < nTone; i++) x[i] = a * std::sin(2 * kPi * c.toneHz * (double)i / 32000.0);
            Iir1 pre = j17Pre(32000);
            for (int pass = 0; pass < 2; pass++)
                for (size_t i = 0; i < nTone; i++) { const float y = pre.run((float)x[i]); if (pass) tone[i] = (int16_t)std::lround(std::max(-8192.f, std::min(8191.f, y))); }
        }
        Counters ct;
        const std::vector<uint8_t>& prbs = prbsFrame(V);
        const int per = line625Bits(V);
        const int W = card->width();
        std::vector<float> fy[2], fu[2], fv[2];
        uint32_t cut = 0x1234567u ^ c.seed;
        auto at = [&](size_t frameBase, int line, int s) -> float& {   // sample s (may be <= 0: the previous line) of line (1 based)
            long idx = (long)(frameBase + (size_t)(line - 1) * lineN) + s - 1;
            const long T = (long)total;
            idx = ((idx % T) + T) % T;
            return bb[(size_t)idx];
        };
        std::vector<uint8_t> sub[2];
        std::vector<float> lineBuf(lineN);
        for (int k = 0; k < nFrames; k++) {
            const size_t fb = (size_t)k * frameN;
            const double t = k / 25.0;
            for (int fl = 0; fl < 2; fl++) {
                fy[fl].assign((size_t)W * kFieldRows, 0.f); fu[fl].assign(fy[fl].size(), 0.f); fv[fl].assign(fy[fl].size(), 0.f);
                card->renderField(fl, t + fl * 0.02, (uint64_t)k, fy[fl].data(), fu[fl].data(), fv[fl].data());
            }
            // the packet multiplex of this frame
            std::vector<Packet> pk[2];
            framePackets(k, ct, pk);
            for (int s = 0; s < subframes(V); s++) {
                sub[s].assign((size_t)kDataLines * kSubframeBits, 0);
                for (int n = 0; n < kPacketsPerFrame; n++) packetBits(pk[s][(size_t)n], sub[s].data() + (size_t)n * kPacketBits);
            }
            for (int L = 1; L <= kLines; L++) {
                // ---- vision and the fixed levels (samples 1..1296 of this line)
                for (size_t s = 0; s < lineN; s++) lineBuf[s] = 0.5f;
                const bool f1 = L >= kField1First && L <= 310, f2 = L >= kField2First && L <= 622;
                if (f1 || f2) {
                    const int fl = f1 ? 0 : 1, first = f1 ? kField1First : kField2First;
                    const int r = L - first;                    // luminance row of this line (0 = the black reference line)
                    const int rc_ = r + 1;                      // the colour difference of this line belongs to the next line's row
                    float* Y = &lineBuf[kLumaFirst - 1];
                    float* C = &lineBuf[kChromaFirst - 1];
                    for (int j = 0; j < kLumaN; j++) {
                        if (r == 0) { Y[j] = 0.f; continue; }
                        const double x = (j + 0.5) * W / (double)kLumaN - 0.5;
                        const int x0 = std::max(0, std::min(W - 2, (int)std::floor(x)));
                        const float a = (float)std::max(0.0, std::min(1.0, x - x0));
                        const float* row = &fy[fl][(size_t)r * W];
                        Y[j] = std::max(0.f, std::min(1.f, row[x0] * (1 - a) + row[x0 + 1] * a));
                    }
                    const bool isU = chromaIsU(L);
                    for (int n = 0; n < kChromaN; n++) {
                        if (rc_ >= kFieldRows) { C[n] = 0.5f; continue; }
                        const double lj = 2.0 * n - 4;   // the luminance sample it is co-sited with
                        const double x = (lj + 0.5) * W / (double)kLumaN - 0.5;
                        const int x0 = std::max(0, std::min(W - 2, (int)std::floor(x)));
                        const float a = (float)std::max(0.0, std::min(1.0, x - x0));
                        const float* row = isU ? &fu[fl][(size_t)rc_ * W] : &fv[fl][(size_t)rc_ * W];
                        const float e = row[x0] * (1 - a) + row[x0 + 1] * a;   // BT.470 E'U or E'V
                        const float m = isU ? kUScale * e / 0.493f : kVScale * e / 0.877f;
                        C[n] = 0.5f + std::max(-0.5f, std::min(0.5f, m));
                    }
                    // transitions: clamp -> colour difference, colour difference -> luminance, luminance -> grey
                    for (int i = 0; i < 10; i++) lineBuf[225 + i] = 0.5f + (C[0] - 0.5f) * rc(i, 10);
                    for (int i = 0; i < 5; i++) lineBuf[584 + i] = C[kChromaN - 1] + (Y[0] - C[kChromaN - 1]) * rc(i, 5);
                    for (int i = 0; i < 6; i++) lineBuf[1286 + i] = Y[kLumaN - 1] + (0.5f - Y[kLumaN - 1]) * rc(i, 6);
                    if (c.scrambled) {   // single-cut line rotation of the active part (236 .. 1286)
                        cut = cut * 1664525u + 1013904223u;
                        const int cp = 53 + (int)((cut >> 8) % 254);
                        const int n = 1286 - 236 + 1;
                        std::vector<float> tmp(lineBuf.begin() + 235, lineBuf.begin() + 235 + n);
                        for (int i = 0; i < n; i++) lineBuf[(size_t)(235 + i)] = tmp[(size_t)((i + cp) % n)];
                    }
                } else if (L == 624) {   // grey, white, black references with 1/8, 1/2, 7/8 transitions
                    auto seg = [&](int s0, int s1, float lv) { for (int s = s0; s <= s1; s++) lineBuf[(size_t)(s - 1)] = lv; };
                    seg(210, 372, 0.5f); seg(372, 534, 1.f); seg(534, 696, 0.f);
                    const float tw[3] = {0.125f, 0.5f, 0.875f};
                    for (int i = 0; i < 3; i++) {
                        lineBuf[(size_t)(370 + i)] = 0.5f + 0.5f * tw[i];
                        lineBuf[(size_t)(532 + i)] = 1.f - tw[i];
                        lineBuf[(size_t)(694 + i)] = 0.5f * tw[i];
                    }
                }
                if (L != 625) for (size_t s = 0; s < lineN; s++) at(fb, L, (int)s + 1) = lineBuf[s];
                // ---- the data burst
                const bool w1 = ((L & 1) ^ (k & 1)) != 0;
                const int nb = L == 625 ? per : burstBits(V);
                std::vector<uint8_t> A((size_t)nb + 1, 1);
                for (int i = 1; i <= 6; i++) A[(size_t)i] = (uint8_t)lswBit(w1, i - 1);
                if (L <= kDataLines) {
                    for (int s = 0; s < subframes(V); s++)
                        for (int i = 0; i < kSubframeBits; i++) {
                            const int bit = 7 + s * kSubframeBits + i;
                            A[(size_t)bit] = sub[s][(size_t)(L - 1) * kSubframeBits + i] ^ prbs[(size_t)(L - 1) * per + (bit - 7)];
                        }
                } else if (L == 624) {
                    const int cmEnd = nb, cm0 = nb - 31;
                    for (int i = 7; i < cm0; i++) A[(size_t)i] = (uint8_t)((i - 7) & 1);
                    for (int i = cm0; i <= cmEnd; i++) A[(size_t)i] = (uint8_t)((kClampMarker >> (31 - (i - cm0))) & 1);
                } else {   // line 625
                    const bool inv = (k & 1) != 0;
                    for (int i = 0; i < 96; i++) A[(size_t)(7 + i)] = (uint8_t)(fsdBit(i) ^ (inv ? 1 : 0));
                    if (inv) for (int i = 1; i <= 6; i++) A[(size_t)i] ^= 1;
                    Sdf sdf;
                    sdf.chid = c.chid;
                    sdf.mvscg = (uint8_t)(0x1 | 0x2 | 0x4 | (c.wide ? 0 : 0x8) | 0x10 | ((c.scrambled ? 0x6 : 0x1) << 5));
                    sdf.cafcnt = (uint32_t)(k >> 8);
                    Rdf r[5];
                    r[0] = {0x01, k & 255, false, 0, 622, 1023, 1023, 9, 205, (k & 1) != 0};
                    r[1] = {0x10, k & 255, false, 22, 309, 334, 621, 231, 583, (k & 1) != 0};
                    r[2] = {0x11, k & 255, false, 22, 309, 334, 621, 585, 1285, (k & 1) != 0};
                    r[3] = {0x00, k & 255, false, 1023, 1023, 1023, 1023, 2047, 2047, false};
                    r[4] = r[3];
                    uint8_t sp[546];
                    line625Encode(sdf, r, 0x1F, sp);
                    for (int i = 0; i < 546; i++) A[(size_t)(103 + i)] = sp[i];
                }
                // duobinary: B = ~A xor B(prev), C = 2B - 1, d = (C + C(prev)) / 2; extreme levels = logic 1
                std::vector<float> d((size_t)nb + 3, 0.f);   // d[i] for bit i (1 based), zero around
                int B = 0;
                for (int i = 1; i <= nb; i++) {
                    const int Bn = (A[(size_t)i] ? 0 : 1) ^ B;
                    d[(size_t)i] = 0.5f * ((2 * Bn - 1) + (2 * B - 1));
                    B = Bn;
                }
                const int sp_ = bitSpacing(V);
                for (int i = 1; i <= nb; i++) at(fb, L, bitSample(V, i)) = 0.5f + kDataAmp * d[(size_t)i];
                if (sp_ == 2)   // the transition samples between bits (Part 1 4.4.2), and one either side
                    for (int i = 0; i <= nb; i++) {
                        auto D = [&](int j) { return j >= 1 && j <= nb ? d[(size_t)j] : 0.f; };
                        const float m = 9.f / 16 * (D(i) + D(i + 1)) - 1.f / 16 * (D(i - 1) + D(i + 2));
                        at(fb, L, bitSample(V, i) + 1) = 0.5f + kDataAmp * m;
                    }
            }
        }
    }

    // ------------------------------------------------------------ FM and the radio
    void buildRf() {
        const size_t N = bb.size();
        // pre-emphasis around the clamp level, settled by a first pass round the loop
        std::vector<float> x(N);
        Iir1 pre = preEmphasis(kClockHz);
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i < N; i++) { const float y = c.preEmphasis ? pre.run(bb[i] - 0.5f) : bb[i] - 0.5f; if (pass) x[i] = y; }
        if (c.dispersal) {   // 25 Hz triangle, frame synchronous
            const double a = 0.5 * kDispersalHzPp / kDevHzPerV;
            const size_t fr = (size_t)kLineSamples * kLines;
            for (size_t i = 0; i < N; i++) {
                const double ph = (double)(i % fr) / (double)fr;
                x[i] += (float)(a * (ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph));
            }
        }
        // x2 up to 40.5 Msps (windowed sinc, cyclic)
        const int H = 16;
        std::vector<float> h(H);
        for (int k = 0; k < H; k++) { const double tau = k + 0.5; h[(size_t)k] = (float)(sinc(tau * 0.92) * 0.92 * kaiserW(tau / (H + 0.5), 6.0)); }
        { double s = 0; for (float v : h) s += 2 * v; for (float& v : h) v = (float)(v / s); }
        const size_t N2 = 2 * N;
        std::vector<cf32> fm(N2);
        double ph = 0;
        const double kph = 2 * kPi * kDevHzPerV / kFm;
        for (size_t n = 0; n < N; n++) {
            for (int half = 0; half < 2; half++) {
                double v;
                if (!half) v = x[n];
                else {
                    double s = 0;
                    for (int k = 0; k < H; k++) s += h[(size_t)k] * (x[(n + N - (size_t)k) % N] + x[(n + 1 + (size_t)k) % N]);
                    v = s;
                }
                fm[2 * n + (size_t)half] = cf32((float)std::cos(ph), (float)std::sin(ph));
                ph += kph * v;
            }
            if (ph > 1e4 || ph < -1e4) { /* keep precision: fold whole turns */ const double t = std::floor(ph / (2 * kPi)) * 2 * kPi; ph -= t; }
        }
        loopRot = std::polar(1.0, ph);   // the phase the loop leaves behind (mod 2 pi)
        if (c.invert) { for (auto& z : fm) z = std::conj(z); loopRot = std::conj(loopRot); }
        // the radio's filter and the output rate (cyclic, the FM phase carried across the wrap)
        const double bw = c.radioBwHz > 0 ? c.radioBwHz : std::min(0.96 * c.rate, 36e6);
        const double fcN = 0.5 * bw / kFm;      // cut-off in cycles per input sample
        const int L = 24, P = 256;
        std::vector<float> tab((size_t)P * 2 * L);
        for (int p = 0; p < P; p++) {
            const double fr = (double)p / P;
            double s = 0;
            for (int t = 0; t < 2 * L; t++) {
                const double tau = (t - (L - 1)) - fr;
                const double v = 2 * fcN * sinc(2 * fcN * tau) * kaiserW(tau / L, 7.0);
                tab[(size_t)p * 2 * L + t] = (float)v; s += v;
            }
            for (int t = 0; t < 2 * L; t++) tab[(size_t)p * 2 * L + t] = (float)(tab[(size_t)p * 2 * L + t] / s);
        }
        const double loopSec = (double)N / kClockHz;
        const size_t M = (size_t)std::llround(loopSec * c.rate);
        loop.assign(M, cf32(0, 0));
        const double ratio = kFm / c.rate;
        const std::complex<float> rotF((float)loopRot.real(), (float)loopRot.imag()), rotB = std::conj(rotF);
        double pw = 0;
        for (size_t m = 0; m < M; m++) {
            const double u = (double)m * ratio;
            const long j0 = (long)std::floor(u);
            const int p = std::min(P - 1, (int)((u - j0) * P + 0.5));
            const float* w = &tab[(size_t)p * 2 * L];
            cf32 acc(0, 0);
            for (int t = 0; t < 2 * L; t++) {
                long j = j0 - (L - 1) + t;
                cf32 z;
                if (j < 0) z = fm[(size_t)(j + (long)N2)] * rotB;
                else if (j >= (long)N2) z = fm[(size_t)(j - (long)N2)] * rotF;
                else z = fm[(size_t)j];
                acc += w[t] * z;
            }
            loop[m] = acc;
            pw += std::norm(acc);
        }
        const float g = (float)(1.0 / std::sqrt(pw / (double)M + 1e-20));
        for (auto& z : loop) z *= g;
    }

    void generate(cf32* out, size_t n) {
        // the carrier offset and the phase the loop leaves behind, as one float rotator (renormalised now and then)
        const cf32 w((float)std::cos(2 * kPi * c.cfoHz / c.rate), (float)std::sin(2 * kPi * c.cfoHz / c.rate));
        const cf32 lr((float)loopRot.real(), (float)loopRot.imag());
        cf32 r = rot;
        const float a = (float)c.amp;
        for (size_t i = 0; i < n; i++) {
            out[i] = loop[pos] * r * a;
            r *= w;
            if (++pos == loop.size()) { pos = 0; r *= lr; }
            if ((++nrm & 1023) == 0) r /= std::abs(r);
        }
        rot = r;
        if (sigma > 0) noise->add(out, n, sigma);
    }
};

MacGenerator::MacGenerator(const MacGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
MacGenerator::~MacGenerator() = default;
void MacGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }
const std::vector<float>& MacGenerator::baseband() const { return p_->bb; }
int MacGenerator::loopFrames() const { return p_->c.loopFrames; }
void MacGenerator::referenceFrame(int k, std::vector<uint8_t>& rgba) const { p_->card->referenceFrame(k / 25.0, (uint64_t)k, rgba); }
double MacGenerator::noiseSigma(const MacGenConfig& c) {
    // carrier power amp^2 against the noise in 27 MHz; white over the output rate
    return c.amp * std::sqrt(c.rate / 27e6 / std::pow(10.0, c.snrDb / 10.0));
}

namespace {
class MacSynth : public ModeSynth {
public:
    MacSynth(const SynthConfig& cfg, double fs) {
        MacGenConfig c;
        c.rate = fs;
        c.variant = cfg.modeOpt[0] == 1 ? kD : kD2;
        c.scrambled = cfg.modeOpt[1] != 0;
        c.wide = cfg.modeOpt[2] != 0;
        c.pattern = std::max(0, std::min(2, cfg.modeOpt[3]));
        if (cfg.modeVal[0] > 0) c.toneHz = cfg.modeVal[0];
        c.snrDb = cfg.snrDb;
        c.cfoHz = cfg.cfoHz;
        gen_ = std::make_unique<MacGenerator>(c);
        fs_ = fs;
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    std::unique_ptr<MacGenerator> gen_;
    double fs_;
};
} // namespace

std::unique_ptr<ModeSynth> makeMacSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 8e6) return nullptr;
    return std::make_unique<MacSynth>(cfg, sampleRate);
}

} // namespace dect2

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) PPDU builder for the test signal: STF, LTF1, SIG, further LTFs (or D-STF / D-LTF / SIG-B of the long preamble) and the data
// field at any MCS, on the tone grid of the width (halow_phy.h). IEEE 802.11-2020 clause 23.3.8 - 23.3.11. The SERVICE field is 8 bits, the tail 6 bits
// (per encoder), the pad bits zero; the scrambler runs over all of them and the tail is zeroed again afterwards.
#include "dect2/halow/halow_phy.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace dect2::halow {

namespace {

// A cached FFT of size n for this thread.
Fft& fftN(int n) {
    static thread_local std::map<int, std::unique_ptr<Fft>> m;
    auto& p = m[n];
    if (!p) p = std::make_unique<Fft>(n);
    return *p;
}

// one OFDM symbol from its spectrum: inverse FFT and a cyclic prefix of gi samples
void addSymbol(std::vector<cf32>& out, cf32* X, float g, int N, int gi) {
    fftN(N).inverse(X);
    for (int i = N - gi; i < N; i++) out.push_back(X[i] * g);
    for (int i = 0; i < N; i++) out.push_back(X[i] * g);
}

// a phase slope across the tones: a stand-in for transmit beamforming (the receiver must take its channel from the D-LTF)
void tilt(cf32* X, int N, double rad) {
    if (rad == 0) return;
    for (int i = 0; i < N; i++) { const int k = i < N / 2 ? i : i - N; X[i] *= cf32((float)std::cos(rad * k), (float)std::sin(rad * k)); }
}

// Symbol spectrum from tone values: data tones (per copy of the layout), pilots as in 11a (+1 +1 +1 -1 pattern times the polarity), quad: QBPSK (data on the Q axis)
void fillSymbol(cf32* X, int N, const std::vector<Layout>& lays, const std::vector<cf32>& tones, int polarityIdx, bool quad) {
    for (int i = 0; i < N; i++) X[i] = cf32(0, 0);
    const int pol = pilotPolarity(polarityIdx);
    for (const Layout& l : lays) {
        for (size_t i = 0; i < l.dataK.size() && i < tones.size(); i++) if (l.dataK[i]) X[bin(l.dataK[i], N)] = quad ? cf32(-tones[i].imag(), tones[i].real()) : tones[i];
        for (size_t i = 0; i < l.pilotK.size(); i++) X[bin(l.pilotK[i], N)] = cf32((float)((i % 4 == 3 ? -1 : 1) * pol), 0);
    }
}

// info bits -> coded bits -> symbols of interleaved bits (rep: every ncbps/2 coded bits sent twice in a symbol, 1 MHz MCS 10 and the 1 MHz SIG)
std::vector<std::vector<uint8_t>> sigToSymbols(const std::vector<uint8_t>& infoBits, int ncbps, int ncol, int nrow, bool rep) {
    const std::vector<uint8_t> coded = convEncode(infoBits);
    const size_t blk = rep ? (size_t)ncbps / 2 : (size_t)ncbps;
    std::vector<std::vector<uint8_t>> syms;
    for (size_t at = 0; at < coded.size(); at += blk) {
        std::vector<uint8_t> raw;
        for (size_t i = 0; i < blk; i++) raw.push_back(at + i < coded.size() ? coded[at + i] : 0);
        if (rep) { const std::vector<uint8_t> c = raw; raw.insert(raw.end(), c.begin(), c.end()); }
        std::vector<uint8_t> il((size_t)ncbps);
        for (int k = 0; k < ncbps; k++) il[(size_t)interleave(ncol, nrow, k)] = raw[(size_t)k];
        syms.push_back(std::move(il));
    }
    return syms;
}

// coded bits of the data field in transmit order, for nes encoders: the data bits are dealt to the encoders one at a time, each gets 6 tail bits,
// the encoder outputs are punctured and then taken in blocks of s = max(1, nbpscs / 2) bits alternately (clause 23.3.11.3 - 23.3.11.5; unverified for nes > 1)
std::vector<uint8_t> encodeData(const std::vector<uint8_t>& scrambled, int nes, const Mcs& m) {
    if (nes <= 1) return puncture(convEncode(scrambled), m.num, m.den);
    std::vector<std::vector<uint8_t>> st((size_t)nes), co((size_t)nes);
    for (size_t i = 0; i < scrambled.size(); i++) st[i % (size_t)nes].push_back(scrambled[i]);
    for (int e = 0; e < nes; e++) { for (int t = 0; t < 6; t++) st[(size_t)e].push_back(0); co[(size_t)e] = puncture(convEncode(st[(size_t)e]), m.num, m.den); }
    const size_t s = (size_t)std::max(1, m.nbpscs / 2);
    std::vector<uint8_t> out;
    for (size_t at = 0; at < co[0].size(); at += s)
        for (int e = 0; e < nes; e++) for (size_t i = 0; i < s && at + i < co[(size_t)e].size(); i++) out.push_back(co[(size_t)e][at + i]);
    return out;
}

int presOf(const PpduSpec& s) { return s.pre >= 0 ? s.pre : (s.w == W1 ? Pre1M : PreShort); }
bool decodable(const PpduSpec& s) { return s.nss == 1 && !s.stbc && !s.ldpc && !s.ndp && !s.agg && dataBitsPerSym(s.w, s.mcs, 1) > 0; }
// Data symbols of a PPDU: none for NDP, the SIG length for A-MPDU, else from the PSDU length.
int nDataSyms(const PpduSpec& s) {
    if (s.ndp) return 0;
    if (s.agg) return std::max(0, std::min(511, (int)s.psdu.size()));
    return dataSymbols(s.w, s.mcs, (int)s.psdu.size(), s.nss, s.stbc);
}
int streamsOf(const PpduSpec& s) { return s.stbc ? s.nss * 2 : s.nss; }

} // namespace

int dataBitsPerSym(const PpduSpec& s) { return s.ndp ? 0 : dataBitsPerSym(s.w, s.mcs, s.nss); }

// Samples of the whole PPDU on the grid of its width.
size_t ppduSamples(const PpduSpec& s) {
    const WidthInfo& wi = info(s.w);
    const size_t N = (size_t)nfft(s.w), full = N + N / 4;
    const size_t ltf = s.w == W1 ? (size_t)wi.ltfSyms * full : N / 2 + 2 * N;
    const int pre = presOf(s);
    return (size_t)wi.stfSamples + ltf + (size_t)(wi.sigSyms + extraSyms(pre, streamsOf(s) - 1)) * full + (size_t)nDataSyms(s) * (size_t)symLen(s.w, s.sgi);
}

// The PPDU: STF, LTF1, SIG, further LTFs (or D-STF / D-LTF / SIG-B) and the data field (clause 23.3.8).
std::vector<cf32> buildPpdu(const PpduSpec& s) {
    const int w = s.w, N = nfft(w), gi = N / 4, pre = presOf(s);
    const WidthInfo& wi = info(w);
    std::vector<cf32> out;
    out.reserve(ppduSamples(s));
    std::vector<cf32> X((size_t)N), L((size_t)N);
    // STF
    stfTime(w, X.data());
    for (int i = 0; i < wi.stfSamples; i++) out.push_back(X[(size_t)(i % N)]);
    // LTF1: four GI + LTS symbols at 1 MHz, else a double guard interval and two LTS
    ltfTime(w, L.data());
    if (w == W1) {
        for (int k = 0; k < wi.ltfSyms; k++) { for (int i = N - gi; i < N; i++) out.push_back(L[(size_t)i]); for (int i = 0; i < N; i++) out.push_back(L[(size_t)i]); }
    } else {
        for (int i = N / 2; i < N; i++) out.push_back(L[(size_t)i]);
        for (int r = 0; r < 2; r++) for (int i = 0; i < N; i++) out.push_back(L[(size_t)i]);
    }
    // SIG
    Sig sg;
    sg.w = w; sg.pre = pre; sg.mcs = s.mcs; sg.len = s.agg ? nDataSyms(s) : (int)s.psdu.size(); sg.agg = s.agg; sg.nsts = streamsOf(s) - 1; sg.stbc = s.stbc;
    sg.ldpc = s.ldpc; sg.sgi = s.sgi; sg.id = s.id; sg.ul = s.ul; sg.ndp = s.ndp; sg.beamformed = s.beamformed;
    const auto sigSyms = sigToSymbols(sigBits(sg), wi.sigNcbps, wi.sigNcol, wi.sigNrow, w == W1);
    const float g = symScale(w);
    int symIdx = 0;
    std::vector<cf32> tones;
    for (size_t j = 0; j < sigSyms.size(); j++) {
        tones.clear();
        for (uint8_t b : sigSyms[j]) tones.push_back(cf32(b ? 1.f : -1.f, 0.f));
        fillSymbol(X.data(), N, wi.sig, tones, symIdx++, pre == PreLong && j == 1);   // S1G_LONG: the second SIG-A symbol is rotated (QBPSK), the way the long preamble is told apart
        addSymbol(out, X.data(), g, N, gi);
    }
    // further LTFs (S1G_SHORT, S1G_1M with more than one stream) or D-STF, D-LTFs and SIG-B (S1G_LONG)
    const int nltf = ltfCount(sg.nsts);
    auto ltfSym = [&]() {   // one LTF symbol (guard + LTS), with the beamforming tilt when asked
        std::vector<float> lf((size_t)N);
        ltfSpectrum(w, lf.data());
        std::vector<cf32> sp((size_t)N);
        for (int i = 0; i < N; i++) sp[(size_t)i] = cf32(lf[(size_t)i], 0.f);
        tilt(sp.data(), N, s.beamTilt);
        addSymbol(out, sp.data(), g, N, gi);
    };
    if (pre == PreLong) {
        stfSpectrum(w, X.data());
        tilt(X.data(), N, s.beamTilt);
        const float gs = stfScale(w);
        fftN(N).inverse(X.data());   // one 40 us period-8 us symbol: the STF is periodic, take N + N/4 samples
        for (int i = 0; i < N + gi; i++) out.push_back(X[(size_t)((i + N - gi) % N)] * gs);
        for (int k = 0; k < nltf; k++) ltfSym();
        std::vector<uint8_t> pb = scramblerSeq(0x2B, (size_t)wi.nsd);   // SIG-B: one BPSK symbol of fixed bits (its content is not read)
        tones.clear();
        for (uint8_t b : pb) tones.push_back(cf32(b ? 1.f : -1.f, 0.f));
        fillSymbol(X.data(), N, {wi.data}, tones, symIdx++, false);
        tilt(X.data(), N, s.beamTilt);
        addSymbol(out, X.data(), g, N, gi);
    } else {
        for (int k = 1; k < nltf; k++) { ltfTime(w, L.data()); for (int i = N - gi; i < N; i++) out.push_back(L[(size_t)i]); for (int i = 0; i < N; i++) out.push_back(L[(size_t)i]); }
    }
    // data
    const int nsym = nDataSyms(s);
    if (nsym <= 0) return out;
    const int nb = mcsInfo(s.mcs).nbpscs, sgiGi = s.sgi ? N / 8 : gi;
    const int ncbps = wi.nsd * nb;
    std::vector<std::vector<uint8_t>> symBits;
    if (decodable(s)) {
        const Mcs& m = mcsInfo(s.mcs);
        const int nd = dataBitsPerSym(w, s.mcs, 1), nes = encoders(w, s.mcs, 1);
        std::vector<uint8_t> bits((size_t)nsym * (size_t)nd, 0);
        for (size_t i = 0; i < s.psdu.size(); i++) for (int b = 0; b < 8; b++) bits[8 + i * 8 + (size_t)b] = (uint8_t)(s.psdu[i] >> b & 1);
        std::vector<uint8_t> coded;
        if (nes == 1) {
            const std::vector<uint8_t> sc = scramblerSeq(s.seed ? s.seed : 0x5D, bits.size());
            for (size_t i = 0; i < bits.size(); i++) bits[i] ^= sc[i];
            for (size_t i = 0; i < 6; i++) bits[8 + s.psdu.size() * 8 + i] = 0;   // the tail follows the PSDU; the pad after it stays scrambled
            coded = encodeData(bits, 1, m);
        } else {
            bits.resize((size_t)nsym * (size_t)nd - 6 * (size_t)nes);
            const std::vector<uint8_t> sc = scramblerSeq(s.seed ? s.seed : 0x5D, bits.size());
            for (size_t i = 0; i < bits.size(); i++) bits[i] ^= sc[i];
            coded = encodeData(bits, nes, m);
        }
        const size_t blk = m.rep ? (size_t)ncbps / 2 : (size_t)ncbps;
        for (size_t at = 0; at + blk <= coded.size() && (int)symBits.size() < nsym; at += blk) {
            std::vector<uint8_t> raw(coded.begin() + (ptrdiff_t)at, coded.begin() + (ptrdiff_t)(at + blk));
            if (m.rep) { const std::vector<uint8_t> c = raw; raw.insert(raw.end(), c.begin(), c.end()); }
            std::vector<uint8_t> il((size_t)ncbps);
            for (int k = 0; k < ncbps; k++) il[(size_t)interleaveJ(wi.ncol, wi.nrow * nb, ncbps, nb, k)] = raw[(size_t)k];
            symBits.push_back(std::move(il));
        }
    }
    const std::vector<uint8_t> fill = scramblerSeq(0x35, (size_t)ncbps * 3);
    for (int j = 0; j < nsym; j++) {
        tones.clear();
        if (j < (int)symBits.size()) {
            for (int t = 0; t < wi.nsd; t++) tones.push_back(mapQam(symBits[(size_t)j].data() + (size_t)t * (size_t)nb, nb));
        } else {   // streams, STBC, LDPC, A-MPDU symbols: filler that looks like data (QPSK from the scrambler sequence)
            for (int t = 0; t < wi.nsd; t++) { const uint8_t b2[2] = {fill[(size_t)((t * 2 + j) % (int)fill.size())], fill[(size_t)((t * 2 + 1 + 3 * j) % (int)fill.size())]}; tones.push_back(mapQam(b2, 2)); }
        }
        fillSymbol(X.data(), N, {wi.data}, tones, symIdx++, false);
        tilt(X.data(), N, s.beamTilt);
        addSymbol(out, X.data(), g, N, sgiGi);
    }
    return out;
}

// the old narrow entry points: one stream, BCC, no short GI; an MCS the width cannot send gives the preamble and SIG field only
std::vector<cf32> buildPpdu(const std::vector<uint8_t>& psdu, int w, int mcs, uint8_t seed, bool agg) {
    PpduSpec s;
    s.w = w; s.mcs = mcs; s.seed = seed; s.agg = agg; s.psdu = psdu;
    return buildPpdu(s);
}

size_t ppduSamples(int w, int mcs, int len) {
    PpduSpec s;
    s.w = w; s.mcs = mcs; s.psdu.assign((size_t)len, 0);
    return ppduSamples(s);
}

} // namespace dect2::halow

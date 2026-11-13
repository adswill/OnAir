// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OFDM symbol processing of the DVB-T2 receiver: FFT, timing and carrier tracking.
#include "t2rx_impl.h"

namespace dect2 {

void T2Receiver::Impl::processFrames() {
    const int N = fftN, G = guard;
    while (!frames.empty()) {
        bool stalled = false;
        while (!frames.empty() && frames.front().next < frames.front().maxSyms) {
            Frame& f = frames.front();
            if (f.next == 0) { gridOff += pendingGridOff; pendingGridOff = 0; symErased.assign((size_t)std::max(0, f.maxSyms), 0); }
            int64_t s = f.anchor + gridOff + (int64_t)std::llround((double)f.next * (N + G) * (1.0 + f.sro));
            if (f.lostAt >= 0 && f.lostKnown) s -= f.lost;
            if (s + N + G + 40 > end()) { stalled = true; break; }
            // A symbol whose cyclic prefix has stopped matching, right after one that matched: samples were lost here and every symbol after
            // sits earlier by that many. Hold the frame until the next P1 says exactly how many (or give up after two frames and go on as before).
            if (f.lostAt < 0 && f.next > 0 && frameLen > 0 && cpCorrAvg > 0.3 && cpRhoAt(s) < 0.3 * cpCorrAvg) {
                f.lostAt = f.next; f.lostFrom = s;
                if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] symbol %d of the frame lost its cyclic prefix: samples lost?\n", f.next);
                if (f.lostKnown) continue;   // the next P1 has already told how many
            }
            if (f.lostAt >= 0 && !f.lostKnown) {
                if (end() > s + 2 * (int64_t)frameLen + 4 * (N + G)) { f.lostKnown = true; f.lost = 0; f.intactAgain = true; if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] no P1 to say how many samples were lost (end %lld)\n", (long long)end()); }
                else { stalled = true; break; }
            }
            if (f.lostAt >= 0 && f.lostKnown && !f.intactAgain && f.next >= f.lostAt) {
                // the symbols whose samples straddle the gap or went with it: erased, until the first one entirely after the gap
                if (s < f.lostFrom || cpRhoAt(s) < 0.5 * cpCorrAvg) { eraseSymbol(f.next++); continue; }
                f.intactAgain = true;
            }
            int idx = f.next++;
            { StageClock sc(3); processSymbol(s, idx); } // may clear `frames` (loss of lock / guard-interval correction)
        }
        if (stalled || frames.empty()) break;
        if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] frame done: %d of %d symbols\n", frames.front().next, frames.front().maxSyms);
        frames.pop_front();
    }
}

// Copy N samples from `w0` into fr/fi, rotated by exp(j (ph0 + dph n)). A sin and cos per sample cost more than the FFT itself for a 16K or 32K
// symbol, so a phasor is stepped instead and re-anchored exactly every 256 samples (its error stays far below float precision).
void T2Receiver::Impl::derotate(int64_t w0, double ph0, double dph) {
    const int N = fftN;
    fr.assign(N, 0.f); fi.assign(N, 0.f);
    const cd step(std::cos(dph), std::sin(dph));
    // Four 256-sample blocks side by side: each block's phasor recurrence is a chain of dependent complex multiplies (about eight cycles a
    // sample one block at a time); four independent chains run in the time of one. Every sample gets exactly the phasor it got before.
    int n0 = 0;
    if (w0 >= base && w0 + N <= end()) {   // the window is inside the buffer: no bounds checks per sample
        const cf32* src = &buf[(size_t)(w0 - base)];
        for (; n0 + 4 * 256 <= N; n0 += 4 * 256) {
            cd z[4];
            for (int q = 0; q < 4; q++) { const double a = ph0 + dph * (n0 + 256 * q); z[q] = cd(std::cos(a), std::sin(a)); }
            for (int j = 0; j < 256; j++)
                for (int q = 0; q < 4; q++) {
                    const int n = n0 + 256 * q + j;
                    const cf32 x = src[n];
                    const float cs = (float)z[q].real(), sn = (float)z[q].imag();
                    fr[n] = x.real() * cs - x.imag() * sn;
                    fi[n] = x.real() * sn + x.imag() * cs;
                    z[q] *= step;
                }
        }
    }
    for (; n0 < N; n0 += 256) {
        const double a = ph0 + dph * n0;
        cd z(std::cos(a), std::sin(a));
        const int n1 = std::min(N, n0 + 256);
        for (int n = n0; n < n1; n++) {
            const cf32 x = at(w0 + n);
            const float cs = (float)z.real(), sn = (float)z.imag();
            fr[n] = x.real() * cs - x.imag() * sn;
            fi[n] = x.real() * sn + x.imag() * cs;
            z *= step;
        }
    }
}

void T2Receiver::Impl::fftCells(int64_t s, std::vector<cf32>& cells, int K) {
    StageClock sc(4);
    const int N = fftN, G = guard;
    int back = std::min(G / 4, 32);
    int64_t w0 = s + G - back;
    double dph = -kTwoPi * frameCfo / fn;
    double ph0 = dph * (double)(w0 - curAnchor);
    derotate(w0, ph0, dph);
    doFft((int)std::lround(std::log2((double)N)));
    cells.resize(K);
    for (int kk = 0; kk < K; kk++) {
        int f = kk - (K - 1) / 2;
        int b = (f + N) % N;
        cells[kk] = cf32(fr[b], fi[b]);
    }
}

double T2Receiver::Impl::cpRhoAt(int64_t s) const {
    const int N = fftN, G = guard;
    cd c = 0; double e = 0;
    for (int i = 0; i < G; i++) {
        const cf32 a = at(s + i), b = at(s + i + N);
        c += cd(a.real(), a.imag()) * std::conj(cd(b.real(), b.imag()));
        e += 0.5 * ((double)std::norm(a) + (double)std::norm(b));
    }
    return e > 0 ? std::abs(c) / e : 0.0;
}

// a symbol that went with lost samples: no cells (the data stage leaves it out of the channel estimate and gives its cells no weight)
void T2Receiver::Impl::eraseSymbol(int k) {
    if (k < (int)symErased.size()) symErased[(size_t)k] = 1;
    if (k < nP2) return;   // the P2 symbols carry L1: that frame cannot be decoded
    if (l1preOk && frameSyms > 0 && k < frameSyms) {
        if ((int)frameCells.size() != frameSyms) frameCells.assign(frameSyms, {});
        frameCells[k].assign(kMax, cf32(0, 0));
        if (k == frameSyms - 1) runDataStage();
    }
    symbols++;
    symCounter++;
}

void T2Receiver::Impl::processSymbol(int64_t s, int k) {
    const int N = fftN, G = guard;
    if (k == 0) { frameCfo = cfoEst; curAnchor = s; }
    // cyclic-prefix correlation, at the nominal position and at every offset in -D..D (for the timing error): the products are formed once
    // and the windows of G samples are slid along them, instead of summing G samples for each of the 2D+1 offsets
    {
    StageClock scCp(6);
    const int D = std::min(8, std::max(2, G / 8));
    const int Lw = G + 2 * D;
    cpP.resize(Lw); cpE.resize(Lw);
    for (int i = 0; i < Lw; i++) {
        const cf32 a = at(s - D + i), b = at(s - D + i + N);
        cpP[i] = cd(a.real(), a.imag()) * std::conj(cd(b.real(), b.imag()));
        cpE[i] = 0.5 * ((double)std::norm(a) + (double)std::norm(b));
    }
    cd cw = 0;
    double ew = 0;
    for (int i = 0; i < G; i++) { cw += cpP[i]; ew += cpE[i]; }
    double mags[17];
    cd c = 0;
    double e = 0;
    for (int dd = -D; dd <= D; dd++) {
        mags[dd + D] = ew > 0 ? std::abs(cw) / ew : 0;
        if (dd == 0) { c = cw; e = ew; }
        if (dd < D) { cw += cpP[dd + D + G] - cpP[dd + D]; ew += cpE[dd + D + G] - cpE[dd + D]; }
    }
    int best = D;
    for (int i = 0; i <= 2 * D; i++) if (mags[i] > mags[best]) best = i;
    double rho = e > 0 ? std::abs(c) / e : 0;
    cpCorrAvg += 0.1 * (rho - cpCorrAvg);
    if (rho > 0.15) {
        double epsCp = -std::arg(c) * fn / (kTwoPi * N);
        double bin = fn / N;
        double meas = epsCp + std::round((cfoEst - epsCp) / bin) * bin;
        cfoEst += 0.08 * (meas - cfoEst);
        lowCount = 0;
    } else if (++lowCount > 40) {
        state = 0; // lost the signal
        frames.clear();
        lowCount = 0;
        return;
    }
    // timing error from the CP correlation peak
    double off = best - D;
    if (best > 0 && best < 2 * D) {
        double a = mags[best - 1], b = mags[best], cc = mags[best + 1];
        double den = a - 2 * b + cc;
        if (den < 0) off += 0.5 * (a - cc) / den;
    }
    timingAvg += 0.05 * (off - timingAvg);
    if (std::fabs(timingAvg) > 6 && rho > 0.3) { pendingGridOff += (int64_t)std::llround(timingAvg); timingAvg = 0; } // applied at the next frame start
    }

    if (k < nP2) {
        if ((int)p2cells.size() != nP2) p2cells.assign(nP2, {});
        fftCells(s, p2cells[k], kMax);
        if (k == nP2 - 1) { StageClock sc(8); runP2Stage(); }
    }
    if (l1preOk && frameSyms > 0 && k < frameSyms && k >= 0) {
        if ((int)frameCells.size() != frameSyms) frameCells.assign(frameSyms, {});
        if (k >= nP2) fftCells(s, frameCells[k], kMax);
        else if (k < (int)p2cells.size()) frameCells[k] = p2cells[k];
        else frameCells[k].clear(); // runP2Stage() just reset the receiver (cleared the P2 cells)
        if (k == frameSyms - 1) runDataStage();
    }
    symbols++;
    symCounter++;
    // display: FFT consecutive symbol pairs, at a limited rate
    // (about 25 pairs a second is plenty for a constellation plot; with 32K symbols the old figure of 100 gave a stride of 2,
    // i.e. an extra full-size FFT with per-sample sin/cos on every single symbol)
    int stride = std::max(2, (int)std::lround(fn / (N + G) / 25.0));
    int phase = (int)(symCounter % stride);
    if (phase > 1) return;
    StageClock scDisp(7);
    int back = std::min(G / 4, 32);
    int64_t w0 = s + G - back;
    double ph0 = -kTwoPi * cfoEst * (double)(w0 - s) / fn;
    double dph = -kTwoPi * cfoEst / fn;
    derotate(w0, ph0, dph);
    doFft((int)std::lround(std::log2((double)N)));
    int K = carriers;
    std::vector<cf32> cells(K);
    double pw = 0;
    for (int kk = 0; kk < K; kk++) {
        int f = kk - (K - 1) / 2;
        int b = (f + N) % N;
        cells[kk] = cf32(fr[b], fi[b]);
        pw += std::norm(cells[kk]);
    }
    float nrm = (float)std::sqrt(std::max(1e-20, pw / K));
    for (auto& cl : cells) cl /= nrm;
    const size_t maxPts = 4096;
    size_t step = std::max<size_t>(1, K / maxPts);
    rawCells.clear();
    for (size_t i = 0; i < (size_t)K; i += step) rawCells.push_back(cells[i]);
    if (phase == 1 && prevCells.size() == (size_t)K) {
        diffCells.clear();
        for (size_t i = 0; i < (size_t)K; i += step) diffCells.push_back(cells[i] * std::conj(prevCells[i]));
    }
    prevCells = std::move(cells);
    (void)k;
}

} // namespace dect2

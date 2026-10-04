// OFDM symbol processing of the DVB-T2 receiver: FFT, timing and carrier tracking.
#include "t2rx_impl.h"

namespace dect2 {

void T2Receiver::Impl::processFrames() {
    const int N = fftN, G = guard;
    while (!frames.empty()) {
        bool stalled = false;
        while (!frames.empty() && frames.front().next < frames.front().maxSyms) {
            Frame& f = frames.front();
            if (f.next == 0) { gridOff += pendingGridOff; pendingGridOff = 0; }
            int64_t s = f.anchor + gridOff + (int64_t)std::llround((double)f.next * (N + G) * (1.0 + sro));
            if (s + N + G + 40 > end()) { stalled = true; break; }
            int idx = f.next++;
            { StageClock sc(3); processSymbol(s, idx); } // may clear `frames` (loss of lock / guard-interval correction)
        }
        if (stalled || frames.empty()) break;
        frames.pop_front();
    }
}

void T2Receiver::Impl::fftCells(int64_t s, std::vector<cf32>& cells, int K) {
    StageClock sc(4);
    const int N = fftN, G = guard;
    int back = std::min(G / 4, 32);
    int64_t w0 = s + G - back;
    fr.assign(N, 0.f); fi.assign(N, 0.f);
    double dph = -kTwoPi * frameCfo / fn;
    double ph0 = dph * (double)(w0 - curAnchor);
    for (int n = 0; n < N; n++) {
        cf32 x = at(w0 + n);
        double a = ph0 + dph * n;
        float cs = (float)std::cos(a), sn = (float)std::sin(a);
        fr[n] = x.real() * cs - x.imag() * sn;
        fi[n] = x.real() * sn + x.imag() * cs;
    }
    doFft((int)std::lround(std::log2((double)N)));
    cells.resize(K);
    for (int kk = 0; kk < K; kk++) {
        int f = kk - (K - 1) / 2;
        int b = (f + N) % N;
        cells[kk] = cf32(fr[b], fi[b]);
    }
}

void T2Receiver::Impl::processSymbol(int64_t s, int k) {
    const int N = fftN, G = guard;
    if (k == 0) { frameCfo = cfoEst; curAnchor = s; }
    // cyclic-prefix correlation
    auto cpCorr = [&](int64_t st, double& e) {
        cd c = 0;
        e = 0;
        for (int n = 0; n < G; n++) {
            cf32 a = at(st + n), b = at(st + n + N);
            c += cd(a.real(), a.imag()) * std::conj(cd(b.real(), b.imag()));
            e += 0.5 * (std::norm(cd(a.real(), a.imag())) + std::norm(cd(b.real(), b.imag())));
        }
        return c;
    };
    {
    StageClock scCp(6);
    double e;
    cd c = cpCorr(s, e);
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
    int D = std::min(8, std::max(2, G / 8));
    double mags[17];
    int best = D;
    for (int dd = -D; dd <= D; dd++) {
        double ee;
        cd cc = cpCorr(s + dd, ee);
        mags[dd + D] = ee > 0 ? std::abs(cc) / ee : 0;
        if (mags[dd + D] > mags[best]) best = dd + D;
    }
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
    fr.assign(N, 0.f); fi.assign(N, 0.f);
    double ph0 = -kTwoPi * cfoEst * (double)(w0 - s) / fn;
    double dph = -kTwoPi * cfoEst / fn;
    for (int n = 0; n < N; n++) {
        cf32 x = at(w0 + n);
        double a = ph0 + dph * n;
        float cs = (float)std::cos(a), sn = (float)std::sin(a);
        fr[n] = x.real() * cs - x.imag() * sn;
        fi[n] = x.real() * sn + x.imag() * cs;
    }
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

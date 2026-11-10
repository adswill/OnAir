// Analog TV picture decoder: turning a line into pixels (extractPicture), publishing fields and capturing lines.
#include "atv_video_impl.h"
#include "dect2/atv/atv_blend.h"

namespace dect2 {

void AtvVideo::Impl::extractPicture(double tL, int row, int field) {
    (void)field;
    const int W = this->W;                    // a copy: the byte stores below could change the member as far as the compiler knows, which keeps loops from vectorising
    const AtvFormat& F = fmt;
    const double g = lineGain();
    if (g <= 0) return;
    // the span of samples this line's active part needs, with margins for the filters
    const double a0 = tL + (F.blankEndUs - 1.0) * spu, a1 = tL + (F.blankEndUs + F.activeUs + 1.0) * spu;
    const long sa = (long)std::floor(a0) - 24, sb = (long)std::ceil(a1) + 24;
    const size_t len = (size_t)(sb - sa + 1);
    const bool useColour = (colourKind == kAtvPal || colourKind == kAtvNtsc) && !bpf.empty();
    const bool colourNow = useColour && colourLocked && !killer && colourCapable && prm.colourOn;
    const bool secamNow = colourKind == kAtvSecam && secamLineOk && !bpf.empty() && !killer && colourCapable && prm.colourOn;
    std::vector<float>& rawv = yv;
    rawv.resize(len);
    for (size_t k = 0; k < len; k++) rawv[k] = rv[(uint64_t)(sa + (long)k) & kMask];
    const double clampB = Bline;
    // luminance: the video without the colour subcarrier band (or all of it for a picture without colour)
    std::vector<float>& lum = lumBuf;
    lum.assign(rawv.begin(), rawv.end());
    std::vector<float>& zr = zi;
    std::vector<float>& zq_ = zq;
    size_t nz = 0;
    const int Dz = zdec;
    if (colourNow || secamNow) {
        const int nb = (int)bpf.size(), mb = nb / 2;
        bp.resize(len);
        std::fill(bp.begin(), bp.end(), 0.f);
        desamp(rawv.data(), 1, bpf.data(), bp.data() + mb, (int)(len - (size_t)nb + 1), nb);
        for (size_t k = 0; k < len; k++) lum[k] -= bp[k];
    }
    if (colourNow) {
        // chroma: mix the baseband down by the reference, low-pass at every Dz-th sample
        ti.resize(len); tq.resize(len);
        const double dph = 2 * kPi * mainLock.fsc / fv;
        const double ph0 = mainLock.ph + dph * ((double)sa - mainLock.tRef);
        // the I and Q rings of this line in order (the ring may wrap), then four phasors a sample apart, each stepped by four samples,
        // so that the multiplications do not wait for each other (explicit float arithmetic: the complex operators of the library are slow)
        yiL.resize(len); yqL.resize(len);
        {
            const size_t s0 = (size_t)((uint64_t)sa & kMask), first = std::min(len, kRing - s0);
            std::memcpy(yiL.data(), &ri[s0], first * sizeof(float)); std::memcpy(yqL.data(), &rq[s0], first * sizeof(float));
            if (first < len) { std::memcpy(yiL.data() + first, ri.data(), (len - first) * sizeof(float)); std::memcpy(yqL.data() + first, rq.data(), (len - first) * sizeof(float)); }
        }
        const std::complex<double> pd0 = std::polar(1.0, -ph0), rd = std::polar(1.0, -dph);
        float pr[4], pim[4];
        for (int q = 0; q < 4; q++) { const std::complex<float> v(pd0 * std::pow(rd, q)); pr[q] = v.real(); pim[q] = v.imag(); }
        const std::complex<float> r4(std::pow(rd, 4));
        const float r4r = r4.real(), r4i = r4.imag();
        size_t k = 0;
        for (; k + 4 <= len; k += 4) {
            for (int q = 0; q < 4; q++) {
                const float yi = yiL[k + (size_t)q], yq = yqL[k + (size_t)q];
                ti[k + (size_t)q] = yi * pr[q] - yq * pim[q];
                tq[k + (size_t)q] = yi * pim[q] + yq * pr[q];
                const float nr = pr[q] * r4r - pim[q] * r4i;
                pim[q] = pr[q] * r4i + pim[q] * r4r;
                pr[q] = nr;
            }
            if ((k & 127) == 124) for (int q = 0; q < 4; q++) { const float m = 1.f / std::sqrt(pr[q] * pr[q] + pim[q] * pim[q]); pr[q] *= m; pim[q] *= m; }
        }
        for (int q = 0; k + (size_t)q < len; q++) {
            const float yi = yiL[k + (size_t)q], yq = yqL[k + (size_t)q];
            ti[k + (size_t)q] = yi * pr[q] - yq * pim[q];
            tq[k + (size_t)q] = yi * pim[q] + yq * pr[q];
        }
        const int nt = (int)lpC.size(), m = nt / 2;
        // outputs for k = m, m + Dz, ...: z[j] belongs to sample m + j Dz
        nz = (len - (size_t)nt) / (size_t)Dz + 1;
        zr.resize(nz); zq_.resize(nz);
        desamp(ti.data(), Dz, lpC.data(), zr.data(), (int)nz, nt);
        desamp(tq.data(), Dz, lpC.data(), zq_.data(), (int)nz, nt);
        zoff = (double)m;
    }
    // pixels
    const double pxUs = F.activeUs / W;
    const float setup = (float)F.setup, scl = 1.f / (1.f - setup);
    const double advSamp = F.chromaDelayNs * 1e-3 * spu;                      // the transmitter advanced the chrominance: look later
    const float gsat = prm.saturation * scl;
    const float ch = std::cos((float)(prm.hueDeg * kPi / 180)), sh = std::sin((float)(prm.hueDeg * kPi / 180));
    const bool pal = F.pal, hue = prm.hueDeg != 0;
    const float accGain = colourNow ? (float)std::min(3.0, std::max(0.33, 1.0 / std::max(0.05, burstEma))) : 1.f;
    const float invG = (float)(1.0 / g);
    // Sync compression (a transmitter or modulator that squeezes the sync pulses) makes the sync-referenced gain too high. The burst is as
    // high as the sync pulse in every system here (300 mV against 300 mV, 40 IRE against 40 IRE), so a burst that is much larger than that
    // gives the gain instead.
    const float invGY = invG / (float)compK;
    uint8_t* dst = &fbuf[(size_t)row * W * 4];
    float whitePk = 0;
    const bool avgPrev = pal && prevValid && prevLineNo + 1 == curNo;
    // luminance at the pixels: 8-tap windowed sinc, positions stepped along the line
    const float stepY = (float)(pxUs * spu), pos0 = (float)(tL + (F.blankEndUs + 0.5 * pxUs) * spu - (double)sa);
    yRowP.resize((size_t)W);
    {
        const float* kt = kern.data();
        const float* lp = lum.data();
        float* out = yRowP.data();
        float pos = pos0;
        for (int x = 0; x < W; x++, pos += stepY) {
            const int i0 = (int)pos;
            const int ph = (int)((pos - (float)i0) * (float)kInterpPhases + 0.5f);
            const float* k = kt + (size_t)ph * kInterpTaps;
            const float* q = lp + i0 - (kInterpTaps / 2 - 1);
            // four partial sums (the eight taps in two halves), so that the additions do not wait for each other
            const float a0 = k[0] * q[0] + k[4] * q[4], a1 = k[1] * q[1] + k[5] * q[5], a2 = k[2] * q[2] + k[6] * q[6], a3 = k[3] * q[3] + k[7] * q[7];
            out[x] = (a0 + a1) + (a2 + a3);
        }
    }
    // chrominance at every second pixel (cubic), linear in between
    if (colourNow) {
        cuRow.resize((size_t)W + 2); cvRow.resize((size_t)W + 2);
        // every second pixel (and one more at each end): the position in the filtered chrominance and the four cubic weights first
        // (plain arithmetic over arrays, which vectorises), then the sums
        const double advPos = advSamp, invDz = 1.0 / Dz;
        const int nx = (W + 2 + 1) / 2;
        czP.resize((size_t)nx); czW.resize((size_t)nx * 4); czIdx.resize((size_t)nx);
        int* czI = czIdx.data();
        float* czF = czP.data();
        const float pzBase = (float)(((double)pos0 + advPos - zoff) * invDz), pzStep = (float)((double)stepY * invDz);
        for (int j = 0; j < nx; j++) {
            const int xc = std::min(2 * j, W - 1);
            const float pz = pzBase + (float)xc * pzStep;
            const int i0 = (int)pz;
            czF[j] = pz - (float)i0;
            czI[j] = std::min(std::max(i0 - 1, 0), (int)nz - 4);
        }
        float* cw = czW.data();
        for (int j = 0; j < nx; j++) {
            const float fr = czF[j], t2 = fr * fr, t3 = t2 * fr;
            cw[4 * j] = -0.5f * t3 + t2 - 0.5f * fr; cw[4 * j + 1] = 1.5f * t3 - 2.5f * t2 + 1.f;
            cw[4 * j + 2] = -1.5f * t3 + 2.f * t2 + 0.5f * fr; cw[4 * j + 3] = 0.5f * t3 - 0.5f * t2;
        }
        const float* zrp = zr.data(); const float* zqp = zq_.data();
        const float gU = invG * accGain, gV = invG * accGain * (pal ? (float)vsign : 1.f);
        for (int j = 0; j < nx; j++) {
            const int x = 2 * j;
            const float* a = zrp + czI[j]; const float* b = zqp + czI[j];
            const float w0 = cw[4 * j], w1 = cw[4 * j + 1], w2 = cw[4 * j + 2], w3 = cw[4 * j + 3];
            const float re = w0 * a[0] + w1 * a[1] + w2 * a[2] + w3 * a[3], im = w0 * b[0] + w1 * b[1] + w2 * b[2] + w3 * b[3];
            // z in the raw domain, p = -z / G: z_p = sV - jU for a locked reference
            cuRow[(size_t)x] = im * gU;
            cvRow[(size_t)x] = -re * gV;
            if (x + 1 < W + 2) { cuRow[(size_t)x + 1] = 0; cvRow[(size_t)x + 1] = 0; }
        }
        for (int x = 1; x < W; x += 2) { cuRow[(size_t)x] = 0.5f * (cuRow[(size_t)x - 1] + cuRow[(size_t)x + 1]); cvRow[(size_t)x] = 0.5f * (cvRow[(size_t)x - 1] + cvRow[(size_t)x + 1]); }
        for (int x = 0; x < W; x++) { uRow[(size_t)x] = cuRow[(size_t)x]; vRow[(size_t)x] = cvRow[(size_t)x]; }
    } else if (secamNow) {
        cuRow.resize((size_t)W); cvRow.resize((size_t)W);
        for (int x = 0; x < W; x++) { cuRow[(size_t)x] = secU[(size_t)x]; cvRow[(size_t)x] = secV[(size_t)x]; }
    }
    const float* __restrict yp = yRowP.data();
    if (colourNow || secamNow) {
        // one pass over the row: luminance, chrominance (with the previous line on PAL and the hue rotation), the BT.470 matrix (see atvYuvToRgb)
        // and the bytes. Without the previous line the chrominance is averaged with itself, which leaves it as it is.
        const float cY = (float)clampB, ky = invGY * scl, ko = setup * scl;
        const float* __restrict cu = cuRow.data(); const float* __restrict cv = cvRow.data();
        const float* __restrict pu = avgPrev ? prevU.data() : cuRow.data(); const float* __restrict pv = avgPrev ? prevV.data() : cvRow.data();
        const float hs = hue && !secamNow ? sh : 0.f, hc = hue && !secamNow ? ch : 1.f;
        uint8_t* __restrict o = dst;
        // the white peak apart from the pixels (a running maximum would keep the compiler from vectorising the loop), in four lanes
        float m4[4] = {0, 0, 0, 0};
        for (int x = 0; x + 4 <= W; x += 4) for (int l = 0; l < 4; l++) m4[l] = std::max(m4[l], (cY - yp[x + l]) * ky - ko);
        float wp = std::max(std::max(m4[0], m4[1]), std::max(m4[2], m4[3]));
        for (int x = W & ~3; x < W; x++) wp = std::max(wp, (cY - yp[x]) * ky - ko);
        for (int x = 0; x < W; x++) {
            const float Y = (cY - yp[x]) * ky - ko;
            const float u0 = 0.5f * (cu[x] + pu[x]) * gsat, v0 = 0.5f * (cv[x] + pv[x]) * gsat;
            const float U = u0 * hc - v0 * hs, V = u0 * hs + v0 * hc;
            const float r = (Y + 1.1403f * V) * 255.f + 0.5f, g = (Y - 0.3939f * U - 0.5808f * V) * 255.f + 0.5f, b = (Y + 2.0284f * U) * 255.f + 0.5f;
            o[4 * x] = (uint8_t)std::min(255.f, std::max(0.f, r));
            o[4 * x + 1] = (uint8_t)std::min(255.f, std::max(0.f, g));
            o[4 * x + 2] = (uint8_t)std::min(255.f, std::max(0.f, b));
            o[4 * x + 3] = 255;
        }
        whitePk = std::max(whitePk, wp);
    } else {
        uint8_t* __restrict o = dst;
        const float cB = (float)clampB;
        float m4[4] = {whitePk, whitePk, whitePk, whitePk};
        for (int x = 0; x + 4 <= W; x += 4) for (int l = 0; l < 4; l++) m4[l] = std::max(m4[l], ((cB - yp[x + l]) * invGY - setup) * scl);
        float wp = std::max(std::max(m4[0], m4[1]), std::max(m4[2], m4[3]));
        for (int x = W & ~3; x < W; x++) wp = std::max(wp, ((cB - yp[x]) * invGY - setup) * scl);
        for (int x = 0; x < W; x++) {
            float Y = (cB - yp[x]) * invGY;
            Y = (Y - setup) * scl;
            const uint8_t v8 = to8(Y);
            o[4 * x] = v8; o[4 * x + 1] = v8; o[4 * x + 2] = v8; o[4 * x + 3] = 255;
        }
        whitePk = wp;
    }
    if (colourNow && pal) { prevU = uRow; prevV = vRow; prevValid = true; prevLineNo = curNo; }
    else prevValid = false;
    whiteTrack = std::max(whiteTrack * 0.999f, whitePk);
    whiteLast = std::max(whiteLast, whitePk);
    anyPicture = true;
    // (SECAM: a line the decoder could not identify stays without colour, but the picture is a colour picture)
    lastColourNow = colourNow || secamNow || (colourKind == kAtvSecam && colourLocked && !killer && colourCapable && prm.colourOn);
}

void AtvVideo::Impl::publish(int field) {
    if (!owner || !owner->frameReady || W <= 0) return;
    auto f = std::make_shared<AtvFrame>();
    f->seq = ++frameSeq;
    f->width = W; f->height = Hh;
    f->fieldRate = fmt.fieldHz;
    f->colour = lastColourNow;
    f->field = field;
    f->rgba = fbuf;
    if (prm.deinterlace == 1) {            // bob: this field's rows, the others made from their neighbours
        for (int r = 0; r < Hh; r++) {
            if ((r & 1) == field) continue;
            const int a = r > 0 ? r - 1 : r + 1, b = r + 1 < Hh ? r + 1 : r - 1;
            for (int x = 0; x < W * 4; x++) f->rgba[(size_t)r * W * 4 + (size_t)x] = (uint8_t)(((int)fbuf[(size_t)a * W * 4 + (size_t)x] + (int)fbuf[(size_t)b * W * 4 + (size_t)x]) / 2);
        }
    }
    temporalFilter(*f);
    doFrame(f);
}

// Temporal noise reduction: a recursive average over the pictures, per pixel and per colour, that lets go where the picture changed by more
// than noise does (motion, a cut). Automatic: the weight of a new picture follows the measured video S/N (none above 30 dB, 0.12 at 8 dB and below).
void AtvVideo::Impl::temporalFilter(AtvFrame& f) {
    double k = 1.0;
    double sig = 0.02;
    if (gainValid) sig = std::sqrt(std::max(noiseVar, 1e-30)) / std::max(1e-9, lineGain());
    const double snr = 20 * std::log10(1.0 / std::max(sig, 1e-6));
    if (prm.denoise < 0) k = snr >= 30 ? 1.0 : std::max(0.12, 0.12 + 0.88 * (snr - 8.0) / 22.0 * (snr - 8.0) / 22.0);
    else if (prm.denoise == 1) k = 0.5; else if (prm.denoise == 2) k = 0.3; else if (prm.denoise >= 3) k = 0.15;
    if (k >= 0.999 || f.rgba.size() != tfPrev.size()) {      // off, or the picture size changed: start again from this picture
        tfPrev = f.rgba;
        return;
    }
    const double T = 5.0 * sig * 255.0 + 8.0;
    atvTemporalBlend(f.rgba.data(), tfPrev.data(), f.rgba.size() & ~(size_t)3, k, T);
    tfPrev = f.rgba;
}

// ------------------------------------------------------------------------------ scope
void AtvVideo::Impl::captureLine(double tL) {
    const double g = lineGain();
    if (g <= 0) return;
    const int n = 512;
    lineWave.assign((size_t)n, 0.f);
    const double a = tL - 2.0 * spu, span = P;
    for (int k = 0; k < n; k++) {
        const double t = a + span * k / n;
        const long i0 = (long)std::floor(t);
        const double fr = t - (double)i0;
        const double r = rvAt((uint64_t)i0) * (1 - fr) + rvAt((uint64_t)i0 + 1) * fr;
        lineWave[(size_t)k] = (float)((Bline - r) / g);
    }

}

// A blanking line for the VITS analyser: the whole line period from the sync's leading edge, at the input rate, in IRE (blanking 0, white 100).
// Each line number is averaged over 16 fields before it is handed over. The scale comes from the line's own sync pulse and blanking level (as a VITS analyser measures against the sync amplitude), so the
// receiver's slow level tracking does not enter the measurement.
void AtvVideo::Impl::vitsLine(double tL, int lineNo) {
    if (vitsAcc.size() >= 32 && !vitsAcc.count(lineNo)) return;
    const int n = (int)std::lround(P);
    if (n < 16 || spu <= 0) return;
    std::vector<double> raw((size_t)n);
    for (int k = 0; k < n; k++) {
        const double t = tL + k;
        const long i0 = (long)std::floor(t);
        const double fr = t - (double)i0;
        raw[(size_t)k] = rvAt((uint64_t)i0) * (1 - fr) + rvAt((uint64_t)i0 + 1) * fr;
    }
    auto mean = [&](double a, double b) {   // microseconds from the sync's leading edge
        const int i0 = std::max(0, (int)(a * spu)), i1 = std::min(n, (int)(b * spu));
        double s = 0; for (int i = i0; i < i1; i++) s += raw[(size_t)i];
        return i1 > i0 ? s / (i1 - i0) : 0.0;
    };
    const double tip = mean(0.8, 4.0), blank = mean(8.8, 10.2);   // the sync tip, and the back porch after the colour burst
    if (std::fabs(blank - tip) < 1e-9) return;
    std::vector<double>& acc = vitsAcc[lineNo];
    if (acc.size() != (size_t)n) { acc.assign((size_t)n, 0.0); vitsN[lineNo] = 0; }   // a new line length (another standard or rate): start again
    for (int k = 0; k < n; k++) acc[(size_t)k] += 100.0 * fmt.syncLevel * (blank - raw[(size_t)k]) / (blank - tip);
    if (++vitsN[lineNo] < 16) return;
    std::vector<float> v((size_t)n);
    for (int k = 0; k < n; k++) v[(size_t)k] = (float)(acc[(size_t)k] / 16);
    vitsPub[lineNo] = std::move(v);
    acc.assign((size_t)n, 0.0);
    vitsN[lineNo] = 0;
}

// The ghost report: blanking lines without data, averaged (a ghost of the sync pulse shows on them, and no picture can look like one)
void AtvVideo::Impl::ghostLine(double tL) {
    const double g = lineGain();
    if (g <= 0) return;
    const int n = 512;
    if (ghostAcc.size() != (size_t)n) { ghostAcc.assign((size_t)n, 0.f); ghostN = 0; }
    const double a = tL - 2.0 * spu;
    for (int k = 0; k < n; k++) {
        const double t = a + P * k / n;
        const long i0 = (long)std::floor(t);
        const double fr = t - (double)i0;
        ghostAcc[(size_t)k] += (float)((Bline - (rvAt((uint64_t)i0) * (1 - fr) + rvAt((uint64_t)i0 + 1) * fr)) / g);
    }
    if (++ghostN >= 48) {
        std::vector<float> avg((size_t)n);
        for (int k = 0; k < n; k++) avg[(size_t)k] = ghostAcc[(size_t)k] / ghostN;
        ghost = atvFindGhost(avg, 2.0, n * spu / P, P / spu > 90 ? 9.0 : P / spu < 55 ? 2.5 : 4.7);
        std::fill(ghostAcc.begin(), ghostAcc.end(), 0.f); ghostN = 0;
    }
}

void AtvVideo::Impl::captureVbi() {
    const double g = lineGain();
    const int n = 512;
    vbiWave.assign((size_t)n, 0.f);
    const double a = vsyncT - 3 * P, span = 15 * P;
    if (nAbs < (uint64_t)(a + span) || a < (double)nAbs - (double)kRing + 100) return;
    for (int k = 0; k < n; k++) {
        const double t0 = a + span * k / n, t1 = a + span * (k + 1) / n;
        double s = 0; int c = 0;
        for (uint64_t m = (uint64_t)t0; m < (uint64_t)t1; m++) { s += rvAt(m); c++; }
        vbiWave[(size_t)k] = c ? (float)((Bline - s / c) / g) : 0.f;
    }
}

} // namespace dect2

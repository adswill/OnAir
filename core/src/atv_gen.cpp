// Analog TV test signal, see atv_gen.h.
//
//   test card (field by field) -> composite video line by line at 12 Msps, timing from the half-line grid of BT.470-6
//   -> carrier amplitude (sync tip 100 %, white 12.5 %) -> vestigial-sideband FIR (complex taps) -> carrier at the vision frequency
//   -> interpolation to the output rate (position stepped in double precision, so a clock error is just a different step)
//   + FM sound carrier made at the output rate, ghost, noise.
#include "dect2/atv_gen.h"
#include "dect2/atv_card.h"
#include "atv_dsp.h"
#include "dect2/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {
using namespace atvdsp;
constexpr double kFi = AtvGenerator::kInternalRate;
constexpr int kVsbTaps = 57;
constexpr int kKernTaps = 12, kKernPhases = 512;
constexpr int kSecamNb = 256, kSecamPad = 16, kSecamGTaps = 33;    // SECAM chrominance is made at 4 Msps (256 samples a line), around 4.286 MHz
constexpr double kSecamFb = 4e6;

// Raised-cosine step, 0 -> 1, centred on x = 0; rise10_90 is the 10 to 90 % build-up time (a raised cosine takes 0.59 of its length for that)
inline float edge(float x, float rise10_90) {
    const float te = rise10_90 / 0.59f;
    if (x <= -0.5f * te) return 0.f;
    if (x >= 0.5f * te) return 1.f;
    return 0.5f * (1.f + sinHalf((float)kPi * x / te));
}

// Sound programme: a 1 kHz tone with short gaps, or a melody. Returns the audio sample (before pre-emphasis) and the gain of the
// pre-emphasis at its frequency, so the FM deviation is what the standard asks for.
struct Programme {
    int mode = 0;
    double preUs = 50;
    double amp = 0.5;
    static constexpr double kNotes[16] = {261.63, 261.63, 392.00, 392.00, 440.00, 440.00, 392.00, 0, 349.23, 349.23, 329.63, 329.63, 293.66, 293.66, 261.63, 0};
    double gain(double f) const { const double w = 2 * kPi * f * preUs * 1e-6; return std::sqrt(1 + w * w); }
    // audio at time t, multiplied by the pre-emphasis gain
    // sin(2 pi x) for any x (a Taylor series on the quarter turn, error below 1e-10)
    static double sinTurns(double x) {
        double r = x - std::nearbyint(x);                            // -0.5 .. 0.5 turns
        r = r > 0.25 ? 0.5 - r : r < -0.25 ? -0.5 - r : r;
        const double y = 2 * kPi * r, y2 = y * y;
        return y * (1 + y2 * (-1.0 / 6 + y2 * (1.0 / 120 + y2 * (-1.0 / 5040 + y2 * (1.0 / 362880 + y2 * (-1.0 / 39916800 + y2 * (1.0 / 6227020800.0 + y2 * (-1.0 / 1307674368000.0))))))));
    }
    double at(double t) const {
        double f = 1000, env = 1;
        if (mode == 0) { const double u = t * (1.0 / 1.5); env = (u - std::floor(u)) * 1.5 < 1.3 ? 1 : 0; }
        else if (mode == 1) {
            const double ts = t / 0.35;
            const double fl = std::floor(ts);
            const int note = (int)(fl - 16.0 * std::floor(fl * (1.0 / 16.0)));
            const double x = ts - fl;
            f = kNotes[note];
            env = f > 0 ? std::min(1.0, std::min(x, 1 - x) * 20) : 0;
        } else if (mode != 4) return 0;
        if (f <= 0 || env <= 0) return 0;
        return amp * env * gain(f) * sinTurns(f * t);
    }
};

} // namespace

struct AtvGenerator::Impl {
    AtvGenConfig cfg;
    AtvFormat fmt;
    bool ok = false;
    std::unique_ptr<AtvCard> card;
    std::function<void(const float*, size_t)> tap;

    // line timing in exact integers: a line lasts hNum / hDen internal samples
    int64_t hNum = 768, hDen = 1;
    uint64_t lineIdx = 0;

    // picture of the current frame, per field
    std::vector<float> fy[2], fu[2], fv[2];
    int64_t fieldFrame[2] = {-1, -1};
    std::vector<float> padY, padU, padV, lineV, lineY, lineU, lineC;
    // What the picture part of a line needs at each of its samples does not depend on the line, only on the offset of the first sample
    // (1 class on 625 lines, 3 on 525): the position in the picture row, the interpolation weights, the blanking edges, the subcarrier turn.
    struct PicTab {
        bool built = false;
        int j0 = 0, j1 = 0;                  // samples of the line the tables cover
        std::vector<int> iy, ic;             // index into the padded luminance row (first of four) and into the chrominance rows (first of two)
        std::vector<float> w4, tc, g;        // the four luminance weights per sample, the chrominance fraction, the gate
        std::vector<float> cT, sT;           // cos and sin of j times the subcarrier step
    };
    std::vector<PicTab> picTab;
    std::vector<float> pY, pU, pV;

    // carrier amplitude, then the filter, then the carrier
    std::vector<float> a;            // a[0] is sample aBase
    int64_t aBase = 0;
    int64_t firNext = 0;             // next sample to filter
    std::vector<float> hR, hI, tR, tI;
    std::vector<float> xr, xi;       // filtered and on the carrier (real and imaginary part); xr[0] is sample xvBase
    int64_t xvBase = 0;
    double carPhase = 0;

    // resampler
    std::vector<float> kern;         // (kKernPhases + 1) x kKernTaps
    double pos = 64, step = 1;
    uint64_t outCount = 0;

    // SECAM chrominance (625 lines only): FM at baseband around the centre of the bell, anti-bell filter, up to 12 Msps, mixed up
    std::vector<float> secGR, secGI;                 // anti-bell at 4 Msps, taps reversed (a correlation)
    std::vector<float> secPoly[3];                   // interpolation 4 -> 12 Msps (gain 3), the three polyphase branches
    int secMu = 0, secS = 0, secPad = 0;
    double secPhase = 0;                             // FM phase at baseband (radians), continuous from line to line
    std::vector<float> secC0R, secC0I, secC1R, secC1I;

    // sound, ghost, noise
    Programme prog;
    double sndPh = 0;                                // the sound carrier phase, radians (a running sum, reduced only where it is used)
    std::vector<float> sPh;
    std::vector<double> sPhD;
    uint64_t audioBlock = ~0ull;
    double audioA0 = 0, audioA1 = 0, audioDt = 0;
    std::vector<cf32> ghost;
    size_t ghostPos = 0;
    NoiseSource noise;
    std::vector<cf32> blk;

    explicit Impl(const AtvGenConfig& c) : cfg(c), noise(c.seed) {
        if (!atvMakeFormat(c.sys, c.colour, fmt)) return;
        if (c.rate < 1e6 || c.rate > 64e6) return;
        if (!c.setup && fmt.setup > 0) fmt.setup = 0;
        if (!c.groupDelay) fmt.chromaDelayNs = 0;
        // exact line length in internal samples: 64 us * 12 MHz = 768, 286 / 4.5 MHz * 12 MHz = 2288 / 3
        if (fmt.lines == 625) { hNum = 768; hDen = 1; } else { hNum = 2288; hDen = 3; }
        card = std::make_unique<AtvCard>(fmt, c.pattern);
        picTab.assign((size_t)hDen, PicTab());
        for (int f = 0; f < 2; f++) {
            const size_t n = (size_t)fmt.fieldRows * fmt.picW;
            fy[f].assign(n, 0.f); fu[f].assign(n, 0.f); fv[f].assign(n, 0.f);
        }
        designVsb();
        designKernel();
        if (fmt.secam) designSecam();
        a.assign((size_t)kVsbTaps / 2, 0.f); aBase = -(int64_t)kVsbTaps / 2;   // silence before the start
        firNext = 0;
        applyRuntime(c, true);
        ok = true;
    }

    void designVsb() {
        const double bv = std::min(fmt.videoBwMhz, 5.4) * 1e6, ro = 0.5e6, vs = fmt.vestigialMhz * 1e6;
        const bool nyq = cfg.nyquistTx;
        auto resp = [&](double f) -> std::complex<double> {
            const double af = std::fabs(f);
            double lp = 1;
            if (af > bv) lp = af >= bv + ro ? 0 : std::pow(std::cos(kPi / 2 * (af - bv) / ro), 2);
            double v;
            if (nyq) v = f >= vs ? 1 : f <= -vs ? 0 : 0.5 + 0.5 * std::sin(kPi * f / (2 * vs));
            else v = f >= -vs ? 1 : f <= -vs - ro ? 0 : std::pow(std::cos(kPi / 2 * (-vs - f) / ro), 2);
            return lp * v;
        };
        const auto t = fromResponse(resp, kFi, kVsbTaps, 6.0);
        hR.assign(kVsbTaps, 0.f); hI.assign(kVsbTaps, 0.f);
        for (int k = 0; k < kVsbTaps; k++) {      // correlation form: hh[k] = h(lag = m - k) = t[N - 1 - k]; make the symmetry exact
            const auto p = t[(size_t)(kVsbTaps - 1 - k)], q = t[(size_t)k];
            hR[(size_t)k] = (float)(0.5 * (p.real() + q.real()));
            hI[(size_t)k] = (float)(0.5 * (p.imag() - q.imag()));
        }
    }

    void designSecam() {
        const double fc = kSecamBellHz;
        auto resp = [&](double f) -> std::complex<double> { return atvSecamHfPreEmph(fc + f); };      // baseband f -> subcarrier fc + f
        const auto t = fromResponse(resp, kSecamFb, kSecamGTaps, 5.0);
        // anti-bell as a correlation with reversed taps, real and imaginary parts apart
        secGR.assign(t.size(), 0.f); secGI.assign(t.size(), 0.f);
        for (size_t k = 0; k < t.size(); k++) { secGR[k] = (float)t[t.size() - 1 - k].real(); secGI[k] = (float)t[t.size() - 1 - k].imag(); }
        // interpolation 4 -> 12 Msps (gain 3) as three polyphase branches: output j uses the taps t = r + 3 s with r = (j + mu) % 3
        const std::vector<float> up = lowpass(1.5e6, 2.5e6, kFi, 50, 61);
        secMu = (int)up.size() / 2;
        secS = ((int)up.size() + 2) / 3;
        for (int r = 0; r < 3; r++) {
            secPoly[r].assign((size_t)secS, 0.f);
            for (int q = 0; q < secS; q++) if (r + 3 * q < (int)up.size()) secPoly[r][(size_t)q] = 3.f * up[(size_t)(r + 3 * q)];
        }
        secPad = secS + 2;
        secC0R.assign((size_t)(kSecamNb + 2 * kSecamPad), 0.f); secC0I = secC0R;
        secC1R.assign((size_t)(kSecamNb + 2 * secPad), 0.f); secC1I = secC1R;
    }

    // The chrominance of one line, added to lineV. fld, k: the picture row (fld < 0: not a picture line).
    // D'R or D'B (alternating line by line) -> low-frequency pre-emphasis -> frequency modulation of the subcarrier -> anti-bell -> blanking
    void secamChroma(int line, uint64_t li, int fld, int k, int64_t nStart, int count, float u0, float dtUs) {
        const AtvFormat& F = fmt;
        const bool isR = (li & 1) == 0;
        const bool bottle = cfg.secamIdent != 1 && F.secamBottleLine(line);
        const bool lead = cfg.secamIdent != 2;
        if (fld < 0 && !bottle) return;
        const int W = F.picW;
        const double f0 = isR ? kSecamF0R : kSecamF0B, dev = isR ? kSecamDevR : kSecamDevB;
        const float* plane = fld >= 0 ? (isR ? &fv[fld][(size_t)k * W] : &fu[fld][(size_t)k * W]) : nullptr;
        const double dscale = isR ? -1.902 / 0.877 : 1.505 / 0.493;          // D' = dscale * (V or U)
        // D' at 4 Msps, then the pre-emphasis (bilinear transform of (1 + s/w1) / (1 + s/(3 w1)))
        const double a = 2 * kSecamFb / (2 * kPi * kSecamLf1Hz);
        const double b0 = 1 + a, b1 = 1 - a, a0 = 1 + a / 3, a1 = 1 - a / 3;
        double xPrev = 0, yPrev = 0;
        const double dfRest = f0 - kSecamBellHz;
        const double lo = (isR ? kSecamMinR : kSecamMinB) - kSecamBellHz, hi = (isR ? kSecamMaxR : kSecamMaxB) - kSecamBellHz;
        double ph = secPhase;                                // phase at the start of the line: continues from the end of the last one
        const int N0 = kSecamNb + 2 * kSecamPad;
        for (int i = -kSecamPad; i < kSecamNb + kSecamPad; i++) {
            double D = 0;
            const double t = i * 0.25;                       // us after 0H
            if (i >= 0 && i < kSecamNb) {
                if (plane && t >= F.blankEndUs && t < F.blankEndUs + F.activeUs) {
                    const double p = (t - F.blankEndUs) / F.activeUs * W - 0.5;
                    const int ip = (int)std::floor(p);
                    const double fr = p - ip;
                    const double v0 = plane[std::min(W - 1, std::max(0, ip))], v1 = plane[std::min(W - 1, std::max(0, ip + 1))];
                    D = dscale * (v0 + fr * (v1 - v0));
                } else if (bottle) {
                    D = isR ? kSecamBottleR * std::min(1.0, t / 15.0) : kSecamBottleB * std::min(1.0, t / 18.0);
                }
            }
            double y = D;
            if (i >= 0 && i < kSecamNb) { y = (b0 * D + b1 * xPrev - a1 * yPrev) / a0; xPrev = D; yPrev = y; }
            const double df = std::min(hi, std::max(lo, dfRest + dev * y));
            ph += 2 * kPi * df / kSecamFb;
            if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi;
            if (i == kSecamNb - 1) secPhase = ph;
            const float x = (float)ph;
            float yc = x + 1.5707963f;
            if (yc > 3.1415927f) yc -= 6.2831855f;
            secC0R[(size_t)(i + kSecamPad)] = sinPi(yc);     // cos
            secC0I[(size_t)(i + kSecamPad)] = sinPi(x);
        }
        // anti-bell: a correlation with the reversed taps, 16 outputs at a time held in registers
        const int m = kSecamGTaps / 2;
        float accR[kSecamNb], accI[kSecamNb];
        for (int i = 0; i < kSecamNb; i++) { accR[i] = 0; accI[i] = 0; }
        for (int t = 0; t < kSecamGTaps; t++) {
            const float gr = secGR[(size_t)t], gi = secGI[(size_t)t];
            const float* xs = &secC0R[(size_t)(kSecamPad - m + t)];
            const float* ys = &secC0I[(size_t)(kSecamPad - m + t)];
            for (int i = 0; i < kSecamNb; i++) { accR[i] += gr * xs[i] - gi * ys[i]; accI[i] += gr * ys[i] + gi * xs[i]; }
        }
        for (int i = 0; i < kSecamNb; i++) { secC1R[(size_t)(secPad + i)] = accR[i] * (float)kSecamM0; secC1I[(size_t)(secPad + i)] = accI[i] * (float)kSecamM0; }
        (void)N0;
        // 4 -> 12 Msps, mixed up to the subcarrier, blanked
        const double tOn = (bottle || lead) ? kSecamLeadInUs : F.blankEndUs, tOff = F.lineUs - F.frontPorchUs;
        const float tr = 0.3f;
        const double w = 2 * kPi * kSecamBellHz / kFi;
        const int64_t nm = (nStart * 4286) % 12000;                        // exact: fc / fs = 4286 / 12000
        std::complex<double> rot0 = std::polar(1.0, 2 * kPi * (double)nm / 12000.0);
        const std::complex<float> rot((float)std::cos(w), (float)std::sin(w));
        std::complex<float> p((float)rot0.real(), (float)rot0.imag());
        const int jOn = std::max(0, (int)std::floor((tOn - 0.3 / 0.59 - u0) / dtUs)), jOff = std::min(count, (int)std::ceil((tOff + 0.3 / 0.59 - u0) / dtUs) + 1);
        // advance the carrier phasor to jOn
        p *= std::complex<float>(std::polar(1.0, w * (double)jOn));
        for (int j = jOn; j < jOff; j++) {
            const float u = u0 + j * dtUs;
            const float g = edge(u - (float)tOn, tr) - edge(u - (float)tOff, tr);
            if (g > 0) {
                const int r = (j + secMu) % 3, idx0 = (j + secMu - r) / 3;       // the branch and the newest input it uses
                const float* h = secPoly[r].data();
                const float* cr = &secC1R[(size_t)(secPad + idx0 - secS + 1)];
                const float* ci = &secC1I[(size_t)(secPad + idx0 - secS + 1)];
                float yr = 0, yi = 0;
                for (int q = 0; q < secS; q++) { yr += h[q] * cr[secS - 1 - q]; yi += h[q] * ci[secS - 1 - q]; }
                lineV[(size_t)j] += g * (yr * p.real() - yi * p.imag());
            }
            p *= rot;
            if ((j & 255) == 255) p /= std::abs(p);
        }
        (void)li;
    }

    void designKernel() {
        kern.assign((size_t)(kKernPhases + 1) * kKernTaps, 0.f);
        for (int p = 0; p <= kKernPhases; p++) {
            const double fr = (double)p / kKernPhases;
            double sum = 0;
            double w[kKernTaps];
            for (int m = 0; m < kKernTaps; m++) {
                const double d = fr - (m - (kKernTaps / 2 - 1));          // x(P) = sum x[i0 + mm] w(fr - mm), mm = m - 5
                const double sinc = d == 0 ? 1.0 : std::sin(kPi * d) / (kPi * d);
                w[m] = sinc * kaiser(d / (kKernTaps / 2), 6.5);
                sum += w[m];
            }
            for (int m = 0; m < kKernTaps; m++) kern[(size_t)p * kKernTaps + m] = (float)(w[m] / sum);
        }
    }

    void applyRuntime(const AtvGenConfig& c, bool first) {
        cfg.cnrDb = c.cnrDb; cfg.cfoHz = c.cfoHz; cfg.sroPpm = c.sroPpm;
        cfg.echoDb = c.echoDb; cfg.echoDelayUs = c.echoDelayUs; cfg.echoPhaseDeg = c.echoPhaseDeg;
        cfg.syncCompression = c.syncCompression; cfg.humPct = c.humPct; cfg.humHz = c.humHz;
        cfg.sound = c.sound; cfg.level = c.level;
        step = kFi / cfg.rate / (1.0 + cfg.sroPpm * 1e-6);
        prog.mode = cfg.sound; prog.preUs = fmt.preEmphUs;
        const size_t d = (size_t)std::max(1.0, std::round(cfg.echoDelayUs * 1e-6 * cfg.rate));
        if (cfg.echoDb > 0 && ghost.size() != d) { ghost.assign(d, cf32(0, 0)); ghostPos = 0; }
        (void)first;
    }

    // ---- the picture of one field
    void ensureField(int fld, uint64_t frame) {
        if (fieldFrame[fld] == (int64_t)frame) return;
        const double tf = 1.0 / fmt.fieldHz;
        const double t = cfg.startSec + (double)frame * fmt.lines * fmt.lineUs * 1e-6 + fld * tf;
        card->renderField(fld, t, frame, fy[fld].data(), fu[fld].data(), fv[fld].data());
        fieldFrame[fld] = (int64_t)frame;
    }

    // the tables of the picture part for lines whose first sample is u0 microseconds after the line start (see PicTab)
    void buildPicTab(PicTab& T, float u0, float dtUs, float aStart, float aEnd, float trB, float teB, float pxPerUs, float advPx, int W, int count) {
        (void)count;
        const AtvFormat& F = fmt;
        T.j0 = std::max(0, (int)std::floor((aStart - teB - u0) / dtUs));
        T.j1 = (int)std::ceil((aEnd + teB - u0) / dtUs) + 1;
        const int n = std::max(0, T.j1 - T.j0);
        T.iy.resize((size_t)n); T.ic.resize((size_t)n); T.w4.resize((size_t)n * 4); T.tc.resize((size_t)n); T.g.resize((size_t)n);
        T.cT.resize((size_t)n); T.sT.resize((size_t)n);
        const double dphi = (F.pal || F.ntsc) ? 2 * kPi * F.fscHz / kFi : 0.0;
        for (int q = 0; q < n; q++) {
            const int j = T.j0 + q;
            const float u = u0 + j * dtUs;
            const float p = (u - aStart) * pxPerUs - 0.5f;
            const float pf = std::floor(p), t = p - pf;
            const int i = (int)pf + 2;
            T.iy[(size_t)q] = std::min(std::max(i - 1, 0), W);
            const float t2 = t * t, t3 = t2 * t;
            T.w4[(size_t)q * 4] = -0.5f * t3 + t2 - 0.5f * t;
            T.w4[(size_t)q * 4 + 1] = 1.5f * t3 - 2.5f * t2 + 1.f;
            T.w4[(size_t)q * 4 + 2] = -1.5f * t3 + 2.f * t2 + 0.5f * t;
            T.w4[(size_t)q * 4 + 3] = 0.5f * t3 - 0.5f * t2;
            const float pc = p + advPx, pcf = std::floor(pc);
            T.tc[(size_t)q] = pc - pcf;
            T.ic[(size_t)q] = std::min(std::max((int)pcf + 2, 0), W + 2);
            T.g[(size_t)q] = (u > aStart + 0.5f * teB && u < aEnd - 0.5f * teB) ? 1.f : edge(u - aStart, trB) - edge(u - aEnd, trB);
            T.cT[(size_t)q] = (float)std::cos(j * dphi);
            T.sT[(size_t)q] = (float)std::sin(j * dphi);
        }
        T.built = true;
    }

    // ---- one line of composite video, appended as carrier amplitude to a[]
    void renderLine() {
        const AtvFormat& F = fmt;
        const uint64_t li = lineIdx++;
        const int L0 = (int)(li % (uint64_t)F.lines);
        const uint64_t frame = li / (uint64_t)F.lines;
        const int line = L0 + 1;
        const int64_t nStart = (int64_t)((li * (uint64_t)hNum + (uint64_t)hDen - 1) / (uint64_t)hDen);
        const int64_t nEnd = (int64_t)(((li + 1) * (uint64_t)hNum + (uint64_t)hDen - 1) / (uint64_t)hDen);
        const int count = (int)(nEnd - nStart);
        const double rem = (double)((int64_t)nStart * hDen - (int64_t)li * hNum);     // 0 .. hDen - 1, in units of 1/hDen sample
        const float u0 = (float)(rem / hDen / kFi * 1e6);                              // offset of the first sample after the line start, us
        const float dtUs = (float)(1e6 / kFi);
        const float Hus = (float)F.lineUs;
        const int h0 = 2 * L0;
        lineV.assign((size_t)count, 0.f);

        // synchronising pulses: at the line start, in the middle and (the leading edge only) at the next line start
        const float S = (float)-F.syncLevel, trS = (float)F.syncRiseUs;
        const int type[3] = {F.pulseAt(h0), F.pulseAt(h0 + 1), F.pulseAt(h0 + 2)};
        for (int pi = 0; pi < 3; pi++) {
            if (!type[pi]) continue;
            const float w = type[pi] == 1 ? (float)F.syncUs : type[pi] == 2 ? (float)F.eqUs : (float)F.broadUs;
            const float t0 = pi * 0.5f * Hus, t1 = t0 + w, te = trS / 0.59f;
            const int ja = std::max(0, (int)std::floor((t0 - te - u0) / dtUs)), jb = std::min(count, (int)std::ceil((t1 + te - u0) / dtUs) + 1);
            for (int j = ja; j < jb; j++) {
                const float u = u0 + j * dtUs;
                lineV[(size_t)j] -= S * (edge(u - t0, trS) - edge(u - t1, trS));
            }
        }
        // the picture
        int fld = -1, k = 0;
        if (type[0] == 1 && type[1] == 0) {
            for (int ff = 0; ff < 2; ff++) {
                const int d = h0 - F.visibleStartH(ff);
                if (d >= 0 && (d & 1) == 0 && d / 2 < F.fieldRows) { fld = ff; k = d / 2; }
            }
        }
        // colour subcarrier phase at the line start (exact: the subcarrier makes cyclesNum / 62500 cycles a line)
        const bool amChroma = F.pal || F.ntsc;
        double phase0 = 0, dphi = 0;
        bool vsign = true;
        if (amChroma) {
            const uint64_t num = (uint64_t)std::llround(F.fscHz / F.lineHz * 62500.0);
            const double frac = (double)((li * num) % 62500ull) / 62500.0;
            phase0 = 2 * kPi * (frac + F.fscHz * (u0 * 1e-6));
            dphi = 2 * kPi * F.fscHz / kFi;
            if (F.vSwitch) vsign = (((frame * (uint64_t)F.lines + (uint64_t)line) & 1ull) != 0);    // "A" lines: burst at +135 degrees, V as is
        }
        if (fld >= 0) {
            ensureField(fld, frame);
            const int W = F.picW;
            const size_t o = (size_t)k * W;
            auto pad = [&](std::vector<float>& dst, const float* src) {
                dst.resize((size_t)W + 4);
                for (int i = -2; i < W + 2; i++) dst[(size_t)(i + 2)] = src[std::min(W - 1, std::max(0, i))];
            };
            pad(padY, &fy[fld][o]); pad(padU, &fu[fld][o]); pad(padV, &fv[fld][o]);
            const float aStart = (float)F.blankEndUs, aEnd = Hus - (float)F.frontPorchUs, trB = (float)F.blankRiseUs, teB = trB / 0.59f;
            const int ja = std::max(0, (int)std::floor((aStart - teB - u0) / dtUs)), jb = std::min(count, (int)std::ceil((aEnd + teB - u0) / dtUs) + 1);
            const float pxPerUs = (float)(W / F.activeUs), advPx = (float)(F.chromaDelayNs * 1e-3 * pxPerUs);
            const float setup = (float)F.setup, sc = 1.f - setup;
            const int row = 2 * k + fld;
            const bool mb = card->inMultiburst(row);
            if (!mb) {
                PicTab& T = picTab[(size_t)rem];
                if (!T.built) buildPicTab(T, u0, dtUs, aStart, aEnd, trB, teB, pxPerUs, advPx, W, count);
                const int n = jb - ja;
                if (n > 0) {
                    pY.resize((size_t)n);
                    const float* w4 = &T.w4[(size_t)(ja - T.j0) * 4];
                    const int* iy = &T.iy[(size_t)(ja - T.j0)];
                    for (int q = 0; q < n; q++) {
                        const float* y4 = &padY[(size_t)iy[q]];
                        pY[(size_t)q] = (y4[0] * w4[4 * q] + y4[1] * w4[4 * q + 1]) + (y4[2] * w4[4 * q + 2] + y4[3] * w4[4 * q + 3]);
                    }
                    if (amChroma) {
                        pU.resize((size_t)n); pV.resize((size_t)n);
                        const int* ic = &T.ic[(size_t)(ja - T.j0)];
                        const float* tc = &T.tc[(size_t)(ja - T.j0)];
                        const float* pu = padU.data(); const float* pv = padV.data();
                        for (int q = 0; q < n; q++) {
                            const int i = ic[q];
                            pU[(size_t)q] = pu[i] + tc[q] * (pu[i + 1] - pu[i]);
                            pV[(size_t)q] = pv[i] + tc[q] * (pv[i + 1] - pv[i]);
                        }
                    }
                    const float* g = &T.g[(size_t)(ja - T.j0)];
                    float* lv = &lineV[(size_t)ja];
                    if (amChroma) {
                        const float s0 = (float)std::sin(phase0), c0 = (float)std::cos(phase0);
                        const float* cT = &T.cT[(size_t)(ja - T.j0)];
                        const float* sT = &T.sT[(size_t)(ja - T.j0)];
                        const float vs = vsign ? 1.f : -1.f;
                        for (int q = 0; q < n; q++) {
                            const float s = s0 * cT[q] + c0 * sT[q], c = c0 * cT[q] - s0 * sT[q];     // sin and cos of phase0 + j dphi
                            const float C = pU[(size_t)q] * s + vs * pV[(size_t)q] * c;
                            lv[q] += g[q] * (setup + sc * (pY[(size_t)q] + C));
                        }
                    } else {
                        for (int q = 0; q < n; q++) lv[q] += g[q] * (setup + sc * pY[(size_t)q]);
                    }
                }
            } else {
            // the sine of the colour subcarrier by rotation
            float s = (float)std::sin(phase0 + ja * dphi), c = (float)std::cos(phase0 + ja * dphi);
            const float sd = (float)std::sin(dphi), cd = (float)std::cos(dphi);
            const float vs = vsign ? 1.f : -1.f;
            for (int j = ja; j < jb; j++) {
                const float u = u0 + j * dtUs;
                const float p = (u - aStart) * pxPerUs - 0.5f;
                const float pf = std::floor(p), t = p - pf;
                const int i = (int)pf + 2;                                   // index into the padded row
                const float* y4 = &padY[(size_t)std::min(std::max(i - 1, 0), W)];
                const float t2 = t * t, t3 = t2 * t;
                float Y = y4[0] * (-0.5f * t3 + t2 - 0.5f * t) + y4[1] * (1.5f * t3 - 2.5f * t2 + 1.f) + y4[2] * (-1.5f * t3 + 2.f * t2 + 0.5f * t) + y4[3] * (0.5f * t3 - 0.5f * t2);
                float C = 0;
                if (amChroma) {
                    const float pc = p + advPx, pcf = std::floor(pc), tc = pc - pcf;
                    const int ic = std::min(std::max((int)pcf + 2, 0), W + 2);
                    const float U = padU[(size_t)ic] + tc * (padU[(size_t)ic + 1] - padU[(size_t)ic]);
                    const float V = padV[(size_t)ic] + tc * (padV[(size_t)ic + 1] - padV[(size_t)ic]);
                    C = U * s + vs * V * c;
                }
                if (mb) {
                    for (const auto& pk : card->multiburst()) {
                        if (u > pk.startUs - 0.3f && u < pk.endUs + 0.3f) {
                            const float env = edge(u - (float)pk.startUs, 0.3f) - edge(u - (float)pk.endUs, 0.3f);
                            const double cyc = pk.mhz * (u - pk.startUs);                     // cycles
                            Y = 0.5f + env * 0.35f * sinPi((float)(2 * kPi * (cyc - std::floor(cyc + 0.5))));
                            C = 0;
                        }
                    }
                }
                const float g = (u > aStart + 0.5f * teB && u < aEnd - 0.5f * teB) ? 1.f : edge(u - aStart, trB) - edge(u - aEnd, trB);    // 1 away from the edges
                lineV[(size_t)j] += g * (setup + sc * (Y + C));
                const float s2 = s * cd + c * sd;
                c = c * cd - s * sd; s = s2;
            }
        }
            }
        // the colour burst
        if (amChroma && type[0] == 1 && F.burstOnLine(line)) {
            const float bs = (float)F.burstStartUs, be = bs + (float)(F.burstCycles / F.fscHz * 1e6), trBu = 0.3f, teBu = trBu / 0.59f;
            const double th = F.pal ? (vsign ? 135.0 : -135.0) * kPi / 180 : kPi;     // PAL: +-135 degrees from the U axis, NTSC: 180 degrees (BT.470 Table 2 item 2.16)
            const int ja = std::max(0, (int)std::floor((bs - teBu - u0) / dtUs)), jb = std::min(count, (int)std::ceil((be + teBu - u0) / dtUs) + 1);
            for (int j = ja; j < jb; j++) {
                const float u = u0 + j * dtUs;
                const float env = edge(u - bs, trBu) - edge(u - be, trBu);
                lineV[(size_t)j] += (float)F.burstAmp * env * (float)std::sin(phase0 + j * dphi + th);
            }
        }
        if (F.secam && type[0] == 1) secamChroma(line, li, fld, k, nStart, count, u0, dtUs);
        if (tap) tap(lineV.data(), (size_t)count);
        // composite -> carrier amplitude
        const float m = (float)((100.0 - F.whitePct) / 100.0), sL = (float)F.syncLevel, comp = (float)cfg.syncCompression;
        const float hum = (float)(cfg.humPct / 100.0), wh = (float)(2 * kPi * cfg.humHz);
        const float humPh0 = (float)std::fmod(2 * kPi * cfg.humHz * ((double)nStart / kFi), 2 * kPi);
        const float lvl = (float)cfg.level;
        const size_t base = a.size();
        a.resize(base + (size_t)count);
        const float vLo = sL * 1.2f, kAmp = m / (1.f - sL), omc = 1.f - comp;
        float* aOut = &a[base];
        const float* lv = lineV.data();
        for (int j = 0; j < count; j++) {
            float v = lv[j];
            if (v < 0) v *= omc;
            v = std::min(std::max(v, vLo), 1.3f);
            aOut[j] = lvl * (1.f - kAmp * (v - sL));
        }
        if (hum != 0) {
            // the mains hum: a sine of the line count, by rotation (one sine and cosine a line)
            const double wd = (double)wh / kFi;
            float hs = (float)std::sin(humPh0), hc = (float)std::cos(humPh0);
            const float sd = (float)std::sin(wd), cd = (float)std::cos(wd);
            for (int j = 0; j < count; j++) {
                aOut[j] *= 1.f + hum * hs;
                const float s2 = hs * cd + hc * sd;
                hc = hc * cd - hs * sd; hs = s2;
            }
        }
    }

    // ---- filter what is complete, put it on the carrier
    void filterAvailable() {
        const int m = kVsbTaps / 2;
        const int64_t aEnd = aBase + (int64_t)a.size();
        const int64_t last = aEnd - m;                     // exclusive: samples up to last - 1 have all their neighbours
        if (last <= firNext) return;
        const size_t n = (size_t)(last - firNext);
        tR.resize(n); tI.resize(n);
        const float* x = &a[(size_t)(firNext - m - aBase)];
        desamp(x, 1, hR.data(), tR.data(), (int)n, kVsbTaps);        // the correlation of the amplitude with the real and the imaginary taps (Accelerate on the Mac)
        desamp(x, 1, hI.data(), tI.data(), (int)n, kVsbTaps);
        const size_t o = xr.size();
        xr.resize(o + n); xi.resize(o + n);
        const double fc = atvVisionOffsetHz(fmt) + cfg.cfoHz;
        const double w = 2 * kPi * fc / kFi;
        {
            // four phasors a sample apart, each stepped by four samples (explicit arithmetic: the complex operators of the library are slower)
            const std::complex<double> p0 = std::polar(1.0, carPhase), r1 = std::polar(1.0, w);
            float pr[4], pi[4];
            for (int q = 0; q < 4; q++) { const std::complex<double> v = p0 * std::pow(r1, q); pr[q] = (float)v.real(); pi[q] = (float)v.imag(); }
            const std::complex<double> r4d = std::pow(r1, 4);
            const float rr = (float)r4d.real(), ri = (float)r4d.imag();
            float* outR = &xr[o];
            float* outI = &xi[o];
            size_t j = 0;
            for (; j + 4 <= n; j += 4) {
                for (int q = 0; q < 4; q++) {
                    outR[j + q] = tR[j + q] * pr[q] - tI[j + q] * pi[q];
                    outI[j + q] = tR[j + q] * pi[q] + tI[j + q] * pr[q];
                    const float nr = pr[q] * rr - pi[q] * ri;
                    pi[q] = pr[q] * ri + pi[q] * rr;
                    pr[q] = nr;
                }
                if ((j & 1023) == 1020) for (int q = 0; q < 4; q++) { const float m = 1.f / std::sqrt(pr[q] * pr[q] + pi[q] * pi[q]); pr[q] *= m; pi[q] *= m; }
            }
            for (; j < n; j++) {
                const std::complex<double> v = p0 * std::polar(1.0, w * (double)j);
                const cf32 y = cf32(tR[j], tI[j]) * std::complex<float>(v);
                outR[j] = y.real(); outI[j] = y.imag();
            }
        }
        carPhase = std::fmod(carPhase + w * (double)n, 2 * kPi);
        firNext = last;
        // drop what nobody needs any more
        const size_t keepA = (size_t)(kVsbTaps + 8);
        const int64_t dropA = firNext - m - aBase - (int64_t)keepA;
        if (dropA > 8192) { a.erase(a.begin(), a.begin() + dropA); aBase += dropA; }
    }

    void produce() {
        renderLine();
        filterAvailable();
    }

    // The audio is evaluated every 32 output samples (on the absolute count, so the chunk size of the caller does not matter) and
    // interpolated linearly in between: a tone of 15 kHz at most changes very little in 3 us.
    void sound(cf32* out, size_t n, uint64_t k0) {
        if (cfg.sound == 3) return;
        const double dt = step / kFi;                                  // seconds of signal per output sample
        const double fSnd = atvSoundOffsetHz(fmt) + cfg.cfoHz;
        const double dev = fmt.soundDevKhz * 1e3;
        const float amp = (float)(cfg.level * std::pow(10.0, fmt.soundRelDb / 20.0));
        double w0 = 2 * kPi * fSnd * dt;
        w0 -= 2 * kPi * std::nearbyint(w0 / (2 * kPi));                  // the carrier step modulo a turn, so that one correction a sample is enough
        const double dk = 2 * kPi * dev * dt;
        constexpr uint64_t B = 32;
        sPh.resize(n); sPhD.resize(n);
        double ph = sndPh;
        size_t i = 0;
        while (i < n) {
            const uint64_t k = k0 + i, kb = k / B;
            if (kb != audioBlock || audioDt != dt) {
                if (kb == audioBlock + 1 && audioDt == dt) audioA0 = audioA1; else audioA0 = prog.at((double)(kb * B) * dt);
                audioA1 = prog.at((double)((kb + 1) * B) * dt);
                audioBlock = kb; audioDt = dt;
            }
            // up to the end of this block of 32 samples the audio is a straight line: the phase advances by a constant plus a ramp
            const size_t m0 = (size_t)(k % B), cnt = std::min<size_t>(n - i, (size_t)B - m0);
            const double slope = (audioA1 - audioA0) / (double)B;
            double* pd = &sPhD[i];
            // the carrier and the deviation, as phase; the sum is never reduced (the result must not depend on how the calls split the stream)
            for (size_t q = 0; q < cnt; q++) { ph += w0 + dk * (audioA0 + slope * (double)(m0 + q)); pd[q] = ph; }
            i += cnt;
        }
        // the phase into [-pi, pi]
        for (size_t q = 0; q < n; q++) sPh[q] = (float)(sPhD[q] - 2 * kPi * std::nearbyint(sPhD[q] * (0.5 / kPi)));
        sndPh = ph;
        for (size_t i = 0; i < n; i++) {
            const float x = sPh[i];
            float y = x + 1.5707963f;
            if (y > 3.1415927f) y -= 6.2831855f;
            out[i] += cf32(sinPi(y) * amp, sinPi(x) * amp);             // cos and sin
        }
    }

    void generate(cf32* out, size_t n) {
        size_t done = 0;
        while (done < n) {
            const size_t b = std::min<size_t>(n - done, 2048);
            const int64_t need = (int64_t)std::floor(pos + (double)b * step) + kKernTaps;
            while (xvBase + (int64_t)xr.size() < need) produce();
            for (size_t j = 0; j < b; j++) {
                const int64_t i0 = (int64_t)std::floor(pos);
                int ph = (int)std::lround((pos - (double)i0) * kKernPhases);
                int64_t ib = i0;
                if (ph >= kKernPhases) { ph -= kKernPhases; ib++; }
                const float* kk = &kern[(size_t)ph * kKernTaps];
                const size_t at = (size_t)(ib - (kKernTaps / 2 - 1) - xvBase);
                const float* xs = &xr[at];
                const float* ys = &xi[at];
                float re[4] = {0, 0, 0, 0}, im[4] = {0, 0, 0, 0};            // four lanes: the compiler turns this into vector code
                for (int b = 0; b < kKernTaps; b += 4)
                    for (int l = 0; l < 4; l++) { re[l] += kk[b + l] * xs[b + l]; im[l] += kk[b + l] * ys[b + l]; }
                out[done + j] = cf32((re[0] + re[1]) + (re[2] + re[3]), (im[0] + im[1]) + (im[2] + im[3]));
                pos += step;
            }
            sound(out + done, b, outCount);
            if (cfg.echoDb > 0 && !ghost.empty()) {
                const cf32 g = std::polar((float)std::pow(10.0, -cfg.echoDb / 20.0), (float)(cfg.echoPhaseDeg * kPi / 180));
                for (size_t j = 0; j < b; j++) {
                    const cf32 cur = out[done + j];
                    out[done + j] = cur + g * ghost[ghostPos];
                    ghost[ghostPos] = cur;
                    if (++ghostPos == ghost.size()) ghostPos = 0;
                }
            }
            if (cfg.cnrDb < 100) {
                const float sigma = (float)(cfg.level * std::pow(10.0, -cfg.cnrDb / 20.0) * std::sqrt(cfg.rate / 5e6));
                noise.add(out + done, b, sigma);
            }
            outCount += b;
            done += b;
            const int64_t keep = (int64_t)std::floor(pos) - kKernTaps - 2;
            if (keep - xvBase > 16384) { xr.erase(xr.begin(), xr.begin() + (keep - xvBase)); xi.erase(xi.begin(), xi.begin() + (keep - xvBase)); xvBase = keep; }
        }
    }
};

AtvGenerator::AtvGenerator(const AtvGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
AtvGenerator::~AtvGenerator() = default;
bool AtvGenerator::ok() const { return p_->ok; }
const AtvFormat& AtvGenerator::format() const { return p_->fmt; }
const AtvGenConfig& AtvGenerator::config() const { return p_->cfg; }
void AtvGenerator::generate(cf32* out, size_t n) { if (p_->ok) p_->generate(out, n); else std::fill(out, out + n, cf32(0, 0)); }
void AtvGenerator::generate(size_t n, std::vector<cf32>& out) { const size_t o = out.size(); out.resize(o + n); generate(out.data() + o, n); }
void AtvGenerator::setCompositeTap(std::function<void(const float*, size_t)> tap) { p_->tap = std::move(tap); }
const AtvCard& AtvGenerator::card() const { return *p_->card; }
double AtvGenerator::timeSec() const { return p_->cfg.startSec + p_->pos / kFi; }

bool AtvGenerator::update(const AtvGenConfig& c) {
    Impl& s = *p_;
    if (c.sys != s.cfg.sys || c.colour != s.cfg.colour || c.rate != s.cfg.rate || c.pattern != s.cfg.pattern || c.groupDelay != s.cfg.groupDelay ||
        c.nyquistTx != s.cfg.nyquistTx || c.setup != s.cfg.setup || c.secamIdent != s.cfg.secamIdent) return false;
    s.applyRuntime(c, false);
    return true;
}

// ---- the synthetic source of the app

namespace {
class AtvSynth : public ModeSynth {
public:
    AtvSynth(const AtvGenConfig& c) : gen_(c) {}
    bool ok() const { return gen_.ok(); }
    double sampleRate() const override { return gen_.config().rate; }
    void generate(cf32* out, size_t n) override { gen_.generate(out, n); }
    bool configure(const SynthConfig& s) override {
        AtvGenConfig c = gen_.config();
        if (!fromSynth(s, gen_.config().rate, c)) return false;
        return gen_.update(c);
    }
    static bool fromSynth(const SynthConfig& s, double rate, AtvGenConfig& c) {
        static const int kSys[6] = {kAtvG, kAtvB, kAtvI, kAtvDK, kAtvM, kAtvN};
        const int so = s.modeOpt[0];
        if (so < 0 || so > 5) return false;
        c.sys = kSys[so];
        const int co = s.modeOpt[1];
        c.colour = co == 0 ? (c.sys == kAtvM ? kAtvNtsc : kAtvPal) : co == 1 ? kAtvPal : co == 2 ? kAtvNtsc : co == 3 ? kAtvSecam : kAtvMono;
        c.rate = rate;
        c.cnrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.sroPpm = s.sroPpm;
        c.echoDb = s.echoDb;
        c.echoDelayUs = s.modeVal[2] > 0 ? s.modeVal[2] : 1.5;
        c.echoPhaseDeg = s.modeVal[3];
        c.pattern = std::min(2, std::max(0, s.modeOpt[2]));
        c.sound = std::min(4, std::max(0, s.modeOpt[3]));
        c.humPct = s.modeOpt[4] ? (s.modeVal[0] > 0 ? s.modeVal[0] : 4.0) : 0;
        c.humHz = s.modeOpt[4] == 2 ? 60 : 50;
        c.groupDelay = s.modeOpt[5] == 0;
        c.nyquistTx = s.modeOpt[6] == 1;
        c.setup = s.modeOpt[7] == 0;
        c.secamIdent = c.colour == kAtvSecam ? std::min(2, std::max(0, s.modeOpt[7])) : 0;
        c.syncCompression = std::min(0.9, std::max(0.0, s.modeVal[1] / 100.0));
        return true;
    }
private:
    AtvGenerator gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeAtvSynth(const SynthConfig& cfg, double sampleRate) {
    AtvGenConfig c;
    if (!AtvSynth::fromSynth(cfg, sampleRate, c)) return nullptr;
    auto s = std::make_unique<AtvSynth>(c);
    if (!s->ok()) return nullptr;
    return s;
}

} // namespace dect2

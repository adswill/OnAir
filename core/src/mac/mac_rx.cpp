// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MAC television receiver (see mac_rx.h).
#include "dect2/mac/mac_rx.h"
#include "dect2/mac/mac_tel.h"
#include "dect2/mac/mac_proto.h"
#include "dect2/audio/audioout.h"
#include "../atv/atv_dsp.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

using namespace mac;

namespace {
using dect2::math::kPi;
constexpr int kTaps = 16, kPhases = 256;
constexpr int kAcqLines = 48;
constexpr double kTimingSlope = 1.2;   // midpoint deviation per bit of timing error (duobinary, measured on the test signal)

double sinc(double x) { return std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x); }
double kaiserW(double x, double beta) { if (std::fabs(x) >= 1) return 0; return atvdsp::besselI0(beta * std::sqrt(1 - x * x)) / atvdsp::besselI0(beta); }
} // namespace

struct MacReceiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true};
    std::function<void(const std::string&)> log;
    std::function<void(const float*, size_t)> tapSet, tap;
    std::atomic<bool> tapReq{false};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    MacTelemetry pub, tel;
    std::shared_ptr<const AtvFrame> pubFrame;
    uint64_t frameSeq = 0;

    // ------------------------------------------------------------ front end
    double fs = 0, offset = 0;
    atvdsp::Nco nco;
    cf32 prev{1, 0};
    Iir1 deem;
    bool invert = false;
    float hzScale = 0;
    std::vector<float> buf;        // v (volts, clamp level about 0.5) at the input rate
    int64_t base = 0;              // absolute index of buf[0]
    int64_t nIn = 0;
    double power = 0; int64_t nPower = 0;
    double nextReport = 0;
    std::vector<float> ptab;       // polyphase interpolator kPhases x kTaps

    int64_t total() const { return base + (int64_t)buf.size(); }
    float at(double x) const {
        const double fl = std::floor(x);
        int64_t i0 = (int64_t)fl;
        int ph = (int)((x - fl) * kPhases + 0.5);
        if (ph >= kPhases) { ph = 0; i0++; }
        const int64_t s = i0 - kTaps / 2 + 1 - base;
        if (s < 0 || s + kTaps > (int64_t)buf.size()) return 0.5f;
        const float* b = &buf[(size_t)s];
        const float* h = &ptab[(size_t)ph * kTaps];
        float acc = 0;
        for (int t = 0; t < kTaps; t++) acc += h[t] * b[t];
        return acc;
    }

    void initFront() {
        nco.reset(); nco.setFreq(-offset, fs > 0 ? fs : 1);
        prev = cf32(1, 0);
        deem = deEmphasis(fs > 0 ? fs : 1); deem.reset(0);
        hzScale = (float)(fs / (2 * kPi) / kDevHzPerV);
        buf.clear(); base = 0;
        ptab.assign((size_t)kPhases * kTaps, 0.f);
        const double fc = std::min(9.5e6, 0.48 * fs) / fs;
        for (int p = 0; p < kPhases; p++) {
            const double fr = (double)p / kPhases;
            double s = 0;
            for (int t = 0; t < kTaps; t++) {
                const double tau = (t - (kTaps / 2 - 1)) - fr;
                const double v = 2 * fc * sinc(2 * fc * tau) * kaiserW(tau / (kTaps / 2 + 0.5), 5.0);
                ptab[(size_t)p * kTaps + t] = (float)v; s += v;
            }
            for (int t = 0; t < kTaps; t++) ptab[(size_t)p * kTaps + t] = (float)(ptab[(size_t)p * kTaps + t] / s);
        }
    }

    // ------------------------------------------------------------ line tracking
    enum Mode { Search, Track } mode = Search;
    int variant = kD2;
    double o = 0;                  // input position of sample 0 (= sample 1296 of the previous line) of the line being decoded
    double step = 1;               // input samples per 20.25 MHz clock period
    double stepNom = 1;
    int64_t scanFrom = 0;
    int lineNo = -1;               // 1..625, -1 not known
    int fswMismatch = 0, fswMissed = 0;
    float gain = 1;                // data / video gain (1 = 13.5 MHz/V as received)
    float clampOff = 0;
    double cfoAcc = 0; int cfoN = 0;
    double eyeErr = 0; int eyeN = 0;
    float dataAmpAcc = 0; int dataAmpN = 0;
    int lswOkWin = 0, lswWin = 0, lswBadRun = 0;
    uint64_t frameLines = 0;
    int64_t lastFswLine = -100000; int64_t lineCounter = 0;
    double fswErrAcc = 0, fswBitsAcc = 0;
    // line 624 calibration
    float black = 0, white = 1; bool calib = false;
    double refVar = 0; int refN = 0;
    // picture store
    std::vector<float> luma, chroma;   // [line][697], [line][349]
    std::vector<uint8_t> haveLine;
    std::vector<int> xj0, xc0; std::vector<float> xjf, xcf;   // the picture's column positions
    // packets
    std::vector<uint8_t> pk[2];
    int pkFill[2] = {0, 0}, pkCount[2] = {0, 0};
    SiAssembler si;
    int soundAddr = -1, ecmAddr = -1;
    bool soundFromSi = false;
    std::map<int, int> soundCand;
    int lastSoundCi = -1;
    // sound
    Iir1 j17;
    std::vector<float> aud, pcmOut;
    float upPrev = 0; int upPhase = 0;
    double sndPow = 0; int sndN = 0;
    std::unique_ptr<AudioOut> audioOut;
    // service information
    Sdf sdf; bool sdfSeen = false;
    bool accm = false, vconfWide = false, vconfKnown = false;
    int caByte = -1;

    double sigSec() const { return fs > 0 ? (double)nIn / fs : 0; }
    void say(const std::string& s) { if (log) log(s); }

    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); fs = rateSet; offset = offsetSet; }
        initFront();
        mode = Search; variant = kD2; lineNo = -1; fswMismatch = fswMissed = 0;
        stepNom = step = fs / kClockHz; o = 0; scanFrom = 0;
        gain = 1; clampOff = 0; cfoAcc = 0; cfoN = 0; eyeErr = 0; eyeN = 0; dataAmpAcc = 0; dataAmpN = 0;
        lswOkWin = lswWin = lswBadRun = 0; frameLines = 0; lastFswLine = -100000; lineCounter = 0; fswErrAcc = fswBitsAcc = 0;
        black = 0; white = 1; calib = false; refVar = 0; refN = 0; invert = false;
        luma.assign((size_t)(kLines + 1) * kLumaN, 0.f); chroma.assign((size_t)(kLines + 1) * kChromaN, 0.5f); haveLine.assign(kLines + 1, 0);
        for (int s = 0; s < 2; s++) { pk[s].assign(kPacketBits, 0); pkFill[s] = 0; pkCount[s] = 0; }
        si = SiAssembler(); soundAddr = -1; ecmAddr = -1; soundFromSi = false; soundCand.clear(); lastSoundCi = -1;
        j17 = j17De(32000); aud.clear(); upPrev = 0; upPhase = 0; sndPow = 0; sndN = 0;
        sdf = Sdf(); sdfSeen = false; accm = false; vconfWide = false; vconfKnown = false; caByte = -1;
        power = 0; nPower = 0; nIn = 0; nextReport = 0.25 * fs;
        const uint64_t seq = tel.seq;
        tel = MacTelemetry(); tel.seq = seq;
        tel.inputRate = fs;
    }

    // ------------------------------------------------------------ acquisition
    void acquire() {
        const double lineLen = kLineSamples * step;
        const int64_t need = (int64_t)((kAcqLines + 2) * lineLen) + 64;
        if (scanFrom < base + 64) scanFrom = base + 64;
        if (total() - scanFrom < need) return;
        const double a = (double)scanFrom + 16;
        auto raw = [&](double x) { const int64_t i = (int64_t)std::llround(x) - base; return i >= 0 && i < (int64_t)buf.size() ? buf[(size_t)i] : 0.5f; };
        // threshold: the mean distance from the clamp level
        double th = 0; int thn = 0;
        for (int l = 0; l < kAcqLines; l += 3) for (int s = 0; s < kLineSamples; s += 7) { th += std::fabs(raw(a + (l * kLineSamples + s) * step) - 0.5f); thn++; }
        th /= std::max(1, thn);
        // the line sync word correlation at every position of the line, the two halves of the window apart (they give the clock error)
        constexpr int kPad = 8;
        std::vector<float> q((size_t)kLineSamples + 2 * kPad);
        std::vector<double> acc[2][2];
        for (auto& h : acc) for (auto& v : h) v.assign(kLineSamples, 0.0);
        int offs[2][6]; float tm[6];
        for (int V = 0; V < 2; V++) for (int b = 0; b < 6; b++) offs[V][b] = bitSample(V, b + 1);
        for (int b = 0; b < 6; b++) tm[b] = lswBit(true, b) ? 1.f : -1.f;
        for (int l = 0; l < kAcqLines; l++) {
            const double lb = a + (double)l * lineLen;
            for (int s = -kPad; s < kLineSamples + kPad; s++) q[(size_t)(s + kPad)] = std::fabs(raw(lb + s * step) - 0.5f) - (float)th;
            const float sg = (l & 1) ? -1.f : 1.f;
            const int half = l < kAcqLines / 2 ? 0 : 1;
            for (int V = 0; V < 2; V++) {
                double* ac = acc[V][half].data();
                const int* of = offs[V];
                for (int k = 0; k < kLineSamples; k++) {
                    const float* qq = &q[(size_t)(k + kPad)];
                    float c = 0;
                    for (int b = 0; b < 6; b++) c += tm[b] * qq[of[b]];
                    ac[k] += sg * c;
                }
            }
        }
        double bestRatio = 0, bestFrac = 0; int bestK = 0, bestV = kD2;
        for (int V = 0; V < 2; V++) {
            double mx = 0, mean = 0; int km = 0;
            for (int k = 0; k < kLineSamples; k++) {
                // a clock error moves the peak between the halves: take the best neighbour of the other half
                double vb = 0;
                for (int d = -3; d <= 3; d++) vb = std::max(vb, std::fabs(acc[V][1][(size_t)((k + d + kLineSamples) % kLineSamples)]));
                const double v = std::fabs(acc[V][0][(size_t)k]) + vb;
                mean += v; if (v > mx) { mx = v; km = k; }
            }
            mean /= kLineSamples;
            const double ratio = mx / (mean + 1e-9);
            if (ratio > bestRatio) { bestRatio = ratio; bestK = km; bestV = V; }
        }
        if (bestRatio < 5) { scanFrom += (int64_t)(kAcqLines * lineLen); trim(scanFrom - 64); return; }
        {   // the clock error: where the peak is in each half (with a parabola through the top)
            auto peak = [&](const std::vector<double>& v, int around) {
                int km = around; double mx = -1;
                for (int d = -4; d <= 4; d++) { const int k = (around + d + kLineSamples) % kLineSamples; if (std::fabs(v[(size_t)k]) > mx) { mx = std::fabs(v[(size_t)k]); km = k; } }
                const double y0 = std::fabs(v[(size_t)((km + kLineSamples - 1) % kLineSamples)]), y1 = mx, y2 = std::fabs(v[(size_t)((km + 1) % kLineSamples)]);
                const double den = y0 - 2 * y1 + y2;
                double k = km + (den < 0 ? 0.5 * (y0 - y2) / den : 0.0);
                if (k - around > kLineSamples / 2) k -= kLineSamples;
                if (around - k > kLineSamples / 2) k += kLineSamples;
                return k;
            };
            const double kA = peak(acc[bestV][0], bestK), kB = peak(acc[bestV][1], bestK);
            const double drift = (kB - kA) / (kAcqLines / 2);   // clock periods per line
            if (std::fabs(drift) < 0.3) {
                step *= 1 + drift / kLineSamples;
                bestK = (int)std::lround(0.5 * (kA + kB) + 0.5 * drift);
                bestK = (bestK % kLineSamples + kLineSamples) % kLineSamples;
            }
        }
        // fine timing: the fraction of a clock period where the burst decisions sit closest to the three levels
        const int V = bestV, nb = burstBits(V);
        const int lm = kAcqLines / 2;   // decoding starts in the middle of the window
        const double lineLen2 = kLineSamples * step;
        // the extreme level: the mean distance from the clamp level of the outer cluster of the burst samples
        double m1 = 0, amp = 0; int an = 0, n1 = 0;
        for (int pass = 0; pass < 2; pass++, m1 /= std::max(1, n1), n1 = 1)
            for (int l = lm; l < lm + 8; l++)
                for (int i = 1; i <= nb; i++) {
                    const double v = std::fabs(at(a + l * lineLen2 - lm * (lineLen2 - lineLen) + (bestK + bitSample(V, i)) * step) - 0.5f);
                    if (!pass) { m1 += v; n1++; } else if (v > m1) { amp += v; an++; }
                }
        amp = std::max(0.05, amp / std::max(1, an));
        double bestD = 1e30;
        for (int fi = -4; fi <= 4; fi++) {
            const double fr = fi * 0.125;
            double dsum = 0;
            for (int l = lm; l < lm + 8; l++)
                for (int i = 7; i <= nb; i++) {
                    const double y = std::fabs(at(a + l * lineLen2 - lm * (lineLen2 - lineLen) + (bestK + fr + bitSample(V, i)) * step) - 0.5f) / amp;
                    dsum += std::min(y, std::fabs(y - 1));
                }
            if (dsum < bestD) { bestD = dsum; bestFrac = fr; }
        }
        variant = V;
        o = a + lm * lineLen + (bestK + bestFrac) * step;   // line lm of the window (the clock error counted from its middle)
        gain = (float)(amp / kDataAmp);
        mode = Track; lineNo = -1; fswMismatch = fswMissed = 0; lswBadRun = 0;
        for (int s = 0; s < 2; s++) { pkFill[s] = 0; pkCount[s] = 0; }
        char b[160];
        snprintf(b, sizeof b, "MAC: %s line sync found (score %.0f), data level %.2f V", variantName(V), bestRatio, amp);
        say(b);
    }

    void trim(int64_t keepFrom) {
        const int64_t drop = keepFrom - base - 2 * kTaps;
        if (drop > (int64_t)(1 << 20)) { buf.erase(buf.begin(), buf.begin() + drop); base += drop; }
    }

    // ------------------------------------------------------------ one line
    float val(double s) const { return (at(o + s * step) - 0.5f - clampOff) / gain + 0.5f; }

    void decodeLine() {
        const int V = variant;
        // clamp (not on line 625)
        // (line 625 has data there: a clamp period that is not flat is not used)
        float cl = 0, cq = 0;
        for (int s = 213; s <= 223; s++) { const float v = at(o + s * step); cl += v; cq += v * v; }
        cl /= 11; cq = cq / 11 - cl * cl;
        cl -= 0.5f;
        const bool is625 = lineNo == 625;
        // the burst
        const int nbN = burstBits(V);
        int nb = nbN;
        std::vector<int8_t> a(1300 + 4, 0);
        std::vector<uint8_t> bits(1300 + 1, 0);
        std::vector<float> y(1300 + 2, 0.f);
        auto decide = [&](int i0, int i1) {
            for (int i = i0; i <= i1; i++) {
                const float v = (val(bitSample(V, i)) - 0.5f) / kDataAmp;
                y[(size_t)i] = v;
                a[(size_t)i] = (int8_t)(std::fabs(v) > 0.5f ? (v > 0 ? 1 : -1) : 0);
                bits[(size_t)i] = a[(size_t)i] != 0;
            }
        };
        decide(1, nbN);
        // line sync word
        int e1 = 0, e2 = 0;
        for (int b = 0; b < 6; b++) { e1 += bits[(size_t)b + 1] != lswBit(true, b); e2 += bits[(size_t)b + 1] != lswBit(false, b); }
        const bool lswOk = std::min(e1, e2) <= 1;
        lswWin++; lswOkWin += lswOk;
        lswBadRun = lswOk ? 0 : lswBadRun + 1;
        // frame sync sequence (bits 7..102)
        int fe = 0;
        for (int i = 0; i < 96; i++) fe += bits[(size_t)i + 7] != fsdBit(i);
        const bool fswTrue = fe <= 12, fswInv = fe >= 84;
        const bool fsw = fswTrue || fswInv;
        if (fsw) {
            tel.fswFound++;
            fswErrAcc += fswTrue ? fe : 96 - fe; fswBitsAcc += 96;
            if (lineNo != 625) {
                if (lineNo < 0 || ++fswMismatch >= 2) {
                    if (lineNo >= 0) say("MAC: frame sync moved, line count restarted");
                    lineNo = 625; fswMismatch = 0;
                    haveLine.assign(kLines + 1, 0);
                }
            } else fswMismatch = 0;
            lastFswLine = lineCounter;
        }
        // the clamp level of this line (the burst above was sliced with the last one): not on line 625
        if (!is625 && !fsw && cq < 0.004f * gain * gain) { clampOff = cl; cfoAcc += cl; cfoN++; }
        // timing error on the burst transitions (not on line 625, where the special data runs into the line)
        if (lineNo != 625) {
            double e = 0; int ne = 0;
            const float sp = (float)bitSpacing(V);
            for (int i = 1; i < nb; i++) {
                if (a[(size_t)i] == a[(size_t)i + 1]) continue;
                const float am = i > 1 ? a[(size_t)i - 1] : 0, ap = i + 2 <= nb ? a[(size_t)i + 2] : 0;
                const float ref = 9.f / 16 * (a[(size_t)i] + a[(size_t)i + 1]) - 1.f / 16 * (am + ap);
                const float m = (val(bitSample(V, i) + 0.5 * sp) - 0.5f) / kDataAmp;
                e += (m - ref) * (a[(size_t)i + 1] > a[(size_t)i] ? 1 : -1);
                ne++;
            }
            if (ne > 8 && lswOk) {
                const double tau = e / ne / kTimingSlope * sp;   // clock periods late
                o += -0.25 * tau * step;
                step *= 1 - 0.004 * tau / kLineSamples;
                step = std::max(stepNom * 0.999, std::min(stepNom * 1.001, step));
            }
            // eye and data level
            float ampAcc = 0; int an = 0;
            for (int i = 7; i <= nb; i++) {
                const float v = y[(size_t)i];
                if (a[(size_t)i]) { eyeErr += (std::fabs(v) - 1) * (std::fabs(v) - 1); ampAcc += std::fabs(v); an++; }
                else eyeErr += v * v;
                eyeN++;
            }
            if (an > 10 && lswOk) {
                const float g = ampAcc / an;   // measured extreme level / 0.4 V
                gain *= 1 + 0.02f * (g - 1);
                dataAmpAcc += g * gain * kDataAmp; dataAmpN++;
            }
            if (lineNo == 100 || (lineNo < 0 && lineCounter % 625 == 100)) {
                tel.lineWave.resize(512);
                for (int k = 0; k < 512; k++) tel.lineWave[(size_t)k] = val(1 + k * (kLineSamples - 1) / 511.0);
                tel.eye.assign(y.begin() + 1, y.begin() + 1 + nb);
            }
        }
        // what the line carries
        if (lineNo >= 1 && lineNo <= kDataLines) {
            const std::vector<uint8_t>& pr = prbsFrame(V);
            const int per = line625Bits(V);
            if (lineNo == 1) for (int s = 0; s < 2; s++) { pkFill[s] = 0; pkCount[s] = 0; }
            for (int s = 0; s < subframes(V); s++)
                for (int i = 0; i < kSubframeBits; i++) {
                    const int bit = 7 + s * kSubframeBits + i;
                    if (pkCount[s] >= kPacketsPerFrame) break;
                    pk[s][(size_t)pkFill[s]++] = bits[(size_t)bit] ^ pr[(size_t)(lineNo - 1) * per + (bit - 7)];
                    if (pkFill[s] == kPacketBits) { packet(pk[s].data()); pkFill[s] = 0; pkCount[s]++; }
                }
        }
        if (lineNo >= kField1First && lineNo <= 622) {
            float* Y = &luma[(size_t)lineNo * kLumaN];
            float* C = &chroma[(size_t)lineNo * kChromaN];
            for (int j = 0; j < kLumaN; j++) Y[j] = val(kLumaFirst + j);
            for (int n = 0; n < kChromaN; n++) C[n] = val(kChromaFirst + n);
            haveLine[(size_t)lineNo] = 1;
        }
        if (lineNo == 624) line624();
        if (lineNo == 625) {
            decide(nbN + 1, line625Bits(V));
            uint8_t sp[546];
            for (int i = 0; i < 546; i++) sp[i] = bits[(size_t)(103 + i)];
            Sdf s; Rdf r[5]; bool ok = false;
            const int good = line625Decode(sp, s, r, ok);
            tel.blocksBad += (uint64_t)(6 - good);
            if (ok) {
                if (!sdfSeen || s.mvscg != sdf.mvscg) {
                    char b[160];
                    snprintf(b, sizeof b, "MAC: line 625: channel id %04X, %s, %s", s.chid, s.wide() ? "16:9" : "4:3", vsamText(s).c_str());
                    say(b);
                }
                sdf = s; sdfSeen = true;
            }
            for (const Rdf& x : r) if (x.tdmcid >= 0) tel.fcnt = x.fcnt;
            endFrame();
        }
        // next line
        lineCounter++;
        tel.lines++;
        if (lineNo > 0) {
            if (lineNo == 625 && !fsw) fswMissed++;
            lineNo = lineNo == 625 ? 1 : lineNo + 1;
        }
        if (lineCounter - lastFswLine > 4 * kLines && lineNo > 0) { lineNo = -1; say("MAC: frame sync lost"); }
        o += kLineSamples * step;
        if (lswBadRun > 60) {
            say("MAC: line sync lost");
            mode = Search; scanFrom = (int64_t)o; lineNo = -1;
        }
    }

    void line624() {
        auto mean = [&](int s0, int s1, double& var) {
            double s = 0, q = 0; int n = 0;
            for (int x = s0; x <= s1; x += 2) { const double v = val(x); s += v; q += v * v; n++; }
            s /= n; var = q / n - s * s;
            return (float)s;
        };
        double vg, vw, vb;
        mean(kRefGrey0, kRefGrey1, vg);
        const float w = mean(kRefWhite0, kRefWhite1, vw), b = mean(kRefBlack0, kRefBlack1, vb);
        if (w - b < -0.3f && !invert) { invert = true; say("MAC: the picture is negative: spectrum inverted, corrected"); return; }
        if (w - b < 0.3f) return;
        if (!calib) { black = b; white = w; calib = true; }
        else { black += 0.2f * (b - black); white += 0.2f * (w - white); }
        refVar += (vg + vw + vb) / 3; refN++;
    }

    // ------------------------------------------------------------ the picture
    void endFrame() {
        tel.frames++;
        int have = 0;
        for (int L = kField1First; L <= 622; L++) have += haveLine[(size_t)L];
        if (have < 400) return;
        const bool wide = sdfSeen ? sdf.wide() : vconfWide;
        const int W = wide ? 1024 : 768, H = 576;
        auto f = std::make_shared<AtvFrame>();
        f->width = W; f->height = H; f->fieldRate = 50; f->colour = true; f->field = 1;
        f->rgba.assign((size_t)W * H * 4, 255);
        const float bl = calib ? black : 0.f, sc = calib ? 1.f / std::max(0.3f, white - black) : 1.f;
        // where each output column samples the luminance and colour-difference rows (linear interpolation)
        if ((int)xj0.size() != W) {
            xj0.resize((size_t)W); xjf.resize((size_t)W); xc0.resize((size_t)W); xcf.resize((size_t)W);
            for (int x = 0; x < W; x++) {
                const double j = (x + 0.5) * kLumaN / W - 0.5;
                const double jj = std::max(0.0, std::min((double)kLumaN - 1.001, j));
                xj0[(size_t)x] = (int)jj; xjf[(size_t)x] = (float)(jj - (int)jj);
                const double c = std::max(0.0, std::min((double)kChromaN - 1.001, (j + 4) / 2));
                xc0[(size_t)x] = (int)c; xcf[(size_t)x] = (float)(c - (int)c);
            }
        }
        std::vector<float> ea((size_t)W), eb((size_t)W);
        static const std::vector<float> neutral(kChromaN, 0.5f);
        auto row = [&](int L) -> const float* { return &chroma[(size_t)L * kChromaN]; };
        for (int R = 0; R < H; R++) {
            const int fl = R & 1, r = R >> 1;
            const int first = fl ? kField2First : kField1First;
            const int L = first + r;
            uint8_t* px = &f->rgba[(size_t)R * W * 4];
            const bool okY = L <= 622 && haveLine[(size_t)L] && r > 0;
            const bool okA = r > 0 && L - 1 >= first && haveLine[(size_t)L - 1];
            const bool okB0 = L - 2 >= first && haveLine[(size_t)L - 2], okB1 = L <= 622 && haveLine[(size_t)L];
            const bool aIsU = chromaIsU(L - 1);
            const float* Ca = okA ? row(L - 1) : neutral.data();
            const float* Cb0 = okB0 ? row(L - 2) : okB1 ? row(L) : neutral.data();
            const float* Cb1 = okB1 ? row(L) : Cb0;
            const float* Yr = &luma[(size_t)L * kLumaN];
            const float su = sc / kUScale, sv = sc / kVScale;
            for (int x = 0; x < W; x++) {
                const int c0 = xc0[(size_t)x]; const float cf = xcf[(size_t)x];
                const float a = Ca[c0] + (Ca[c0 + 1] - Ca[c0]) * cf - 0.5f;
                const float b0 = Cb0[c0] + (Cb0[c0 + 1] - Cb0[c0]) * cf, b1 = Cb1[c0] + (Cb1[c0 + 1] - Cb1[c0]) * cf;
                const float b = 0.5f * (b0 + b1) - 0.5f;
                const float by = (aIsU ? a : b) * su, ry = (aIsU ? b : a) * sv;
                float Y = 0;
                if (okY) { const int j0 = xj0[(size_t)x]; Y = (Yr[j0] + (Yr[j0 + 1] - Yr[j0]) * xjf[(size_t)x] - bl) * sc; }
                const float gy = -(0.299f * ry + 0.114f * by) * (1.f / 0.587f);
                auto q = [](float v) { v = v < 0 ? 0 : v > 1 ? 1 : v; return (uint8_t)(v * 255.f + 0.5f); };
                px[4 * x] = q(Y + ry); px[4 * x + 1] = q(Y + gy); px[4 * x + 2] = q(Y + by);
            }
        }
        f->seq = ++frameSeq;
        std::lock_guard<std::mutex> lk(mu);
        pubFrame = f;
    }

    // ------------------------------------------------------------ packets
    void packet(const uint8_t* bits) {
        Packet p;
        const int e = parsePacket(bits, p);
        tel.packets++; tel.blocksOk++;
        if (e) tel.packetsCorrected++;
        if (p.address == 1023) { tel.dummyPackets++; return; }
        if (p.address == 0) {
            int tg; std::vector<SiCommand> cmds;
            if (si.push(p, tg, cmds)) siGroup(tg, cmds);
            tel.siGroupsOk = si.groupsOk; tel.siGroupsBad = si.groupsBad;
            return;
        }
        if (p.address == ecmAddr) { tel.ecmPackets++; return; }
        const uint8_t pt = p.data[0];
        const bool bc = pt == kPtBc1 || pt == kPtBc2;
        if (soundAddr < 0 && bc) {
            if (++soundCand[p.address] >= 8) { soundAddr = p.address; char b[80]; snprintf(b, sizeof b, "MAC: sound packets on address %d", soundAddr); say(b); }
            return;
        }
        if (p.address != soundAddr) return;
        if (!bc) return;   // interpretation blocks (BI): not decoded
        tel.soundPackets++;
        if (lastSoundCi >= 0 && p.ci != ((lastSoundCi + 1) & 3)) tel.soundErrors++;
        lastSoundCi = p.ci;
        if (tel.soundScrambled) return;
        if (!soundFromSi && tel.frames < 50) return;   // play only once the TV service description says the sound is clear (or none came for 2 s)
        int16_t x[64];
        tel.soundErrors += (uint64_t)soundDecode(p.data + 1, false, x);
        for (int i = 0; i < 64; i++) {
            const float v = j17.run(x[i] / 8192.f);
            aud.push_back(v);
            sndPow += (double)v * v; sndN++;
        }
        if (aud.size() >= 640) flushAudio();
    }

    void siGroup(int tg, const std::vector<SiCommand>& cmds) {
        tel.siSeen = true;
        for (const SiCommand& c : cmds) {
            if (c.ci == kCiNetwork) {
                for (const SiParam& p : c.params) {
                    if (p.pi == kPiNwo && p.field.size() >= 3) {
                        tel.satChannel = (p.field[0] >> 4) * 10 + (p.field[0] & 15);
                        tel.country = siText(p.field, 3);
                    } else if (p.pi == kPiNwname) tel.networkName = siText(p.field);
                    else if (p.pi == kPiListx && p.field.size() >= 4 && p.field[0] == 0x01 && !soundFromSi) {
                        const int acc = p.field[2] << 8 | p.field[3];
                        if ((acc & 0x3FF) != 0) soundAddr = acc & 0x3FF;
                    }
                }
            } else if (c.ci == kCiTv) {
                std::function<void(const std::vector<SiParam>&, bool)> walk = [&](const std::vector<SiParam>& ps, bool inCa) {
                    bool caHere = inCa;
                    for (const SiParam& p : ps) if (p.pi == kPiAccm) caHere = true;
                    for (const SiParam& p : ps) {
                        if (p.pi == kPiPg) walk(p.group, inCa);
                        else if (p.pi == kPiSref && p.field.size() >= 1) {
                            const std::string n = siText(p.field, 1);
                            if (!n.empty() && n != tel.channelName) { tel.channelName = n; say("MAC: channel \"" + n + "\""); }
                        } else if (p.pi == kPiVconf && p.field.size() >= 1) {
                            vconfKnown = true; vconfWide = ((p.field[0] >> 3) & 7) == 6;
                        } else if (p.pi == kPiAccm && p.field.size() >= 2) {
                            accm = true;
                            const int v = p.field[0] << 8 | p.field[1];
                            if (v >> 15 & 1) ecmAddr = v & 0x3FF;
                            if (p.field.size() >= 3) caByte = p.field[2];
                        } else if (p.pi == kPiTvSound && p.field.size() >= 3) {
                            const int acc = p.field[1] << 8 | p.field[2];
                            if ((acc & 0x3FF) != 0) { soundAddr = acc & 0x3FF; soundFromSi = true; }
                            tel.soundScrambled = caHere;
                        }
                    }
                };
                walk(c.params, false);
            }
        }
        (void)tg;
        if (tel.channelName.empty() && !tel.networkName.empty()) tel.channelName = tel.networkName;
    }

    void flushAudio() {
        if (aud.empty()) return;
        if (tap) tap(aud.data(), aud.size());
        if (!silent.load()) {
            pcmOut.clear();
            for (float s : aud) {   // 32 -> 48 kHz: three outputs per two inputs
                // linear interpolation at 2/3 steps: outputs at input positions n + 0, n + 2/3, n + 4/3 ... (per input sample 1.5 outputs)
                const int nOut = upPhase == 0 ? 2 : 1;
                for (int k = 0; k < nOut; k++) {
                    const float fr = upPhase == 0 ? (k == 0 ? 0.f : 2.f / 3) : 1.f / 3;
                    const float v = upPrev + (s - upPrev) * fr;
                    pcmOut.push_back(v); pcmOut.push_back(v);
                }
                upPhase ^= 1;
                upPrev = s;
            }
            if (!audioOut) { audioOut = std::make_unique<AudioOut>(); audioOut->start(48000); audioOut->setStartThreshold(48000 / 5); }
            audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
            if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();
            audioOut->write(pcmOut.data(), (int)(pcmOut.size() / 2));
        }
        aud.clear();
    }

    // ------------------------------------------------------------ input
    void process() {
        for (int guard = 0; guard < 4000; guard++) {
            if (mode == Search) {
                const int64_t before = scanFrom;
                const Mode m0 = mode;
                acquire();
                if (mode == m0 && scanFrom == before) return;   // waits for more samples
                continue;
            }
            const double end = o + (line625Bits(variant) * 2 + 16) * step + kTaps;   // a whole line and a margin
            if (end >= (double)total()) return;
            decodeLine();
            trim((int64_t)o - 2 * kLineSamples);
        }
    }

    void report() {
        if (tapReq.exchange(false)) { std::lock_guard<std::mutex> lk(mu); tap = tapSet; }
        tel.seq++;
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        const bool tracking = mode == Track;
        const bool framed = tracking && lineNo > 0 && lineCounter - lastFswLine < 2 * kLines;
        tel.state = framed ? 2 : tracking ? 1 : 0;
        tel.dataValid = framed;
        tel.variant = tracking ? variant : -1;
        tel.variantName = tracking ? std::string(variantName(variant)) + "/packet" : "";
        tel.syncQuality = lswWin ? (float)lswOkWin / lswWin : 0.f;
        lswWin = lswOkWin = 0;
        tel.lineHz = tracking ? fs / (kLineSamples * step) : 0;
        tel.lineErrPpm = tracking ? (tel.lineHz / kLineHz - 1) * 1e6 : 0;
        if (cfoN) { tel.cfoHz = (invert ? -1 : 1) * cfoAcc / cfoN * kDevHzPerV * kPreA; cfoAcc = 0; cfoN = 0; }
        if (eyeN) { const double s = std::sqrt(eyeErr / eyeN); tel.snrDb = (float)(20 * std::log10(1.0 / std::max(1e-4, s))); eyeErr = 0; eyeN = 0; }
        if (dataAmpN) { tel.dataAmp = dataAmpAcc / dataAmpN; tel.deviationMhzPerV = (float)(13.5 * tel.dataAmp / kDataAmp); dataAmpAcc = 0; dataAmpN = 0; }
        if (refN) { tel.videoSnrDb = (float)(20 * std::log10(1.0 / std::sqrt(std::max(1e-8, refVar / refN)))); refVar = 0; refN = 0; }
        tel.blackLevel = black; tel.whiteLevel = white;
        tel.bitErrorRate = fswBitsAcc > 0 ? (float)(fswErrAcc / fswBitsAcc) : 0.f;
        if (fswBitsAcc > 96 * 50) { fswErrAcc *= 0.5; fswBitsAcc *= 0.5; }
        tel.inverted = invert;
        if (sdfSeen) {
            tel.chid = sdf.chid; tel.aspectKnown = true; tel.wide = sdf.wide();
            tel.scrambling = vsamText(sdf);
            tel.controlledAccess = sdf.controlled() && sdf.scrambled();
        } else if (vconfKnown) { tel.aspectKnown = true; tel.wide = vconfWide; }
        tel.scrambled = (sdfSeen && sdf.scrambled()) || accm;
        tel.caSystem = accm ? (caByte >= 0 ? caSystemName(caByte) : "conditional access") : (tel.scrambled && tel.controlledAccess ? "conditional access" : "");
        tel.soundAddress = soundAddr;
        tel.soundPresent = framed && tel.soundPackets > 0 && !tel.soundScrambled;
        tel.soundMode = soundAddr < 0 ? "" : tel.soundScrambled ? "scrambled (not played)" : "companded 14/10, 32 kHz, first-level protection";
        if (sndN) { tel.soundLevelDb = (float)(10 * std::log10(sndPow / sndN + 1e-12)); sndPow = 0; sndN = 0; }
        else if (!tel.soundPresent) tel.soundLevelDb = -120;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }

    void feed(const cf32* x, size_t n) {
        const bool mix = offset != 0;
        buf.reserve(buf.size() + n);
        for (size_t i = 0; i < n; i++) {
            cf32 z = x[i];
            if (!std::isfinite(z.real()) || !std::isfinite(z.imag())) z = cf32(0, 0);
            power += (double)std::norm(z);
            if (mix) { const std::complex<double> r = nco.phasor(); z *= cf32((float)r.real(), (float)r.imag()); nco.step(); }
            const cf32 d = z * std::conj(prev);
            prev = z;
            float f = atvdsp::fastAtan2(d.imag(), d.real()) * hzScale;
            if (invert) f = -f;
            buf.push_back(deem.run(f) + 0.5f);
            nIn++;
            if ((double)nIn >= nextReport) {
                nextReport += 0.25 * fs;
                process();
                report();
            }
        }
        nPower += (int64_t)n;
        process();
    }
};

MacReceiver::MacReceiver() : p_(std::make_unique<Impl>()) {}
MacReceiver::~MacReceiver() = default;

void MacReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void MacReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
bool MacReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= macTuning().minSampleRate - 1; }
void MacReceiver::reset() { p_->resetReq = true; }

void MacReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.tapReq.load()) { std::lock_guard<std::mutex> lk(m.mu); m.tap = m.tapSet; m.tapReq = false; }
    if (m.fs <= 0 || n == 0) return;
    m.feed(x, n);
}

bool MacReceiver::telemetry(MacTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

std::shared_ptr<const AtvFrame> MacReceiver::frame(uint64_t& lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->pubFrame || p_->pubFrame->seq <= lastSeq) return nullptr;
    lastSeq = p_->pubFrame->seq;
    return p_->pubFrame;
}

void MacReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void MacReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void MacReceiver::setMuted(bool m) { p_->muted = m; }
void MacReceiver::setSilent(bool s) { p_->silent = s; }
void MacReceiver::setAudioTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tapSet = std::move(cb); p_->tapReq = true; }

ModeTuning macTuning() {
    ModeTuning t;
    t.stdMode = 49; t.id = "mac"; t.name = "MAC";
    t.minMhz = 50; t.maxMhz = 6000; t.defMhz = 1200.0;   // satellite first IF (950 - 2150 MHz) behind an LNB
    t.sampleRate = 20e6;
    t.basebandHz = 20e6;
    t.bandwidthMhz = 27;
    t.minSampleRate = 16e6;
    return t;
}

} // namespace dect2

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bluetooth LE advertising monitor (see ble_rx.h).
#include "dect2/ble/ble_rx.h"
#include "dect2/wirecap/wire.h"
#include "dect2/automation/engine.h"
#include "dect2/results/results_io.h"
#include "dect2/ble/ble_tel.h"
#include "dect2/ble/ble_proto.h"
#include "dect2/net/feeds.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kOut = 4e6;              // demodulator rate: 4 samples per bit
constexpr int kSps = 4;
constexpr int kMean = 160;                // the running mean the bits are sliced against: 40 bits (preamble and access address)
constexpr int kMaxBytes = ble::kMaxPdu + 3;
constexpr int kNeed = (kMaxBytes * 8 + 2) * kSps + 8;   // samples after the access address a candidate waits for
constexpr int kHist = 40 * kSps + 16;     // samples kept before the oldest pending candidate
constexpr int kBlock = 1024;              // input samples mixed at a time

int popc(uint32_t v) { return dect2::bits::popcount(v); }
}

struct BleReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    std::function<void(const ble::Adv&, int, float)> onPacket;
    double rateSet = 0, offsetSet = 0, centerSet = 0;
    std::atomic<bool> resetReq{true}, clearReq{false};
    std::atomic<double> centerHz{0};

    double rate = 0, offset = 0;
    int64_t nIn = 0;
    // front end: mixer, low-pass FIR (computed only where an output sample needs it), linear interpolation to 4 Msps
    std::complex<double> lo{1, 0}, loStep{1, 0};
    int loCount = 0;
    int L = 1;
    std::vector<float> taps, hr, hi;      // hr / hi: the mixed input, the last L samples of the block before first (a window is contiguous)
    std::vector<std::complex<float>> stepTab;   // loStep^j for j < kBlock: the mixer of a block without a recurrence (it vectorises)
    bool twoStage = false;                // the filter in two stages (initFront): taps at the input rate, tapsB at 4 Msps over the ring br / bi
    int LB = 1, bp = 0;
    std::vector<float> tapsB, br, bi;
    double step = 1;
    int64_t outK = 0;
    std::complex<float> yF{0, 0}, prevY{0, 0};
    // demodulator at 4 Msps
    float box[kSps] = {}; int boxPos = 0; float boxSum = 0;
    std::vector<float> meanBuf; int meanPos = 0; double meanSum = 0;
    uint32_t reg[kSps] = {};
    std::vector<float> fb, pb;            // smoothed frequency (Hz) and power, from global index base
    int64_t base = 0, g = -1, skipUntil = 0;
    struct Cand { int64_t at; int errs; };
    std::deque<Cand> cands;
    // results
    BleTelemetry tel, pub;
    std::unordered_map<std::string, size_t> devIdx;
    double power = 0; int64_t nPower = 0;
    double nextReport = 0, lastGood = -100;
    double snrAvg = 0;

    double sigSec() const { return rate > 0 ? nIn / rate : 0; }

    int hintChannel() const {
        const double c = centerHz.load();
        return c > 0 ? ble::advChannelOf((c + offset) / 1e6) : 0;
    }

    void initFront() {
        lo = 1; loCount = 0;
        loStep = rate > 0 ? std::polar(1.0, -2 * kPi * offset / rate) : std::complex<double>(1, 0);
        step = rate / kOut;
        // windowed sinc (Hamming), unit gain at DC, reversed: the dot runs over the history oldest first
        auto design = [](std::vector<float>& h, int n, double fc) {
            h.assign((size_t)n, 0.f);
            double sum = 0;
            for (int i = 0; i < n; i++) {
                const double m = i - (n - 1) / 2.0;
                const double s = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
                const double w = 0.54 - 0.46 * std::cos(2 * kPi * i / (n - 1));
                h[(size_t)i] = (float)(s * w);
                sum += s * w;
            }
            for (float& t : h) t = (float)(t / sum);
            std::reverse(h.begin(), h.end());
        };
        // cut-off 650 kHz over 6.5 us: the GFSK main lobe passes, the noise beyond it does not. When the rate is a whole multiple of 4 Msps (20 Msps
        // for Remote ID) in two stages (3.9): a short filter at the input rate that keeps out what would fold into the passband and the transition of
        // the second one at 4 Msps (cut-off 2 MHz over 1.5 us: flat to 0.9 MHz, stopband from 3.1 MHz), then the same 650 kHz filter at 4 Msps:
        // the same response for less than half the arithmetic. (The demodulator does better at some sampling phases than at others, so the delay is
        // kept as it was: on the generator's packets a delay off by a fraction of a 4 Msps sample cost up to a third of them at 9 dB.)
        twoStage = rate > 0 && step >= 2 && std::fabs(step - std::round(step)) < 1e-9;
        if (twoStage) {
            // its delay a whole number of 4 Msps samples, so the instants the demodulator sees fall where they did with one stage
            L = 2 * (int)std::lround(step) * 3 + 1;
            design(taps, L, 2.0e6 / rate);
            LB = std::max(15, (int)std::ceil(6.5 * kOut / 1e6)) | 1;
            design(tapsB, LB, 650e3 / kOut);
            br.assign((size_t)LB * 2, 0.f); bi.assign((size_t)LB * 2, 0.f); bp = 0;
        } else {
            L = std::max(15, (int)std::ceil(6.5 * rate / 1e6)) | 1;
            design(taps, L, 650e3 / rate);
        }
        hr.assign((size_t)(L + kBlock), 0.f); hi.assign((size_t)(L + kBlock), 0.f);
        stepTab.resize(kBlock);
        yo.assign((size_t)(kBlock + 2), std::complex<float>(0, 0));
        for (int j = 0; j < kBlock; j++) { const std::complex<double> v = std::polar(1.0, -2 * kPi * offset / std::max(rate, 1.0) * j); stepTab[(size_t)j] = std::complex<float>((float)v.real(), (float)v.imag()); }
        outK = 0; nIn = 0;
        yF = prevY = std::complex<float>(0, 0);
    }

    void resetDemod() {
        for (float& b : box) b = 0;
        boxPos = 0; boxSum = 0;
        meanBuf.assign(kMean, 0.f); meanPos = 0; meanSum = 0;
        for (uint32_t& r : reg) r = 0;
        fb.clear(); pb.clear(); base = 0; g = -1; skipUntil = 0;
        cands.clear();
    }

    // The device table and the packet log stay through resets: Clear and a loaded file reach the feed thread's copy through these requests (mu), and the
    // published report at once
    bool resClear = false, resLoad = false;
    BleTelemetry loaded;
    double loadedAt = 0;                   // the report's clock the loaded lists are on
    static void moveLists(BleTelemetry& to, BleTelemetry& from) {
        to.devices = std::move(from.devices); to.log = std::move(from.log);
        from.devices.clear(); from.log.clear();
    }
    static void shiftTimes(BleTelemetry& t, double dt) {
        for (auto& d : t.devices) { d.firstSec += dt; d.lastSec += dt; }
        for (auto& p : t.log) p.sec += dt;
    }
    void takeRequests() {   // feed thread
        std::lock_guard<std::mutex> lk(mu);
        if (!resClear && !resLoad) return;
        if (resClear) { BleTelemetry e; moveLists(tel, e); resClear = false; }
        if (resLoad) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); resLoad = false; }
        reindex();
    }
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        initFront();
        resetDemod();
        power = 0; nPower = 0; nextReport = 0.25 * rate; lastGood = -100;
        tel.inputRate = rate;
        shiftTimes(tel, -tel.timeSec);    // the lists stay (clearResults): the clock starts again
        tel.timeSec = 0;
    }

    void clearAll() {
        const uint64_t seq = tel.seq;
        tel = BleTelemetry(); tel.seq = seq; tel.inputRate = rate;
        devIdx.clear();
        snrAvg = 0;
    }

    // ---------------------------------------------------------------- front end

    // A block of input (at most kBlock samples): mixed to 0 Hz, then the low-pass FIR at the instants an output sample needs (linear interpolation
    // to 4 Msps between two of them). The mixer of the block is the phase at its start times a table of the step's powers, so the loop has no
    // dependency from one sample to the next; the phase at the start goes on in double precision.
    void block(const cf32* x, int k) {
        float* rr = hr.data() + L;
        float* ri = hi.data() + L;
        const std::complex<float> l0((float)lo.real(), (float)lo.imag());
        const std::complex<float>* st = stepTab.data();
        double pw = 0;
        for (int j = 0; j < k; j++) {
            float re = x[j].real(), im = x[j].imag();
            if (!(std::fabs(re) <= 3.0e38f) || !(std::fabs(im) <= 3.0e38f)) re = im = 0;   // not finite
            pw += (double)re * re + (double)im * im;
            const std::complex<float> w = l0 * st[j];
            rr[j] = re * w.real() - im * w.imag();
            ri[j] = re * w.imag() + im * w.real();
        }
        power += pw;
        lo *= std::polar(1.0, -2 * kPi * offset / rate * k);
        if (++loCount >= 64) { lo /= std::abs(lo); loCount = 0; }
        // outputs: sample f of the stream is at buffer index f - nIn + L (the window of L samples ending there starts at f - nIn + 1)
        const int64_t end = nIn + k;
        if (yo.size() < (size_t)((double)k / step) + 4) yo.resize((size_t)((double)k / step) + 4);
        int no = 0;
        for (;;) {
            const double t = outK * step;
            const int64_t f = (int64_t)std::floor(t);
            const float fr = (float)(t - (double)f);
            const bool interp = fr >= 1e-6f;
            if (f + (interp ? 1 : 0) >= end) break;
            float o[2];
            genutil::dot2(taps.data(), hr.data() + (f - nIn + 1), hi.data() + (f - nIn + 1), L, o);
            if (twoStage) {   // no interpolation: the instants fall on input samples
                br[(size_t)bp] = br[(size_t)(bp + LB)] = o[0];
                bi[(size_t)bp] = bi[(size_t)(bp + LB)] = o[1];
                bp = bp + 1 == LB ? 0 : bp + 1;
                genutil::dot2(tapsB.data(), br.data() + bp, bi.data() + bp, LB, o);
                outK++;
                yo[(size_t)(no++)] = std::complex<float>(o[0], o[1]);
                continue;
            }
            std::complex<float> y(o[0], o[1]);
            if (interp) {
                genutil::dot2(taps.data(), hr.data() + (f - nIn + 2), hi.data() + (f - nIn + 2), L, o);
                y = y * (1 - fr) + std::complex<float>(o[0], o[1]) * fr;
            }
            outK++;
            yo[(size_t)(no++)] = y;
        }
        demodBlock(yo.data(), no);
        // the last L samples become the history of the next block
        std::memmove(hr.data(), hr.data() + k, sizeof(float) * (size_t)L);
        std::memmove(hi.data(), hi.data() + k, sizeof(float) * (size_t)L);
        nIn = end;
    }

    // ---------------------------------------------------------------- demodulator

    // atan2 to about 1e-6 rad (a minimax polynomial for atan on [0, 1] and the octant): no library call, and it vectorises. The frequency it gives is
    // off by well under 1 Hz, against 250 kHz of deviation.
    static inline float fastAtan2(float y, float x) {
        const float ax = std::fabs(x), ay = std::fabs(y);
        const float mx = std::max(ax, ay), mn = std::min(ax, ay);
        const float a = mx > 0 ? mn / mx : 0.f;
        const float q = a * a;
        float r = a * (0.99997726f + q * (-0.33262347f + q * (0.19354346f + q * (-0.11643287f + q * (0.05265332f + q * -0.01172120f)))));
        r = ay > ax ? 1.57079632679f - r : r;
        r = x < 0 ? 3.14159265359f - r : r;
        return y < 0 ? -r : r;
    }

    // The demodulator for a block of 4 Msps samples: the instantaneous frequency of all of them first (no dependency between samples), then the
    // smoothing, the slicer and the access address search sample by sample, on locals
    std::vector<std::complex<float>> yo;   // the block's 4 Msps samples
    std::vector<float> fz;
    void demodBlock(const std::complex<float>* y, int cnt) {
        if (cnt <= 0) return;
        fz.resize((size_t)cnt);
        {
            std::complex<float> p = prevY;
            const float kf = (float)(kOut / (2 * kPi));
            for (int j = 0; j < cnt; j++) {
                const std::complex<float> v = y[j];
                const float zr = v.real() * p.real() + v.imag() * p.imag(), zi = v.imag() * p.real() - v.real() * p.imag();   // v conj(p)
                fz[(size_t)j] = fastAtan2(zi, zr) * kf;
                p = v;
            }
            prevY = p;
        }
        const size_t o = fb.size();
        fb.resize(o + (size_t)cnt); pb.resize(o + (size_t)cnt);
        float* fo = fb.data() + o;
        float* po = pb.data() + o;
        for (int j = 0; j < cnt; j++) po[j] = std::norm(y[j]);
        float bs = boxSum;
        double ms = meanSum;
        int bpos = boxPos, mpos = meanPos;
        int64_t gg = g;
        for (int j = 0; j < cnt; j++) {
            const float fHz = fz[(size_t)j];
            bs += fHz - box[bpos]; box[bpos] = fHz; bpos = (bpos + 1) & (kSps - 1);
            const float sm = bs / kSps;
            ms += sm - meanBuf[(size_t)mpos]; meanBuf[(size_t)mpos] = sm; if (++mpos == kMean) mpos = 0;
            fo[j] = sm;
            gg++;
            const int ph = (int)(gg & (kSps - 1));
            const uint32_t bit = sm > (float)(ms / kMean) ? 1u : 0u;
            reg[ph] = (reg[ph] >> 1) | (bit << 31);
            if (gg >= skipUntil) {
                const int e = popc(reg[ph] ^ ble::kAdvAccess);
                if (e <= 1) { cands.push_back({gg, e}); skipUntil = gg + kSps; tel.aaHits++; }
            }
        }
        boxSum = bs; meanSum = ms; boxPos = bpos; meanPos = mpos; g = gg;
    }

    float at(int64_t k) const { return fb[(size_t)(k - base)]; }

    void process() {
        const int64_t end = base + (int64_t)fb.size();
        while (!cands.empty() && cands.front().at + kNeed < end) {
            const Cand c = cands.front();
            cands.pop_front();
            if (c.at < skipUntilPkt) continue;
            decode(c);
        }
        // drop what no candidate needs any more
        const int64_t keepFrom = (cands.empty() ? end : cands.front().at) - kHist;
        if (keepFrom - base > 65536) {
            const size_t drop = (size_t)(keepFrom - base);
            fb.erase(fb.begin(), fb.begin() + (ptrdiff_t)drop);
            pb.erase(pb.begin(), pb.begin() + (ptrdiff_t)drop);
            base += (int64_t)drop;
        }
    }
    int64_t skipUntilPkt = 0;

    void decode(const Cand& cd) {
        // the sample phase with the widest eye over the access address
        int64_t best = -1; double bestEye = -1e30, m1b = 0, m0b = 0;
        for (int d = -1; d < kSps; d++) {
            const int64_t c = cd.at + d;
            if (c - 31 * kSps < base) continue;
            double m1 = 0, m0 = 0; int n1 = 0, n0 = 0;
            for (int k = 0; k < 32; k++) {
                const float v = at(c - (31 - k) * kSps);
                if (ble::kAdvAccess >> k & 1) { m1 += v; n1++; } else { m0 += v; n0++; }
            }
            m1 /= n1; m0 /= n0;
            if (m1 - m0 > bestEye) { bestEye = m1 - m0; best = c; m1b = m1; m0b = m0; }
        }
        if (best < 0 || bestEye < 2 * 60e3) return;   // no eye: noise that happened to match
        double thr = (m1b + m0b) / 2, dev = (m1b - m0b) / 2;
        // the level of a bit with its neighbours' spill (GFSK BT 0.5 and the smoothing): s = dc + c0 a(k) + c1 (a(k-1) + a(k+1)), fitted on the
        // preamble and access address, then a 4-state Viterbi over the PDU (the state: the last two bits)
        int known[40];
        for (int k = 0; k < 8; k++) known[k] = (ble::kAdvAccess & 1) ? (0x55 >> k & 1) : (0xAA >> k & 1);
        for (int k = 0; k < 32; k++) known[8 + k] = ble::kAdvAccess >> k & 1;
        double A[3][3] = {}, B[3] = {};
        for (int k = 1; k < 39; k++) {
            const int64_t pos = best - (int64_t)(39 - k) * kSps;
            if (pos < base) continue;
            const double x[3] = {1, known[k] ? 1.0 : -1.0, (known[k - 1] ? 1.0 : -1.0) + (known[k + 1] ? 1.0 : -1.0)};
            const double v = at(pos);
            for (int r = 0; r < 3; r++) { B[r] += x[r] * v; for (int c = 0; c < 3; c++) A[r][c] += x[r] * x[c]; }
        }
        double cf[3] = {thr, dev, 0};
        {   // Gauss-Jordan on the 3 x 3 normal equations
            double M[3][4];
            for (int r = 0; r < 3; r++) { for (int c = 0; c < 3; c++) M[r][c] = A[r][c]; M[r][3] = B[r]; }
            bool ok = true;
            for (int c = 0; c < 3 && ok; c++) {
                int pv = c;
                for (int r = c + 1; r < 3; r++) if (std::fabs(M[r][c]) > std::fabs(M[pv][c])) pv = r;
                if (std::fabs(M[pv][c]) < 1e-9) { ok = false; break; }
                for (int k = 0; k < 4; k++) std::swap(M[c][k], M[pv][k]);
                for (int r = 0; r < 3; r++) if (r != c) { const double f = M[r][c] / M[c][c]; for (int k = 0; k < 4; k++) M[r][k] -= f * M[c][k]; }
            }
            if (ok) for (int r = 0; r < 3; r++) cf[r] = M[r][3] / M[r][r];
        }
        if (cf[1] < 30e3) return;
        thr = cf[0]; dev = cf[1];
        double var = 0; int nv = 0;
        for (int k = 1; k < 39; k++) {
            const int64_t pos = best - (int64_t)(39 - k) * kSps;
            if (pos < base) continue;
            const double m = cf[0] + cf[1] * (known[k] ? 1 : -1) + cf[2] * ((known[k - 1] ? 1 : -1) + (known[k + 1] ? 1 : -1));
            var += (at(pos) - m) * (at(pos) - m); nv++;
        }
        var /= std::max(1, nv);
        constexpr int N = kMaxBytes * 8;
        static thread_local std::vector<uint8_t> dec;
        dec.assign((size_t)(N + 1) * 4, 0);
        double met[4], nm[4];
        for (double& m : met) m = 1e300;
        met[known[38] << 1 | known[39]] = 0;            // state (a(j-1), a(j)) at j = -1: the access address' last two bits
        double lev[8];                                  // the model's level for (a(j-1), a(j), a(j+1))
        for (int q = 0; q < 8; q++) lev[q] = cf[0] + cf[1] * ((q >> 1 & 1) ? 1 : -1) + cf[2] * (((q >> 2) & 1 ? 1 : -1) + ((q & 1) ? 1 : -1));
        for (int j = -1; j < N - 1; j++) {
            const double v = at(best + (int64_t)(j + 1) * kSps);
            for (double& m : nm) m = 1e300;
            uint8_t* d = &dec[(size_t)(j + 1) * 4];
            for (int st = 0; st < 4; st++) {
                if (met[st] >= 1e299) continue;
                for (int b = 0; b < 2; b++) {
                    const double e = v - lev[st << 1 | b];
                    const double m = met[st] + e * e;
                    const int ns = (st << 1 | b) & 3;
                    if (m < nm[ns]) { nm[ns] = m; d[ns] = (uint8_t)st; }
                }
            }
            for (int k = 0; k < 4; k++) met[k] = nm[k];
        }
        int st = 0;
        for (int k = 1; k < 4; k++) if (met[k] < met[st]) st = k;
        static thread_local std::vector<uint8_t> bits;
        bits.assign(N, 0);
        for (int j = N - 1; j >= 0; j--) {                // the state after step j - 1 holds a(j) in its low bit
            bits[(size_t)j] = (uint8_t)(st & 1);
            st = dec[(size_t)j * 4 + (size_t)st];
        }
        uint8_t raw[kMaxBytes];
        for (int j = 0; j < kMaxBytes; j++) {
            uint8_t v = 0;
            for (int b = 0; b < 8; b++) if (bits[(size_t)(8 * j + b)]) v |= (uint8_t)(1 << b);
            raw[j] = v;
        }
        const int hint = hintChannel();
        int order[3] = {37, 38, 39};
        if (hint) { order[0] = hint; int k = 1; for (int c = 37; c <= 39; c++) if (c != hint) order[k++] = c; }
        for (int ch : order) {
            uint8_t pdu[kMaxBytes];
            memcpy(pdu, raw, 2);
            ble::whiten(pdu, 2, ch);
            const int len = pdu[1];
            const int total = 2 + len + 3;
            memcpy(pdu, raw, (size_t)total);
            ble::whiten(pdu, (size_t)total, ch);
            const uint32_t crc = ble::crc24(pdu, (size_t)(2 + len));
            const uint32_t got = (uint32_t)pdu[2 + len] | (uint32_t)pdu[3 + len] << 8 | (uint32_t)pdu[4 + len] << 16;
            if (crc != got) continue;
            ble::Adv a;
            if (!ble::parsePdu(pdu, (size_t)(2 + len), a)) continue;
            // the packet is good
            const int64_t pEnd = best + (int64_t)total * 8 * kSps;
            skipUntilPkt = pEnd;
            skipUntil = std::max(skipUntil, pEnd);
            double pw = 0; int np = 0;
            for (int64_t k = std::max(base, best - 32 * kSps); k < pEnd; k++) { pw += pb[(size_t)(k - base)]; np++; }
            const float rssi = (float)(10 * std::log10(pw / std::max(1, np) + 1e-20));
            const double snr = var > 0 ? 10 * std::log10(dev * dev / var) : 40;
            snrAvg = tel.blocksOk == 0 ? snr : 0.8 * snrAvg + 0.2 * snr;
            tel.cfoHz = thr;
            tel.deviationHz = (float)dev;
            if (wire::on(wire::Ble)) wire::Hub::get().publish(wire::Ble, wire::bleLl(pdu, (size_t)total));   // de-whitened PDU and CRC
            accept(a, ch, rssi, (double)best / kOut);
            return;
        }
        if (cd.errs == 0) tel.blocksBad++;
    }

    void accept(const ble::Adv& a, int ch, float rssi, double sec) {
        tel.blocksOk++;
        if (onPacket) onPacket(a, ch, rssi);
        if (a.type >= 0 && a.type < 8) tel.perType[a.type]++;
        tel.perChannel[ch - 37]++;
        tel.lastChannel = ch;
        lastGood = sigSec();
        const int64_t wall = (int64_t)time(nullptr);
        // the log
        BlePacket p;
        p.wallTime = wall; p.sec = sec; p.channel = ch; p.type = a.type; p.rssi = rssi; p.len = a.len; p.info = a.summary;
        const bool sentByPeer = a.type == ble::ScanReq || a.type == ble::ConnectInd;
        if (sentByPeer) { p.addr = ble::addrText(a.peer); p.random = a.txRandom; }
        else if (a.hasAdv) { p.addr = ble::addrText(a.adv); p.random = a.txRandom; }
        if (Feeds::get().wantBle()) Feeds::get().ble(p.addr, a.name, rssi);
        if (automation::wants(automation::FBle) && !p.addr.empty()) {   // the automations (automation/engine.h)
            automation::Event ev("ble.device");
            ev.at(automation::wallNow(), "BLE", 0);
            ev.str("mac", p.addr).str("name", a.name).num("rssi_db", rssi);
            automation::emit(std::move(ev));
        }
        tel.log.insert(tel.log.begin(), std::move(p));
        if (tel.log.size() > kBleLogMax) tel.log.pop_back();
        // the device that sent it
        if (sentByPeer) {
            BleDevice& d = device(a.peer, a.txRandom);
            seen(d, a, ch, rssi, wall);
            if (a.type == ble::ScanReq) { BleDevice& t = device(a.adv, a.rxRandom); t.scanned = true; }
        } else if (a.hasAdv) {
            BleDevice& d = device(a.adv, a.txRandom);
            seen(d, a, ch, rssi, wall);
            if (!a.name.empty() && (a.nameComplete || d.name.empty() || d.name.size() <= a.name.size())) d.name = a.name;
            if (!a.vendor.empty()) d.vendor = a.vendor;
            if (!a.beacon.empty()) d.beacon = a.beacon;
            if (a.company >= 0) {
                d.company = a.company;
                std::string h; char b[4];
                for (size_t i = 0; i < a.mfr.size() && i < 32; i++) { snprintf(b, sizeof b, "%02X", a.mfr[i]); h += b; }
                d.mfrHex = h;
            }
            if (!a.services.empty()) {
                std::string s;
                for (const auto& x : a.services) { if (!s.empty()) s += ", "; s += x; }
                d.services = s;
            }
            if (a.txPower != 127) d.txPower = a.txPower;
            if (a.flags >= 0) d.flags = a.flags;
            if (a.type == ble::AdvInd || a.type == ble::AdvDirectInd || (a.type == ble::AdvExtInd && (a.advMode & 1))) d.connectable = true;
            if (a.type == ble::AdvInd || a.type == ble::AdvScanInd || (a.type == ble::AdvExtInd && (a.advMode & 2))) d.scannable = true;
            if (a.type == ble::AdvExtInd) d.extended = true;
        }
    }

    BleDevice& device(const uint8_t addr[6], bool random) {
        const std::string key = ble::addrText(addr) + (random ? "r" : "p");
        auto it = devIdx.find(key);
        if (it != devIdx.end()) return tel.devices[it->second];
        if (tel.devices.size() >= kBleDevMax) {   // forget the longest unheard
            size_t old = 0;
            for (size_t i = 1; i < tel.devices.size(); i++) if (tel.devices[i].lastSec < tel.devices[old].lastSec) old = i;
            tel.devices.erase(tel.devices.begin() + (ptrdiff_t)old);
            reindex();
        }
        BleDevice d;
        d.addr = ble::addrText(addr);
        d.random = random;
        d.kind = ble::addrKind(random, addr);
        d.firstSec = sigSec();
        d.firstWall = (int64_t)time(nullptr);
        tel.devices.push_back(std::move(d));
        devIdx[key] = tel.devices.size() - 1;
        if (log && tel.devices.size() <= 20) log("BLE: new device " + ble::addrText(addr) + " (" + ble::addrKindName(ble::addrKind(random, addr)) + ")");
        return tel.devices.back();
    }

    void reindex() {
        devIdx.clear();
        for (size_t i = 0; i < tel.devices.size(); i++) devIdx[tel.devices[i].addr + (tel.devices[i].random ? "r" : "p")] = i;
    }

    void seen(BleDevice& d, const ble::Adv& a, int ch, float rssi, int64_t wall) {
        d.packets++;
        d.rssi = rssi;
        d.rssiHist.push_back(rssi);
        if (d.rssiHist.size() > kBleRssiHist) d.rssiHist.erase(d.rssiHist.begin());
        d.lastSec = sigSec();
        d.lastWall = wall;
        if (a.type >= 0 && a.type < 32) d.pduMask |= 1u << a.type;
        d.chanMask |= (uint8_t)(1 << (ch - 37));
    }

    void report() {
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.channel = hintChannel();
        tel.snrDb = (float)snrAvg;
        tel.state = sigSec() - lastGood < 5 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        BleTelemetry out = tel;
        std::stable_sort(out.devices.begin(), out.devices.end(), [](const BleDevice& x, const BleDevice& y) { return x.lastSec > y.lastSec; });
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(out);
    }
};

BleReceiver::BleReceiver() : p_(std::make_unique<Impl>()) {}
BleReceiver::~BleReceiver() = default;

void BleReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void BleReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void BleReceiver::setCenterHz(double hz) { p_->centerHz = hz; }
bool BleReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= bleTuning().minSampleRate - 1; }
void BleReceiver::reset() { p_->resetReq = true; }
void BleReceiver::clearDevices() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);   // the report at once; the feed thread's lists (and counters) on its next block
        BleTelemetry e;
        Impl::moveLists(p_->pub, e);
        p_->resLoad = false;
        p_->pub.seq++;
    }
    p_->clearReq = true;
}
void BleReceiver::clearResults() { clearDevices(); }

void BleReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    m.takeRequests();
    if (m.rate < bleTuning().minSampleRate - 1 || n == 0) {
        if (m.rate > 0) { m.nIn += (int64_t)n; if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); } }
        return;
    }
    for (size_t i = 0; i < n;) {
        // in blocks, cut at the reports
        const int64_t toReport = std::max<int64_t>(1, (int64_t)std::ceil(m.nextReport) - m.nIn);
        const int k = (int)std::min<int64_t>({(int64_t)(n - i), (int64_t)kBlock, toReport});
        m.block(x + i, k);
        i += (size_t)k;
        if ((double)m.nIn >= m.nextReport) { m.process(); m.nextReport += 0.25 * m.rate; m.report(); }
    }
    m.nPower += (int64_t)n;
    m.process();
}

std::string BleReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.devices, p_->pub.log);
}
bool BleReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    BleTelemetry l;
    if (!results::unpack(blob, t0, l.devices, l.log)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    BleTelemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->resLoad = true;
    p_->pub.seq++;
    return true;
}
bool BleReceiver::telemetry(BleTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void BleReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void BleReceiver::setPacketCallback(std::function<void(const ble::Adv&, int, float)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->onPacket = std::move(cb); }

ModeTuning bleTuning() {
    ModeTuning t;
    t.stdMode = 53; t.id = "ble"; t.name = "Bluetooth LE";
    t.minMhz = 2400; t.maxMhz = 2484; t.defMhz = 2402;   // advertising channel 37
    t.sampleRate = 4000000;                  // one advertising channel at a time (all three span 78 MHz)
    t.basebandHz = 0;
    t.bandwidthMhz = 2;
    t.minSampleRate = 4000000;
    t.tuneOffsetHz = 1000000;                // the radio sits 1 MHz above the channel, away from its DC spike
    return t;
}

} // namespace dect2

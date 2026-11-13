// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi network monitor (see wifi_rx.h).
#include "dect2/wifi/wifi_rx.h"
#include "dect2/wirecap/wire.h"
#include <random>
#include "dect2/results/results_io.h"
#include "dect2/wifi/wifi_tel.h"
#include "dect2/wifi/wifi_proto.h"
#include "dect2/wifi/wifi_chan.h"
#include "dect2/wifi/wifi_ofdm.h"
#include "dect2/engine/channelizer.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cstdint>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kOut = 20e6;             // working rate: 20 samples a 1 Mbit/s symbol
constexpr int kSps = 20;
constexpr int kBack = 10;                 // symbols before the SFD's last one where a decode starts (inside SYNC)
constexpr int kMargin = 3 * kSps;         // samples past a packet's end a decode waits for (the timing tracker may move)
constexpr int kWin = 12;                  // samples over which the 20 phase detectors' SFD hits are compared

int popc(uint64_t v) { return dect2::bits::popcount(v); }

uint16_t sfdPattern() {   // the SFD as the history register holds it (newest bit lowest)
    uint16_t p = 0;
    for (int k = 0; k < 16; k++) p = (uint16_t)(p << 1 | (wifi::kSfd >> k & 1));
    return p;
}
const uint16_t kSfdPat = sfdPattern();
uint16_t sfdShortPattern() {
    uint16_t p = 0;
    for (int k = 0; k < 16; k++) p = (uint16_t)(p << 1 | (wifi::kSfdShort >> k & 1));
    return p;
}
const uint16_t kSfdShortPat = sfdShortPattern();
constexpr int kAllOne = 1 << 30;          // demod(): no DQPSK part
} // namespace

struct WifiReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    std::function<void(const uint8_t*, size_t, float)> onFrame;
    std::function<void(const uint8_t*, size_t, const WifiFrameMeta&)> onMeta;
    std::atomic<bool> ofdmQpsk{false};
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true}, clearReq{false};
    std::atomic<double> centerHz{0};
    std::atomic<bool> centerChanged{false};
    double reportSec = 0.25;            // seconds of signal between reports (tests shorten it)
    // OFDM receivers for 5 / 6 GHz (and the 6 Mbit/s beacons some 2.4 GHz APs send): one per 20 MHz channel listened to
    struct Sub {
        ChannelFilter cf;
        wifi::ofdm::OfdmRx rx;
        double offHz = 0;               // frequency relative to the radio's centre
        double freqMhz = 0;             // absolute (0: a file, unknown)
    };
    std::vector<std::unique_ptr<Sub>> subs;
    std::vector<cf32> san, tmp;
    bool dsss = true;                   // the 802.11b chain runs: 2.4 GHz or an unknown band
    int band = 0;

    double rate = 0, offset = 0;
    int64_t nIn = 0;
    // front end: mixer and linear interpolation to 20 Msps
    std::complex<double> lo{1, 0}, loStep{1, 0};
    int loCount = 0;
    static constexpr int kBlock = 1024;   // input samples mixed at a time
    std::vector<std::complex<float>> stepTab, mixed;   // loStep^j for j < kBlock (the mixer without a recurrence); a mixed block
    std::vector<float> linR, linI, corR, corI;         // at 20 Msps: the last kSps - 1 mixed samples and the block after them; the block's correlations
    double step = 1;
    bool direct = true;
    int64_t outK = 0;
    std::complex<float> yPrev{0, 0};
    // Barker correlator over one symbol (20 samples)
    float taps[kSps] = {};
    float hr[2 * kSps] = {}, hi[2 * kSps] = {};
    int hp = 0;
    int64_t nOut = 0;
    // correlator output and input power, from global index base
    std::vector<float> cr, ci, pw;
    int64_t base = 0, g = -1;
    int64_t doneTo = INT64_MAX;          // inside workBlock: cr / pw hold the block ahead of the search; process() looks only up to here
    // the 20 phase detectors
    struct Chain { std::complex<float> prev{0, 0}; uint8_t scr = 0; uint64_t h = 0; float e = 0; };
    Chain ch[kSps];
    bool winOpen = false, winShort = false; int64_t winStart = 0, winBest = 0; float winE = 0;
    // the packet being decoded; shortPre: the short preamble (header at 2 Mbit/s); rate: the MPDU's (1 or 2 Mbit/s)
    struct Pend { int64_t at = 0, alt = 0; int stage = 0; int64_t need = 0; int bytes = 0; bool shortPre = false; int rate = 1; };
    bool pending = false;
    Pend pd;
    int64_t busyUntil = 0;
    // results
    WifiTelemetry tel, pub;
    std::unordered_map<std::string, size_t> apIdx;
    double power = 0; int64_t nPower = 0;
    double nextReport = 0, lastGood = -100;
    double snrAvg = 0;
    bool snrSet = false;

    double sigSec() const { return rate > 0 ? nIn / rate : 0; }

    int tunedChannel() const {
        const double c = centerHz.load();
        if (c <= 0) return 0;
        const double mhz = (c + offset) / 1e6;
        const int b = wifi::bandOfMhz(mhz);
        return b < 0 ? 0 : wifi::bandChannelOf(b, mhz, b == wifi::Band24 ? 1.0 : 2.5);
    }

    // The OFDM receivers: one for the signal at the offset (a 20 - 25 Msps capture), or one per 20 MHz channel of a wide capture on 5 / 6 GHz
    // (a 20 MHz channel needs 25 Msps of span plus its neighbours: channelsInSpan). Called on the feed thread.
    void rebuildSubs() {
        subs.clear();
        const double c = centerHz.load();
        const double mhz = (c + offset) / 1e6;
        band = c > 0 ? std::max(0, wifi::bandOfMhz(mhz)) : 0;
        dsss = c <= 0 || band == wifi::Band24;
        std::vector<double> offs, freqs;
        if (c > 0 && band != wifi::Band24 && rate >= 43.5e6) {
            for (int ch : wifi::channelsInSpan(band, c / 1e6, rate)) { const double f = wifi::bandChannelMhz(band, ch); offs.push_back((f - c / 1e6) * 1e6); freqs.push_back(f); }
        }
        if (offs.empty()) { offs.push_back(offset); freqs.push_back(c > 0 ? mhz : 0); }
        for (size_t i = 0; i < offs.size() && rate >= kOut - 1; i++) {
            auto sb = std::make_unique<Sub>();
            sb->offHz = offs[i]; sb->freqMhz = freqs[i];
            sb->cf.configure(rate, offs[i], kOut);
            Sub* sp = sb.get();
            sb->rx.setCallback([this, sp](const uint8_t* p, size_t n, const wifi::ofdm::Info& in) { onOfdm(*sp, p, n, in); });
            sb->rx.setAllowQpsk(ofdmQpsk.load());
            subs.push_back(std::move(sb));
        }
        tel.subChannels = (int)subs.size();
    }

    void onOfdm(const Sub& sb, const uint8_t* p, size_t n, const wifi::ofdm::Info& in) {
        tel.ofdmFrames++;
        if (in.snrDb > 0) { snrAvg = snrSet ? 0.8 * snrAvg + 0.2 * in.snrDb : in.snrDb; snrSet = true; }
        tel.cfoHz = in.cfoHz;
        const int b = sb.freqMhz > 0 ? wifi::bandOfMhz(sb.freqMhz) : -1;
        const int ch = b >= 0 ? wifi::bandChannelOf(b, sb.freqMhz, b == wifi::Band24 ? 1.0 : 2.5) : 0;
        tel.phy = 1;
        if (onMeta) { WifiFrameMeta m; m.rssiDbfs = in.rssiDb; m.rateKbps = in.rateMbps * 1000; m.ofdm = true; m.cfoHz = in.cfoHz; m.snrDb = in.snrDb; onMeta(p, n, m); }
        accept(p, n, (int)n + 4, in.rssiDb, sigSec(), ch, b);
    }

    void initFront() {
        lo = 1; loCount = 0;
        loStep = rate > 0 ? std::polar(1.0, -2 * kPi * offset / rate) : std::complex<double>(1, 0);
        stepTab.resize(kBlock); mixed.resize(kBlock);
        for (int j = 0; j < kBlock; j++) { const std::complex<double> v = std::polar(1.0, rate > 0 ? -2 * kPi * offset / rate * j : 0.0); stepTab[(size_t)j] = std::complex<float>((float)v.real(), (float)v.imag()); }
        direct = std::fabs(rate - kOut) < 1;
        step = rate / kOut;
        outK = 0; nIn = 0;
        yPrev = 0;
        for (int i = 0; i < kSps; i++) taps[i] = (float)wifi::kBarker[i * 11 / kSps] / kSps;
        for (float& v : hr) v = 0;
        for (float& v : hi) v = 0;
        hp = 0; nOut = 0;
        linR.assign((size_t)(kSps - 1 + kBlock), 0.f); linI.assign((size_t)(kSps - 1 + kBlock), 0.f);
        corR.assign((size_t)kBlock, 0.f); corI.assign((size_t)kBlock, 0.f);
    }

    void resetDemod() {
        cr.clear(); ci.clear(); pw.clear(); base = 0; g = -1;
        for (Chain& c : ch) c = Chain();
        winOpen = false; pending = false; busyUntil = 0;
    }

    // The network table and the frame log stay through resets: Clear and a loaded file reach the feed thread's copy through these requests (mu), and the
    // published report at once
    bool resClear = false, resLoad = false;
    WifiTelemetry loaded;
    double loadedAt = 0;                   // the report's clock the loaded lists are on
    static void moveLists(WifiTelemetry& to, WifiTelemetry& from) {
        to.aps = std::move(from.aps); to.log = std::move(from.log);
        from.aps.clear(); from.log.clear();
    }
    static void shiftTimes(WifiTelemetry& t, double dt) {
        for (auto& a : t.aps) { a.firstSec += dt; a.lastSec += dt; }
        for (auto& l : t.log) l.sec += dt;
    }
    void takeRequests() {   // feed thread
        std::lock_guard<std::mutex> lk(mu);
        if (!resClear && !resLoad) return;
        if (resClear) { WifiTelemetry e; moveLists(tel, e); resClear = false; }
        if (resLoad) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); resLoad = false; }
        reindex();
    }
    void reindex() {
        apIdx.clear();
        for (size_t i = 0; i < tel.aps.size(); i++) apIdx[tel.aps[i].bssid] = i;
    }
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        initFront();
        resetDemod();
        power = 0; nPower = 0; nextReport = reportSec * rate; lastGood = -100;
        tel.inputRate = rate;
        rebuildSubs();
        shiftTimes(tel, -tel.timeSec);    // the lists stay (clearResults): the clock starts again
        tel.timeSec = 0;
    }

    void clearAll() {
        const uint64_t seq = tel.seq;
        tel = WifiTelemetry(); tel.seq = seq; tel.inputRate = rate;
        apIdx.clear();
        snrAvg = 0; snrSet = false;
    }

    // ---------------------------------------------------------------- front end

    // A block of input (at most kBlock samples), mixed to 0 Hz with the phase at its start times a table of the step's powers (no dependency
    // from one sample to the next, so it vectorises; the phase at the start goes on in double precision), then sample by sample through the
    // correlator. A frame whose samples are all in is decoded at once, not at the end of feed(): a pending frame holds the search for the next.
    void block(const cf32* x, int k) {
        const std::complex<float> l0((float)lo.real(), (float)lo.imag());
        const std::complex<float>* st = stepTab.data();
        std::complex<float>* y = mixed.data();
        double pwr = 0;
        for (int j = 0; j < k; j++) {
            float re = x[j].real(), im = x[j].imag();
            if (!(std::fabs(re) <= 3.0e38f) || !(std::fabs(im) <= 3.0e38f)) re = im = 0;   // not finite
            pwr += (double)re * re + (double)im * im;
            const std::complex<float> w = l0 * st[j];
            y[j] = std::complex<float>(re * w.real() - im * w.imag(), re * w.imag() + im * w.real());
        }
        power += pwr;
        lo *= std::polar(1.0, -2 * kPi * offset / rate * k);
        if (++loCount >= 64) { lo /= std::abs(lo); loCount = 0; }
        if (direct) { workBlock(y, k); return; }
        for (int j = 0; j < k; j++) {
            {
                // linear interpolation between the previous input sample (index nIn - 1) and this one
                const int64_t i = nIn;
                for (;;) {
                    const double t = outK * step;
                    if (t > (double)i) break;
                    const float fr = (float)(t - (double)(i - 1));
                    work(yPrev * (1 - fr) + y[j] * fr);
                    outK++;
                }
                yPrev = y[j];
                nIn++;
            }
            if (pending && base + (int64_t)cr.size() - kSps >= pd.need) process();
        }
    }

    // work() for a block at 20 Msps: the power and the Barker correlations of the whole block first (a short FIR along the block, which
    // vectorises), then the preamble search sample by sample
    void workBlock(const std::complex<float>* y, int k) {
        float* lr = linR.data();
        float* li = linI.data();
        for (int j = 0; j < k; j++) { lr[kSps - 1 + j] = y[j].real(); li[kSps - 1 + j] = y[j].imag(); }
        const size_t o = pw.size();
        pw.resize(o + (size_t)k); cr.resize(o + (size_t)k); ci.resize(o + (size_t)k);
        float* pp = pw.data() + o;
        for (int j = 0; j < k; j++) pp[j] = lr[kSps - 1 + j] * lr[kSps - 1 + j] + li[kSps - 1 + j] * li[kSps - 1 + j];
        float* ar = corR.data();
        float* ai = corI.data();
        for (int j = 0; j < k; j++) { ar[j] = 0; ai[j] = 0; }
        for (int t = 0; t < kSps; t++) {   // the window of sample j: samples j - kSps + 1 .. j, oldest first
            const float h = taps[t];
            const float* sr = lr + t;
            const float* si = li + t;
            for (int j = 0; j < k; j++) { ar[j] += h * sr[j]; ai[j] += h * si[j]; }
        }
        // the history of the next block, and the ring work() uses
        for (int t = 0; t < kSps - 1; t++) { lr[t] = lr[k + t]; li[t] = li[k + t]; }
        for (int j = std::max(0, k - kSps); j < k; j++) { hr[hp] = hr[hp + kSps] = y[j].real(); hi[hp] = hi[hp + kSps] = y[j].imag(); hp = hp + 1 == kSps ? 0 : hp + 1; }
        // cr / ci / pw hold the whole block now; process() is held to the samples searched so far (doneTo), so it trims nothing still needed
        const int64_t first = base + (int64_t)o;   // global index of the block's first sample in cr / pw
        for (int j = 0; j < k; j++) {
            g++;
            nIn++;
            const int64_t at = first + j - base;
            if (nOut < kSps && ++nOut < kSps) { cr[(size_t)at] = 0; ci[(size_t)at] = 0; continue; }   // the first kSps - 1 samples: no full window yet
            cr[(size_t)at] = ar[j]; ci[(size_t)at] = ai[j];
            detect(std::complex<float>(ar[j], ai[j]));
            if (pending && first + j + 1 - kSps >= pd.need) { doneTo = first + j + 1; process(); doneTo = INT64_MAX; }
        }
    }

    __attribute__((always_inline)) void work(std::complex<float> y) {   // inlined: a call per sample stored the two floats and loaded them as one 8-byte value (store-forwarding stall)
        hr[hp] = hr[hp + kSps] = y.real();
        hi[hp] = hi[hp + kSps] = y.imag();
        hp = hp + 1 == kSps ? 0 : hp + 1;
        pw.push_back(std::norm(y));
        if (++nOut < kSps) { cr.push_back(0); ci.push_back(0); g++; return; }
        float o[2];
        genutil::dot2(taps, hr + hp, hi + hp, kSps, o);   // the symbol that started kSps - 1 samples ago
        cr.push_back(o[0]); ci.push_back(o[1]);
        g++;
        detect(std::complex<float>(o[0], o[1]));
    }

    // ---------------------------------------------------------------- preamble search

    void detect(std::complex<float> c) {
        const int64_t at = g - (kSps - 1);   // where this correlation's symbol starts (cr / pw are indexed by sample)
        Chain& k = ch[(size_t)(at % kSps)];
        const std::complex<float> d = c * std::conj(k.prev);
        k.prev = c;
        const uint8_t bit = d.real() < 0 ? 1 : 0;
        const uint8_t desc = (uint8_t)(bit ^ (k.scr >> 3 & 1) ^ (k.scr >> 6 & 1));
        k.scr = (uint8_t)(((k.scr << 1) | bit) & 0x7F);
        k.h = k.h << 1 | desc;
        k.e = 0.9f * k.e + 0.1f * std::norm(c);
        if (winOpen && at > winStart + kWin) {
            winOpen = false;
            tel.sfdHits++;
            if (!pending && winBest >= busyUntil) {
                // The phase whose SFD came first is not always the correlation peak: a radio rate resampled to 20 Msps (24 or 26 Msps) lets a weak
                // phase on the Barker sidelobes decode the preamble first while the peak's own detector misses the window. The header is read at
                // the phase with the most energy within half a symbol, and at the first phase when that fails.
                int64_t peak = winBest; float pe = -1;
                for (int o = -kSps / 2; o < kSps / 2; o++) {
                    const float e = ch[(size_t)(((winBest + o) % kSps + kSps) % kSps)].e;
                    if (e > pe) { pe = e; peak = winBest + o; }
                }
                pending = true; pd = Pend(); pd.at = peak; pd.alt = winBest; pd.shortPre = winShort;
                pd.need = std::max(peak, winBest) + (int64_t)(winShort ? 25 : 49) * kSps + kMargin;   // the header: 48 bits at 1 Mbit/s, or at 2 Mbit/s after the short SFD
                busyUntil = pd.need;
            }
        }
        if ((uint16_t)k.h == kSfdPat && popc(k.h >> 16 & 0xFFFFFFFFull) >= 31 && at >= busyUntil) {
            if (!winOpen) { winOpen = true; winStart = at; winBest = at; winE = k.e; winShort = false; }
            else if (k.e > winE) { winE = k.e; winBest = at; winShort = false; }
        } else if ((uint16_t)k.h == kSfdShortPat && popc(k.h >> 16 & 0xFFFFFFFFull) <= 1 && at >= busyUntil) {   // short preamble: SYNC is scrambled zeros
            if (!winOpen) { winOpen = true; winStart = at; winBest = at; winE = k.e; winShort = true; }
            else if (k.e > winE) { winE = k.e; winBest = at; winShort = true; }
        }
    }

    // ---------------------------------------------------------------- packet decode

    // the correlation of the symbol that starts at sample i (stored when its last sample came in)
    float mag(int64_t i) const {
        i += kSps - 1;
        if (i < base || i >= base + (int64_t)cr.size()) return 0;
        const size_t j = (size_t)(i - base);
        return std::sqrt(cr[j] * cr[j] + ci[j] * ci[j]);
    }
    std::complex<float> cAt(int64_t i) const {
        i += kSps - 1;
        if (i < base || i >= base + (int64_t)cr.size()) return 0;
        const size_t j = (size_t)(i - base);
        return {cr[j], ci[j]};
    }

    struct Demod { std::vector<uint8_t> bits; double angSum = 0, angSq = 0; int nAng = 0; int64_t end = 0; };

    // nBits descrambled bits after the SFD, with an early / late timing tracker. Bits from qpskFrom on come two to a symbol (DQPSK, clause
    // 16.2.3.6: the phase change of dibit d0 d1 is 00 0, 01 pi/2, 11 pi, 10 -pi/2), the ones before it one to a symbol (DBPSK).
    static int symbolsFor(int nBits, int qpskFrom) { return qpskFrom >= nBits ? nBits : qpskFrom + (nBits - qpskFrom + 1) / 2; }
    void demod(int64_t at, int nBits, int qpskFrom, Demod& out) {
        out.bits.clear();
        int64_t t = at - (int64_t)kBack * kSps;
        std::complex<float> prev = cAt(t);
        uint8_t scr = 0;
        double acc = 0;
        auto descr = [&](uint8_t bit) { const uint8_t desc = (uint8_t)(bit ^ (scr >> 3 & 1) ^ (scr >> 6 & 1)); scr = (uint8_t)(((scr << 1) | bit) & 0x7F); return desc; };
        const int nsym = symbolsFor(nBits, qpskFrom);
        for (int j = -kBack + 1; j <= nsym; j++) {
            t += kSps;
            const float m0 = mag(t - 1), mc = mag(t), m1 = mag(t + 1);
            const double e = (m1 - m0) / (std::max({m0, mc, m1}) + 1e-12);   // normalised by the largest: a small mag(t) off the peak does not throw the tracker
            acc = 0.9 * acc + e;
            if (acc > 2.5) { t++; acc = 0; } else if (acc < -2.5) { t--; acc = 0; }
            const std::complex<float> c = cAt(t);
            const std::complex<float> d = c * std::conj(prev);
            prev = c;
            const bool two = j >= 1 && (int)out.bits.size() >= qpskFrom;
            double a;
            if (!two) {
                const uint8_t bit = d.real() < 0 ? 1 : 0;
                const uint8_t desc = descr(bit);
                if (j >= 1) out.bits.push_back(desc);
                a = std::arg(bit ? -d : d);
            } else {
                // the nearest of 0, pi/2, pi, -pi/2
                const double ph = std::arg(d);
                const int qd = ((int)std::lround(ph / (kPi / 2)) % 4 + 4) % 4;   // 0 .. 3 quarter turns
                static const uint8_t kD0[4] = {0, 0, 1, 1}, kD1[4] = {0, 1, 1, 0};   // 0: 00, 1 (+pi/2): 01, 2 (pi): 11, 3 (-pi/2): 10
                out.bits.push_back(descr(kD0[qd]));
                if ((int)out.bits.size() < nBits) out.bits.push_back(descr(kD1[qd]));
                a = std::remainder(ph - qd * (kPi / 2), 2 * kPi);
            }
            if (j >= -kBack + 8) { out.angSum += a; out.angSq += a * a; out.nAng++; }
        }
        out.end = t + kSps;
    }
    void emitMeta(const uint8_t* f, size_t n, float rssi, bool ok, const Demod& dm) {
        if (!onMeta) return;
        WifiFrameMeta m;
        m.rssiDbfs = rssi; m.rateKbps = pd.rate * 1000; m.shortPreamble = pd.shortPre; m.fcsOk = ok;
        if (dm.nAng > 8) {
            const double mu = dm.angSum / dm.nAng, var = std::max(1e-6, dm.angSq / dm.nAng - mu * mu);
            m.snrDb = (float)(10 * std::log10(1 / var));
            m.cfoHz = mu / (2 * kPi) * wifi::kSymRate;
        }
        onMeta(f, n, m);
    }
    float frameRssi(const Demod& dm) const {
        double p = 0; int np = 0;
        for (int64_t k = std::max(base, pd.at - (int64_t)kBack * kSps); k < std::min(dm.end, base + (int64_t)pw.size()); k++) { p += pw[(size_t)(k - base)]; np++; }
        return (float)(10 * std::log10(p / std::max(1, np) + 1e-20));
    }

    void process() {
        const int64_t end = std::min(base + (int64_t)cr.size(), doneTo) - kSps;   // the last symbol start with its correlation in (and searched)
        while (pending && end >= pd.need) {
            if (pd.stage == 0) {
                Demod dm;
                auto headerOk = [&](int64_t at) {
                    demod(at, 48, pd.shortPre ? 0 : kAllOne, dm);
                    if (dm.bits.size() < 48) return false;
                    uint16_t crc = 0;
                    for (int k = 0; k < 16; k++) crc = (uint16_t)(crc << 1 | dm.bits[(size_t)(32 + k)]);
                    return wifi::crc16(dm.bits.data(), 32) == crc;
                };
                if (!headerOk(pd.at)) {
                    if (pd.alt == pd.at || !headerOk(pd.alt)) { tel.headersBad++; pending = false; busyUntil = 0; break; }
                    pd.at = pd.alt;
                }
                uint8_t h[4] = {};
                for (int i = 0; i < 32; i++) h[i / 8] |= (uint8_t)(dm.bits[(size_t)i] << (i % 8));
                tel.headersOk++;
                const int lenUs = h[2] | h[3] << 8;
                const bool r1 = h[0] == wifi::kSignal1M && !pd.shortPre, r2 = h[0] == wifi::kSignal2M;
                if (!r1 && !r2) { tel.otherRate++; pending = false; busyUntil = pd.at + (int64_t)lenUs * kSps; break; }
                pd.rate = r2 ? 2 : 1;
                const int usPerByte = r2 ? 4 : 8;
                const int bytes = lenUs / usPerByte;
                if (bytes < 14 || bytes > wifi::kMaxMpdu || lenUs % usPerByte) { tel.headersBad++; pending = false; busyUntil = 0; break; }
                pd.stage = 1; pd.bytes = bytes;
                pd.need = pd.at + (int64_t)(1 + symbolsFor(48 + 8 * bytes, pd.shortPre ? 0 : r2 ? 48 : kAllOne)) * kSps + kMargin;
                busyUntil = pd.need;
                continue;
            }
            Demod dm;
            demod(pd.at, 48 + 8 * pd.bytes, pd.shortPre ? 0 : pd.rate == 2 ? 48 : kAllOne, dm);
            pending = false;
            busyUntil = dm.end;
            if (pd.bytes < 14 || dm.bits.size() < (size_t)48 + 8 * (size_t)pd.bytes) { tel.blocksBad++; break; }
            std::vector<uint8_t> f((size_t)pd.bytes, 0);
            for (int i = 0; i < pd.bytes; i++)
                for (int k = 0; k < 8; k++) f[(size_t)i] |= (uint8_t)(dm.bits[(size_t)(48 + 8 * i + k)] << k);
            const size_t n = f.size() - 4;
            const uint32_t got = (uint32_t)f[n] | (uint32_t)f[n + 1] << 8 | (uint32_t)f[n + 2] << 16 | (uint32_t)f[n + 3] << 24;
            if (wifi::crc32(f.data(), n) != got) { tel.blocksBad++; emitMeta(f.data(), n, frameRssi(dm), false, dm); break; }
            const float rssi = frameRssi(dm);
            emitMeta(f.data(), n, rssi, true, dm);
            if (dm.nAng > 8) {
                const double m = dm.angSum / dm.nAng, var = std::max(1e-6, dm.angSq / dm.nAng - m * m);
                const double snr = 10 * std::log10(1 / var);
                snrAvg = snrSet ? 0.8 * snrAvg + 0.2 * snr : snr; snrSet = true;
                tel.cfoHz = m / (2 * kPi) * wifi::kSymRate;
            }
            tel.phy = 0;
            accept(f.data(), n, (int)f.size(), rssi, (double)pd.at / kOut);
        }
        // drop what no decode needs any more
        const int64_t keepFrom = (pending ? pd.at : end) - (int64_t)(kBack + 2) * kSps;
        if (keepFrom - base > (1 << 20)) {
            const size_t drop = (size_t)(keepFrom - base);
            cr.erase(cr.begin(), cr.begin() + (ptrdiff_t)drop);
            ci.erase(ci.begin(), ci.begin() + (ptrdiff_t)drop);
            pw.erase(pw.begin(), pw.begin() + (ptrdiff_t)drop);
            base += (int64_t)drop;
        }
    }

    // A frame whose FCS passed: count it, and for beacons, probe responses and FILS discovery frames update the access point's row.
    // tuned / bnd: the channel and band the frame was heard on (0 / -1: unknown, a file); the DSSS chain passes -1 and gets the radio's.
    WifiClientTracker clients{(uint32_t)std::random_device{}()};

    void accept(const uint8_t* p, size_t n, int len, float rssi, double sec, int tuned = -1, int bnd = -2) {
        tel.blocksOk++;
        lastGood = sigSec();
        if (onFrame) onFrame(p, n, rssi);
        if (tuned < 0) tuned = tunedChannel();
        if (bnd == -2) { const double c = centerHz.load(); bnd = c > 0 ? wifi::bandOfMhz((c + offset) / 1e6) : -1; }
        if (wire::on(wire::Wifi)) wire::Hub::get().publish(wire::Wifi, wire::radiotap80211(p, n, (bnd >= 0 && tuned > 0) ? (int)std::lround(wifi::bandChannelMhz(bnd, tuned)) : 0, rssi));   // as received: a protected payload stays encrypted
        wifi::Frame f;
        if (!wifi::parseFrame(p, n, f, bnd)) return;
        if (f.type >= 0 && f.type < 3) tel.perType[f.type]++;
        clients.observe(p, n, wifi::macText(f.bssid), (int64_t)time(nullptr), sec);
        tel.clients = clients.clients(); tel.floodAlarms = clients.alarms(); tel.deauthFrames = clients.deauths(); tel.handshakeFrames = clients.handshakes();
        if (!f.beacon && !f.probeResp && !f.fils) return;   // data and the other frames: counted only
        // A 40 / 80 / 160 MHz BSS sends its non-HT frame on every 20 MHz part (clause 17.3.8.2): the copies heard away from the primary channel are not new beacons
        if (bnd >= wifi::Band5 && tuned && f.channel && f.channel != tuned && wifi::validChannel(bnd, f.channel) && !f.fils) { tel.dupFrames++; return; }
        if (f.fils) tel.filsFrames++; else if (f.beacon) tel.beacons++; else tel.probeResps++;
        const int64_t wall = (int64_t)time(nullptr);
        WifiFrameLog l;
        l.wallTime = wall; l.sec = sec; l.channel = tuned; l.band = std::max(0, bnd); l.type = f.type; l.subtype = f.subtype; l.bssid = wifi::macText(f.bssid);
        l.rssi = rssi; l.len = len; l.info = f.summary;
        tel.log.insert(tel.log.begin(), std::move(l));
        if (tel.log.size() > kWifiLogMax) tel.log.pop_back();
        WifiAp& a = ap(f.bssid);
        if (f.fils) { a.fils = true; a.filsFrames++; if (!f.ssid.empty() && a.ssid.empty()) a.ssid = f.ssid; if (!tuned) tuned = 0; }
        else if (f.beacon) { a.hidden = f.hidden; if (!f.hidden) a.ssid = f.ssid; a.beacons++; }
        else { if (!f.hidden) a.ssid = f.ssid; a.probeResps++; if (f.unsolicited) a.unsolicited = true; }
        if (f.channel) a.channel = f.channel;
        else if (tuned && !a.channel && bnd != wifi::Band24) a.channel = tuned;
        if (tuned) a.heardOn = tuned;
        if (bnd >= 0) a.band = bnd;
        a.phy = tel.phy;
        if (!f.fils) {
            a.security = f.security; a.enterprise = f.enterprise; a.ciphers = f.ciphers;
            const std::string r = wifi::ratesText(f.rates);
            if (!r.empty()) a.rates = r;
            a.interval = f.interval; a.cap = f.cap;
            const wifi::Elems& e = f.el;
            a.widthMhz = e.widthMhz; a.standard = e.standard; a.caps = wifi::capsText(e); a.akms = e.akms; a.pmf = e.pmf;
            a.bssColor = e.bssColor; a.twt = e.heTwtReq || e.heTwtResp; a.eht = e.eht;
            if (!e.country.empty()) a.country = e.country;
            if (e.powerConstraint >= 0) a.powerConstraint = e.powerConstraint;
            if (e.mobility) a.mdid = e.mdid;
            if (e.he6) a.he6Reg = e.he6Reg;
            if (!e.rnr.empty()) {
                std::string t;
                for (const wifi::Neighbor& nb : e.rnr) {
                    char bf[120];
                    snprintf(bf, sizeof bf, "%s%s ch %d%s%s", t.empty() ? "" : "; ", wifi::bandName(nb.band >= 0 ? nb.band : 2), nb.channel, nb.hasBssid ? " " : "",
                             nb.hasBssid ? wifi::macText(nb.bssid).c_str() : "");
                    t += bf;
                    if (nb.coLocated) t += " (co-located)";
                    if (nb.unsolicitedProbe) t += " (unsolicited probe responses)";
                    if (nb.hasShortSsid) t += " short SSID " + wifi::fmtShortSsid(nb.shortSsid);
                }
                a.neighbors = t;
            }
        }
        a.rssi = rssi;
        a.rssiHist.push_back(rssi);
        if (a.rssiHist.size() > kWifiRssiHist) a.rssiHist.erase(a.rssiHist.begin());
        a.lastSec = sigSec(); a.lastWall = wall;
    }

    WifiAp& ap(const uint8_t b[6]) {
        const std::string key = wifi::macText(b);
        auto it = apIdx.find(key);
        if (it != apIdx.end()) return tel.aps[it->second];
        if (tel.aps.size() >= kWifiApMax) {
            size_t old = 0;
            for (size_t i = 1; i < tel.aps.size(); i++) if (tel.aps[i].lastSec < tel.aps[old].lastSec) old = i;
            tel.aps.erase(tel.aps.begin() + (ptrdiff_t)old);
            apIdx.clear();
            for (size_t i = 0; i < tel.aps.size(); i++) apIdx[tel.aps[i].bssid] = i;
        }
        WifiAp a;
        a.bssid = key;
        a.vendor = wifi::vendorOf(b);
        a.firstSec = sigSec();
        a.firstWall = (int64_t)time(nullptr);
        tel.aps.push_back(std::move(a));
        apIdx[key] = tel.aps.size() - 1;
        if (log && tel.aps.size() <= 30) log("Wi-Fi: new network " + key);
        return tel.aps.back();
    }

    void report() {
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.channel = tunedChannel();
        tel.band = band;
        tel.ofdmTriggers = tel.ofdmSignalOk = tel.ofdmSignalBad = tel.ofdmOther = tel.ofdmBadFcs = 0;
        for (const auto& sb : subs) {
            const auto& q = sb->rx.stats();
            tel.ofdmTriggers += q.triggers; tel.ofdmSignalOk += q.signalOk; tel.ofdmSignalBad += q.signalBad; tel.ofdmOther += q.otherRate; tel.ofdmBadFcs += q.framesBad;
        }
        tel.snrDb = (float)snrAvg;
        tel.state = sigSec() - lastGood < 5 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        WifiTelemetry out = tel;
        std::stable_sort(out.aps.begin(), out.aps.end(), [](const WifiAp& x, const WifiAp& y) { return x.lastSec > y.lastSec; });
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(out);
    }
};

WifiReceiver::WifiReceiver() : p_(std::make_unique<Impl>()) {}
WifiReceiver::~WifiReceiver() = default;

void WifiReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void WifiReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void WifiReceiver::setCenterHz(double hz) { if (hz != p_->centerHz.load()) { p_->centerHz = hz; p_->centerChanged = true; } }
bool WifiReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= wifiTuning().minSampleRate - 1; }
void WifiReceiver::reset() { p_->resetReq = true; }
void WifiReceiver::setReportInterval(double sec) { std::lock_guard<std::mutex> lk(p_->mu); p_->reportSec = std::max(0.005, sec); p_->resetReq = true; }
void WifiReceiver::clearNetworks() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);   // the report at once; the feed thread's lists (and counters) on its next block
        WifiTelemetry e;
        Impl::moveLists(p_->pub, e);
        p_->resLoad = false;
        p_->pub.seq++;
    }
    p_->clearReq = true;
}
void WifiReceiver::clearResults() { clearNetworks(); }

void WifiReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    if (m.centerChanged.exchange(false)) m.rebuildSubs();
    m.takeRequests();
    if (m.rate < wifiTuning().minSampleRate - 1 || n == 0) {
        if (m.rate > 0) { m.nIn += (int64_t)n; if ((double)m.nIn >= m.nextReport) { m.nextReport += m.reportSec * m.rate; m.report(); } }
        return;
    }
    // the OFDM receivers (5 / 6 GHz, and 2.4 GHz beacons sent at 6 Mbit/s and up): every 20 MHz channel through its own channel filter
    if (!m.subs.empty()) {
        const cf32* src = x;
        bool bad = false;
        for (size_t i = 0; i < n && !bad; i++) bad = !std::isfinite(x[i].real()) || !std::isfinite(x[i].imag());
        if (bad) { m.san.assign(x, x + n); for (cf32& v : m.san) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0); src = m.san.data(); }
        for (auto& sb : m.subs) {
            m.tmp.clear();
            sb->cf.process(src, n, m.tmp);
            sb->rx.feed(m.tmp.data(), m.tmp.size());
        }
    }
    if (!m.dsss) {   // 5 / 6 GHz: no 802.11b chain; the clock, level and reports still run
        for (size_t i = 0; i < n; i++) { const float re = x[i].real(), im = x[i].imag(); if (std::isfinite(re) && std::isfinite(im)) m.power += (double)re * re + (double)im * im; }
        m.nPower += (int64_t)n;
        m.nIn += (int64_t)n;
        while ((double)m.nIn >= m.nextReport) { m.nextReport += m.reportSec * m.rate; m.report(); }
        return;
    }
    for (size_t i = 0; i < n;) {
        // in blocks, cut at the reports
        const int64_t toReport = std::max<int64_t>(1, (int64_t)std::ceil(m.nextReport) - m.nIn);
        const int k = (int)std::min<int64_t>({(int64_t)(n - i), (int64_t)Impl::kBlock, toReport});
        m.block(x + i, k);
        i += (size_t)k;
        if ((double)m.nIn >= m.nextReport) { m.process(); m.nextReport += m.reportSec * m.rate; m.report(); }
    }
    m.nPower += (int64_t)n;
    m.process();
}

std::string WifiReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.aps, p_->pub.log);
}
bool WifiReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    WifiTelemetry l;
    if (!results::unpack(blob, t0, l.aps, l.log)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    WifiTelemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->resLoad = true;
    p_->pub.seq++;
    return true;
}
bool WifiReceiver::telemetry(WifiTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void WifiReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void WifiReceiver::setFrameCallback(std::function<void(const uint8_t*, size_t, float)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->onFrame = std::move(cb); }
void WifiReceiver::setFrameMetaCallback(std::function<void(const uint8_t*, size_t, const WifiFrameMeta&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->onMeta = std::move(cb); }
void WifiReceiver::setOfdmQpsk(bool on) { if (p_->ofdmQpsk.exchange(on) != on) p_->centerChanged = true; }

ModeTuning wifiTuning() {
    ModeTuning t;
    t.stdMode = 56; t.id = "wifi"; t.name = "Wi-Fi networks";
    t.minMhz = 2400; t.maxMhz = 7125; t.defMhz = 2437;   // channel 6; 2.4 GHz, 5 GHz (5150 - 5895 MHz) and 6 GHz (5925 - 7125 MHz)
    t.sampleRate = 20000000;                 // one 22 MHz channel at a time
    t.basebandHz = 0;
    t.bandwidthMhz = 20;
    t.minSampleRate = 20000000;
    t.tuneOffsetHz = 0;                      // the channel fills the band; the DC spike is one narrow notch in a spread signal
    return t;
}

} // namespace dect2

// Orbcomm receiver (orbcomm_rx.h).
#include "dect2/session/mode_adapters.h"
#include "dect2/orbcomm/orbcomm_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/orbcomm/orbcomm_gen.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

namespace dect2 {

using namespace orbcomm;

namespace {

using cd = std::complex<double>;

std::vector<float> lowpass(int n, double cutNorm) {   // windowed sinc (Blackman), cutoff as a fraction of the rate, unit gain at 0 Hz
    std::vector<float> h(n);
    const int m = (n - 1) / 2;
    double s = 0;
    for (int i = 0; i < n; i++) {
        const double k = i - m;
        const double sinc = k == 0 ? 2 * cutNorm : std::sin(2 * M_PI * cutNorm * k) / (M_PI * k);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) + 0.08 * std::cos(4 * M_PI * i / (n - 1));
        h[i] = (float)(sinc * w);
        s += h[i];
    }
    for (auto& v : h) v = (float)(v / s);
    return h;
}

double rrcAt(double t) {
    const double a = kRollOff;
    if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / M_PI;
    if (std::fabs(std::fabs(t) - 1 / (4 * a)) < 1e-9)
        return a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
    return (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - 16 * a * a * t * t));
}

struct Fir {   // decimating FIR on a doubled ring (symmetric taps)
    std::vector<float> h;
    std::vector<cf32> buf;
    int n = 1, pos = 0, d = 1, ph = 0;
    void init(const std::vector<float>& taps, int dec) { h = taps; n = (int)h.size(); buf.assign(2 * n, cf32(0, 0)); d = dec; pos = 0; ph = 0; }
    void clear() { std::fill(buf.begin(), buf.end(), cf32(0, 0)); pos = 0; ph = 0; }
    bool push(cf32 x, cf32& y) {
        buf[pos] = x; buf[pos + n] = x;
        pos = pos + 1 == n ? 0 : pos + 1;
        if (++ph < d) return false;
        ph = 0;
        const cf32* b = &buf[pos];
        float re = 0, im = 0;
        for (int i = 0; i < n; i++) { re += b[i].real() * h[i]; im += b[i].imag() * h[i]; }
        y = cf32(re, im);
        return true;
    }
};

constexpr int kHist = 256;      // samples kept at the decoder rate (timing interpolation and the eye)
constexpr int kAcqFft = 2048, kAcqBlocks = 4, kAcqN = kAcqFft * kAcqBlocks;   // 4th power: FFT size and blocks averaged
constexpr double kPubSec = 0.25;

} // namespace

struct OrbcommReceiver::Impl {
    struct Dec {
        int state = 0;                 // 0 free, 1 acquiring, 2 tracking, 3 frame lock
        double fc = 0;                 // first mixer, Hz against the centre
        cd rotA{1, 0}, stepA{1, 0};
        int rotN = 0;
        Fir fa, fb, rrc;
        double fd = 0, phB = 0;        // fine mixer (Doppler against fc)
        std::vector<cf32> acq;
        int attempts = 0;
        std::vector<double> tried;     // carriers tried without frames (the side lines of the 4th power)
        float pw = 1e-6f, symPw = 1;
        cf32 hist[kHist];
        int64_t nS = 0;
        double tNext = 0, spsAdj = 0, spsAvg = 0;   // spsAvg: the loop integrator smoothed for the clock read-out
        cf32 yPrev{0, 0};
        bool havePrev = false;
        double thAvg = 0, var = 1;
        // framing
        std::deque<uint8_t> win;
        bool locked = false;
        int pol = 0, pktIdx = 0, bitCount = 0, missed = 0;
        uint8_t bits[96];
        int sat = -1;
        double startTrack = 0, lastGood = 0;
        uint64_t frames = 0, pktOk = 0, pktBad = 0, syncLost = 0;
        std::deque<cf32> constel;
        std::deque<std::vector<float>> eye;
        int64_t symN = 0;
        float levelDb = -120;
    };

    std::atomic<double> rate{0}, center{0};
    std::atomic<int> maxCh{2};
    std::atomic<bool> resetReq{false};
    std::function<void(const std::string&)> logCb;

    // set up by configure
    double R = 0, fsA = 0, fs1 = 0, sps = 10;
    int dA = 1, dB = 1;
    std::vector<float> tapsA, tapsB, tapsR;
    int nd = 4096;
    bool ok = false;

    Dec dec[kOrbcommChannels];
    int64_t nIn = 0;
    // detection
    std::vector<cf32> dbuf;
    std::vector<float> psd, win;
    int dpos = 0, dBlocks = 0, dAcc = 0;
    std::unique_ptr<Fft> fftD, fftA;
    struct Black { double off, until; };
    std::vector<Black> black;

    // results
    std::map<int, OrbcommSat> sats;
    std::deque<OrbcommPacket> log;
    uint64_t typeCount[kTypeSlots] = {};
    uint64_t messages = 0, blocksOk = 0, blocksBad = 0;
    std::map<int, std::string> msgParts;   // sat << 8 | message ID
    double nextPub = 0;
    std::mutex mu;
    OrbcommTelemetry tel;
    uint64_t seq = 0;
    // the satellites and the packet log stay through resets (feed thread); Clear and a loaded file reach them through these requests
    // (mu), and the report at once
    bool clearReq = false, loadReq = false;
    std::map<int, OrbcommSat> loadSats;
    std::deque<OrbcommPacket> loadLog;
    uint64_t loadMessages = 0;

    void shiftTimes(double dt) {
        for (auto& [id, st] : sats) { st.lastHeard += dt; if (st.hasPos) st.posTime += dt; }
        for (auto& p : log) p.time += dt;
    }
    void takeRequests() {        // feed thread
        std::lock_guard<std::mutex> lk(mu);
        if (clearReq) { sats.clear(); log.clear(); msgParts.clear(); messages = 0; std::fill(std::begin(typeCount), std::end(typeCount), 0); clearReq = false; }
        if (loadReq) {
            sats = std::move(loadSats); log = std::move(loadLog); messages = loadMessages;
            for (auto& [id, st] : sats) st.chan = -1;
            const double dt = now() - tel.timeSec;   // the file's times are on the report's clock
            shiftTimes(dt);
            loadReq = false;
        }
    }

    double now() const { return R > 0 ? (double)nIn / R : 0; }

    void say(const std::string& s) { if (logCb) logCb(s); }

    void setup(double r) {
        R = r;
        ok = R >= 60e3;
        if (!ok) return;
        dA = std::max(1, (int)std::floor(R / 200e3));
        fsA = R / dA;
        dB = std::max(1, (int)std::floor(fsA / 48e3));
        fs1 = fsA / dB;
        sps = fs1 / kSymbolRate;
        const double pass = 14e3;
        if (dA > 1) {
            const double tr = std::max(20e3, fsA - 2 * pass);
            int n = (int)std::ceil(5.5 * R / tr) | 1;
            tapsA = lowpass(std::max(n, 9), 0.5 * fsA / R);
        } else tapsA.clear();
        {
            const double tr = std::max(8e3, fs1 - 2 * pass);
            int n = (int)std::ceil(5.5 * fsA / tr) | 1;
            tapsB = lowpass(std::max(n, 9), std::min(0.45, 0.5 * fs1 / fsA));
        }
        {
            const int m = (int)std::lround(4 * sps);
            tapsR.resize(2 * m + 1);
            for (int i = 0; i <= 2 * m; i++) tapsR[i] = (float)rrcAt((i - m) / sps);
        }
        nd = 1;
        while (nd < R / 60) nd <<= 1;
        fftD = std::make_unique<Fft>(nd);
        fftA = std::make_unique<Fft>(kAcqFft);
        win.resize(nd);
        for (int i = 0; i < nd; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / nd));
        clearAll();
    }

    void clearAll() {
        for (auto& d : dec) d = Dec();
        shiftTimes(-now());              // the satellites and the log stay (clearResults): the clock starts again
        nIn = 0;
        nextPub = 0;
        dbuf.assign(nd, cf32(0, 0));
        psd.assign(nd, 0.f);
        dpos = dBlocks = dAcc = 0;
        black.clear();
        sats.clear();
        log.clear();
        std::fill(std::begin(typeCount), std::end(typeCount), 0);
        messages = blocksOk = blocksBad = 0;
        msgParts.clear();
        nextPub = 0;
    }

    void startDec(Dec& d, double off) {
        d = Dec();
        d.state = 1;
        d.fc = off;
        d.stepA = std::polar(1.0, -2 * M_PI * off / R);
        if (dA > 1) d.fa.init(tapsA, dA);
        d.fb.init(tapsB, dB);
        d.rrc.init(tapsR, 1);
        d.acq.reserve(kAcqN);
        d.startTrack = now();
        d.lastGood = now();
    }

    void release(Dec& d, bool blacklist) {
        if (blacklist) black.push_back({d.fc, now() + 10});
        for (auto& [id, s] : sats) if (s.chan == (int)(&d - dec)) s.chan = -1;
        d.state = 0;
    }

    double absHz(const Dec& d) const { const double c = center.load(); return c + d.fc + d.fd; }
    double nominalHz(double a) const { return center.load() > 0 ? rasterHz(a) : std::round(a / kRasterHz) * kRasterHz; }

    // ---- detection
    void detect() {
        const int nmax = std::clamp(maxCh.load(), 1, kOrbcommChannels);
        int freeSlots = 0;
        for (int i = 0; i < nmax; i++) if (dec[i].state == 0) freeSlots++;
        if (!freeSlots) return;
        const double bin = R / nd;
        std::vector<float> p(nd);
        for (int i = 0; i < nd; i++) p[i] = psd[(i + nd / 2) % nd];   // fft shifted: index nd/2 is 0 Hz
        // the DC spike of the radio
        for (int i = nd / 2 - 3; i <= nd / 2 + 3; i++) p[i] = p[nd / 2 + 8];
        const int w = std::max(1, (int)std::lround(5e3 / bin));
        std::vector<double> cs(nd + 1, 0);
        for (int i = 0; i < nd; i++) cs[i + 1] = cs[i] + p[i];
        std::vector<float> sm(nd, 0.f);
        for (int i = 0; i < nd; i++) { const int a = std::max(0, i - w / 2), b = std::min(nd, i + w / 2 + 1); sm[i] = (float)((cs[b] - cs[a]) / (b - a)); }
        std::vector<float> tmp(sm);
        std::nth_element(tmp.begin(), tmp.begin() + nd / 2, tmp.end());
        const float floor = std::max(1e-30f, tmp[nd / 2]);
        const double t = now();
        black.erase(std::remove_if(black.begin(), black.end(), [&](const Black& b) { return b.until < t; }), black.end());
        struct Cand { double off; float lvl; };
        std::vector<Cand> cand;
        const double edge = R / 2 - 16e3;
        for (int i = 1; i < nd - 1; i++) {
            if (!(sm[i] >= sm[i - 1] && sm[i] > sm[i + 1])) continue;
            if (sm[i] < floor * 2.5f) continue;   // 4 dB
            // the -3 dB region and its centroid
            int a = i, b = i;
            while (a > 0 && sm[a - 1] > sm[i] * 0.5f) a--;
            while (b < nd - 1 && sm[b + 1] > sm[i] * 0.5f) b++;
            if ((b - a + 1) * bin > 15e3) continue;
            double s = 0, sw = 0;
            for (int k = a; k <= b; k++) { s += (sm[k] - floor) * (k - nd / 2) * bin; sw += sm[k] - floor; }
            const double off = sw > 0 ? s / sw : (i - nd / 2) * bin;
            if (std::fabs(off) > edge) continue;
            const double c = center.load();
            if (c > 0 && (c + off < 137.15e6 || c + off > 137.85e6)) continue;
            bool skip = false;
            for (const auto& d : dec) if (d.state != 0 && std::fabs(d.fc + d.fd - off) < 9e3) skip = true;
            for (const auto& bl : black) if (std::fabs(bl.off - off) < 3e3) skip = true;
            for (const auto& c2 : cand) if (std::fabs(c2.off - off) < 9e3) skip = true;
            if (!skip) cand.push_back({off, sm[i]});
        }
        std::sort(cand.begin(), cand.end(), [](const Cand& x, const Cand& y) { return x.lvl > y.lvl; });
        size_t k = 0;
        for (int i = 0; i < nmax && k < cand.size(); i++)
            if (dec[i].state == 0) startDec(dec[i], cand[k++].off);
    }

    void detectFeed(cf32 x) {
        dbuf[dpos++] = x;
        if (dpos < nd) return;
        dpos = 0;
        if (dBlocks++ % 4) return;
        bool need = false;
        const int nmax = std::clamp(maxCh.load(), 1, kOrbcommChannels);
        for (int i = 0; i < nmax; i++) need |= dec[i].state == 0;
        if (!need) { dAcc = 0; std::fill(psd.begin(), psd.end(), 0.f); return; }
        std::vector<cf32> v(nd);
        for (int i = 0; i < nd; i++) v[i] = dbuf[i] * win[i];
        fftD->forward(v.data());
        for (int i = 0; i < nd; i++) psd[i] += std::norm(v[i]);
        if (++dAcc >= 8) { detect(); dAcc = 0; std::fill(psd.begin(), psd.end(), 0.f); }
    }

    // ---- per decoder
    // The carrier from the square. SDPSK is BPSK turned a quarter turn more every symbol: a BPSK at +1200 Hz (and equally at -1200 Hz),
    // so the square holds two lines, at 2 x offset +/- 2400 Hz, of the same strength. |FFT|^2 of x^2 averaged over kAcqBlocks blocks
    // (a Doppler ramp smears one long block); the pair 4800 Hz apart with the most power gives the carrier. A decoder that found no
    // frames on a carrier tries the next pair.
    void acquire(Dec& d) {
        std::vector<double> P(kAcqFft, 0.0);
        std::vector<cf32> v(kAcqFft);
        for (int b = 0; b < kAcqBlocks; b++) {
            for (int i = 0; i < kAcqFft; i++) { const cf32 a = d.acq[b * kAcqFft + i]; v[i] = a * a; }
            fftA->forward(v.data());
            for (int i = 0; i < kAcqFft; i++) P[i] += std::norm(v[i]);
        }
        d.acq.clear();
        double sum = 0;
        for (double p : P) sum += p;
        const double mean = sum / kAcqFft, bin = fs1 / kAcqFft;
        auto at = [&](int i) { return P[((i % kAcqFft) + kAcqFft) % kAcqFft]; };
        auto peak = [&](int i, int& where) { where = i; for (int k = i - 1; k <= i + 1; k++) if (at(k) > at(where)) where = k; return at(where); };
        auto fine = [&](int i) {   // parabolic, in Hz of the square, signed
            const double pm = at(i - 1), p0 = at(i), pp = at(i + 1), den = pm - 2 * p0 + pp;
            double k = i + (den != 0 ? std::clamp(0.5 * (pm - pp) / den, -0.5, 0.5) : 0);
            k = std::fmod(k + kAcqFft, (double)kAcqFft);
            if (k >= kAcqFft / 2) k -= kAcqFft;
            return k * bin;
        };
        const int sep = (int)std::lround(2 * kSymbolRate / 2 / bin);   // 4800 Hz in the square
        double bestScore = 0, bestFd = 0, bestLine = 0;
        for (int i = 0; i < kAcqFft; i++) {
            int wa, wb;
            const double pa = peak(i, wa), pb = peak(i + sep, wb);
            if (wa != i) continue;   // each pair once, from its lower line's peak
            const double score = std::min(pa, pb);
            if (score <= bestScore) continue;
            const double fa = fine(wa), fb = fine(wb);
            double mid = fa + std::remainder(fb - fa, fs1) / 2;   // the centre of the pair, in the square
            const double fdc = std::remainder(mid, fs1) / 2;
            bool tried = false;
            for (double t : d.tried) if (std::fabs(t - fdc) < 300) tried = true;
            if (tried) continue;
            bestScore = score; bestFd = fdc; bestLine = std::max(pa, pb);
        }
        if (bestScore < 5 * mean) {
            if (++d.attempts >= 4) release(d, true);
            return;
        }
        (void)bestLine;
        d.fd = bestFd;
        d.tried.push_back(d.fd);
        d.phB = 0;
        d.state = 2;
        d.startTrack = now();
        d.tNext = (double)d.nS + sps;
        d.havePrev = false;
        d.win.clear(); d.locked = false;
    }

    void frameBit(Dec& d, int bit) {
        const int ci = (int)(&d - dec);
        if (!d.locked) {
            d.win.push_back((uint8_t)bit);
            if (d.win.size() > 96) d.win.pop_front();
            if (d.win.size() < 96) return;
            for (int pol = 0; pol < 2; pol++) {
                uint8_t p[kPktBytes] = {};
                for (int i = 0; i < 96; i++) p[i / 8] |= (uint8_t)((d.win[i] ^ pol) << (i % 8));
                if (p[0] == kSync[0] && p[1] == kSync[1] && p[2] == kSync[2] && fletcherOk(p)) {
                    d.locked = true; d.pol = pol; d.pktIdx = 1; d.bitCount = 0; d.missed = 0;
                    d.state = 3;
                    d.win.clear();
                    packet(d, ci, p);
                    return;
                }
            }
            return;
        }
        d.bits[d.bitCount++] = (uint8_t)(bit ^ d.pol);
        if (d.bitCount < 96) return;
        d.bitCount = 0;
        uint8_t p[kPktBytes] = {};
        for (int i = 0; i < 96; i++) p[i / 8] |= (uint8_t)(d.bits[i] << (i % 8));
        const int idx = d.pktIdx;
        d.pktIdx = (d.pktIdx + 1) % kFramePkts;
        if (idx == 0) {
            const bool sync = p[0] == kSync[0] && p[1] == kSync[1] && p[2] == kSync[2] && fletcherOk(p);
            if (!sync) {
                if (++d.missed >= 2) { d.locked = false; d.state = 2; d.startTrack = now(); d.syncLost++; d.win.clear(); return; }
            } else d.missed = 0;
        }
        packet(d, ci, p);
    }

    OrbcommSat& satEntry(int id) {
        OrbcommSat& s = sats[id];
        s.id = id;
        return s;
    }

    void packet(Dec& d, int ci, const uint8_t* p) {
        if (!fletcherOk(p)) { d.pktBad++; blocksBad++; if (d.sat >= 0) satEntry(d.sat).bad++; return; }
        d.pktOk++; blocksOk++;
        d.lastGood = now();
        const int type = typeOf(p[0]);
        typeCount[type]++;
        if (type == TSync) {
            if (d.sat != p[3]) {
                d.sat = p[3];
                char b[96]; snprintf(b, sizeof b, "Orbcomm: satellite %d on %.4f MHz (Doppler %+.0f Hz)", d.sat, absHz(d) / 1e6, absHz(d) - nominalHz(absHz(d)));
                say(b);
            }
            d.frames++;
            satEntry(d.sat).frames++;
        }
        if (d.sat >= 0) {
            OrbcommSat& s = satEntry(d.sat);
            s.packets++;
            s.lastHeard = now();
            s.chan = ci;
            s.freqHz = absHz(d);
            s.nominalHz = nominalHz(s.freqHz);
            s.dopplerHz = s.freqHz - s.nominalHz;
            s.snrDb = (float)(10 * std::log10(1 / std::max(1e-4, d.var)));
            if (type == TEphemeris) {
                double x, y, z; int sec;
                unpackEphemeris(p, x, y, z, sec);
                ecefToGeo(x, y, z, s.lat, s.lon, s.altKm);
                s.hasPos = true; s.posTime = now();
                if (s.track.size() < 2 || std::fabs(s.track[s.track.size() - 2] - s.lat) + std::fabs(s.track.back() - s.lon) > 0.3) {
                    s.track.push_back((float)s.lat); s.track.push_back((float)s.lon);
                    if (s.track.size() > 600) s.track.erase(s.track.begin(), s.track.begin() + 2);
                }
            } else if (type == TElements) {
                s.hasElements = true;
                s.inclDeg = u16(p + 2) / 100.f; s.raanDeg = u16(p + 4) / 100.f; s.argLatDeg = u16(p + 6) / 100.f; s.periodMin = u16(p + 8) / 100.f;
            } else if (type == TDownlink || type == TUplink) {
                std::vector<double>& v = type == TDownlink ? s.downlinksMhz : s.uplinksMhz;
                v.clear();
                for (int i = 0; i < 4 && i < p[1]; i++) v.push_back((type == TDownlink ? 137.0 : 148.0) + u16(p + 2 + 2 * i) * 0.0025);
            }
        }
        if (type == TSync || type == TFill) return;
        OrbcommPacket pk;
        pk.time = now(); pk.sat = d.sat; pk.chan = ci; pk.type = type;
        memcpy(pk.bytes, p, kPktBytes);
        pk.text = describe(p);
        if (type == TMessage) {
            const int key = (d.sat & 0xFFFF) << 8 | p[1];
            std::string& m = msgParts[key];
            if ((p[2] & 0x7F) == 0) m.clear();
            for (int i = 0; i < 7; i++) { const uint8_t c = p[3 + i]; m += c >= 32 && c < 127 ? (char)c : '.'; }
            if (p[2] & 0x80) {
                while (!m.empty() && m.back() == ' ') m.pop_back();
                OrbcommPacket full = pk;
                char b[48]; snprintf(b, sizeof b, "message %d complete: ", p[1]);
                full.text = b + m;
                pushLog(pk);
                pushLog(full);
                messages++;
                msgParts.erase(key);
                if (msgParts.size() > 64) msgParts.clear();
                return;
            }
        }
        pushLog(pk);
    }

    void pushLog(const OrbcommPacket& pk) {
        log.push_back(pk);
        while (log.size() > kOrbcommLogKeep) log.pop_front();
    }

    cf32 interp(const Dec& d, double t) const {
        const int64_t i = (int64_t)std::floor(t);
        const float fr = (float)(t - i);
        const cf32 a = d.hist[((i % kHist) + kHist) % kHist], b = d.hist[(((i + 1) % kHist) + kHist) % kHist];
        return a + (b - a) * fr;
    }

    void symbolOut(Dec& d, cf32 y, cf32 m, double tStrobe) {
        d.symPw = 0.99f * d.symPw + 0.01f * std::norm(y);
        if (!d.havePrev) { d.yPrev = y; d.havePrev = true; return; }
        float e = ((y - d.yPrev) * std::conj(m)).real() / std::max(1e-9f, d.symPw);
        e = std::clamp(e, -1.f, 1.f);
        const double kp = 0.02 * sps, ki = 2e-4 * sps;
        d.spsAdj = std::clamp(d.spsAdj - ki * e, -0.01 * sps, 0.01 * sps);
        d.tNext += sps + d.spsAdj - kp * e;
        d.spsAvg += 0.002 * (d.spsAdj - d.spsAvg);   // the raw integrator wanders with the noise by tens of ppm
        const cf32 z = y * std::conj(d.yPrev);
        d.yPrev = y;
        const float az = std::abs(z);
        if (az < 1e-12f) return;
        const cf32 zu = z / az;
        const double th = 0.5 * std::arg(-(zu * zu));
        const double kf = d.locked ? 0.008 : 0.02;
        d.fd += kf * th * kSymbolRate / (2 * M_PI);
        d.thAvg = 0.95 * d.thAvg + 0.05 * th;
        d.var = 0.995 * d.var + 0.005 * th * th;
        const cf32 zr = zu * std::polar(1.f, (float)-d.thAvg);
        const int bit = zr.imag() > 0 ? 1 : 0;
        d.constel.push_back(zr);
        if (d.constel.size() > 600) d.constel.pop_front();
        if ((d.symN++ & 3) == 0) {   // an eye trace: the phase step over one symbol, across the last two symbols
            const int L = (int)std::floor(2 * sps), lag = (int)std::lround(sps);
            const int64_t end = (int64_t)std::floor(tStrobe);   // this symbol's strobe
            if (end - L - lag > d.nS - kHist) {
                std::vector<float> tr(L);
                for (int i = 0; i < L; i++) {
                    const int64_t q = end - L + 1 + i;
                    const cf32 a = d.hist[((q % kHist) + kHist) % kHist], b = d.hist[(((q - lag) % kHist) + kHist) % kHist];
                    tr[i] = (float)(std::arg(a * std::conj(b) * std::polar(1.f, (float)-d.thAvg)) / (M_PI / 2));
                }
                d.eye.push_back(std::move(tr));
                if (d.eye.size() > 48) d.eye.pop_front();
            }
        }
        frameBit(d, bit);
    }

    void decSample(Dec& d, cf32 s) {
        d.levelDb = 0.999f * d.levelDb + 0.001f * (float)(10 * std::log10(std::norm(s) + 1e-20));
        if (d.state == 1) {
            d.acq.push_back(s);
            if ((int)d.acq.size() >= kAcqN) acquire(d);
            return;
        }
        // fine mixer
        d.phB -= 2 * M_PI * d.fd / fs1;
        if (d.phB > M_PI || d.phB < -M_PI) d.phB = std::remainder(d.phB, 2 * M_PI);
        s *= std::polar(1.f, (float)d.phB);
        d.pw = 0.995f * d.pw + 0.005f * std::norm(s);
        s /= std::sqrt(std::max(1e-20f, d.pw));
        cf32 r;
        d.rrc.push(s, r);
        d.hist[d.nS % kHist] = r;
        d.nS++;
        while (d.tNext <= (double)d.nS - 2 && d.state >= 2) {
            const double t = d.tNext;
            const cf32 y = interp(d, t), m = interp(d, t - sps / 2);
            symbolOut(d, y, m, t);
            if (d.tNext <= t) d.tNext = t + sps;   // never stand still
        }
        // keep or let go
        const double tn = now();
        if (d.state == 2 && tn - d.startTrack > 3.5) {
            if (++d.attempts >= 3) release(d, true);
            else { d.state = 1; d.acq.clear(); }
        } else if (d.state == 3 && tn - d.lastGood > 5) {
            d.locked = false; d.state = 2; d.startTrack = tn; d.syncLost++;
        }
        if (d.state >= 2 && std::fabs(d.fd) > 11e3) release(d, false);
    }

    void feedOne(cf32 x) {
        detectFeed(x);
        for (auto& d : dec) {
            if (d.state == 0) continue;
            const cd v = cd(x.real(), x.imag()) * d.rotA;
            d.rotA *= d.stepA;
            if (++d.rotN >= 4096) { d.rotA /= std::abs(d.rotA); d.rotN = 0; }
            cf32 a((float)v.real(), (float)v.imag()), b;
            if (dA > 1 && !d.fa.push(a, a)) continue;
            if (!d.fb.push(a, b)) continue;
            decSample(d, b);
        }
        nIn++;
    }

    void dedupe() {
        for (int i = 0; i < kOrbcommChannels; i++)
            for (int j = i + 1; j < kOrbcommChannels; j++) {
                Dec &a = dec[i], &b = dec[j];
                if (a.state < 2 || b.state < 2) continue;
                if (std::fabs(a.fc + a.fd - b.fc - b.fd) < 3e3) release(a.pktOk >= b.pktOk ? b : a, false);
            }
        const int nmax = std::clamp(maxCh.load(), 1, kOrbcommChannels);
        for (int i = nmax; i < kOrbcommChannels; i++) if (dec[i].state) release(dec[i], false);
    }

    void publish() {
        OrbcommTelemetry t;
        t.timeSec = now(); t.centerHz = center.load(); t.inputRate = R; t.maxChannels = maxCh.load();
        int st = 0;
        for (int i = 0; i < kOrbcommChannels; i++) {
            const Dec& d = dec[i];
            OrbcommChannel& c = t.ch[i];
            c.active = d.state != 0;
            c.state = d.state;
            if (!c.active) continue;
            c.offsetHz = d.fc + d.fd;
            c.freqHz = absHz(d);
            c.dopplerHz = c.freqHz - nominalHz(c.freqHz);
            c.snrDb = d.state >= 2 ? (float)(10 * std::log10(1 / std::max(1e-4, d.var))) : 0.f;
            c.levelDb = d.levelDb;
            c.sat = d.sat;
            c.clockPpm = d.spsAvg / sps * -1e6;
            c.frames = d.frames; c.pktOk = d.pktOk; c.pktBad = d.pktBad; c.syncLost = d.syncLost;
            c.constel.assign(d.constel.begin(), d.constel.end());
            c.eyeLen = d.eye.empty() ? 0 : (int)d.eye.front().size();
            for (const auto& tr : d.eye) if ((int)tr.size() == c.eyeLen) c.eye.insert(c.eye.end(), tr.begin(), tr.end());
            st = std::max(st, d.state == 3 ? 2 : 1);
            t.snrDb = std::max(t.snrDb, c.snrDb);
        }
        t.state = st;
        t.dataValid = st == 2;
        t.cfoHz = t.ch[0].active ? t.ch[0].dopplerHz : t.ch[1].dopplerHz;
        t.blocksOk = blocksOk; t.blocksBad = blocksBad;
        for (auto& [id, s] : sats) {
            if (s.chan >= 0 && (dec[s.chan].state == 0 || dec[s.chan].sat != id)) s.chan = -1;
            t.sats.push_back(s);
        }
        t.log.assign(log.begin(), log.end());
        std::copy(std::begin(typeCount), std::end(typeCount), t.typeCount);
        t.messages = messages;
        if (!st) t.note = "searching 137.2 - 137.8 MHz for downlinks";
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++seq;
        tel = std::move(t);
    }
};

OrbcommReceiver::OrbcommReceiver() : p_(std::make_unique<Impl>()) {}
OrbcommReceiver::~OrbcommReceiver() = default;

void OrbcommReceiver::configure(double r) { p_->rate = r; p_->setup(r); }
bool OrbcommReceiver::ready() const { return p_->ok; }
void OrbcommReceiver::reset() { p_->resetReq = true; }
void OrbcommReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }
void OrbcommReceiver::setCenterHz(double hz) { p_->center = hz; }
void OrbcommReceiver::setMaxChannels(int n) { p_->maxCh = std::clamp(n, 1, kOrbcommChannels); }

void OrbcommReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    if (!s.ok) return;
    if (s.resetReq.exchange(false)) s.clearAll();
    s.takeRequests();
    for (size_t i = 0; i < n; i++) s.feedOne(x[i]);
    s.dedupe();
    if (s.now() >= s.nextPub) { s.nextPub = s.now() + kPubSec; s.publish(); }
}

void OrbcommReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->clearReq = true; p_->loadReq = false;
    p_->tel.sats.clear(); p_->tel.log.clear(); p_->tel.messages = 0;
    p_->tel.seq = ++p_->seq;
}
std::string OrbcommReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->tel.timeSec, p_->tel.sats, p_->tel.log, p_->tel.messages);
}
bool OrbcommReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::vector<OrbcommSat> sats;
    std::vector<OrbcommPacket> log;
    uint64_t messages = 0;
    if (!results::unpack(blob, t0, sats, log, messages)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    const double dt = p_->tel.timeSec - t0;
    for (auto& st : sats) { st.lastHeard += dt; if (st.hasPos) st.posTime += dt; st.chan = -1; }
    for (auto& pk : log) pk.time += dt;
    p_->loadSats.clear();
    for (const auto& st : sats) p_->loadSats[st.id] = st;
    p_->loadLog.assign(log.begin(), log.end());
    p_->loadMessages = messages;
    p_->tel.sats = std::move(sats); p_->tel.log = std::move(log); p_->tel.messages = messages;
    p_->loadReq = true;
    p_->tel.seq = ++p_->seq;
    return true;
}

bool OrbcommReceiver::telemetry(OrbcommTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq == 0 || p_->tel.seq == lastSeq) return false;
    out = p_->tel;
    sess::observeOrbcomm(out);
    return true;
}

ModeTuning orbcommTuning() {
    ModeTuning t;
    t.stdMode = 46; t.id = "orbcomm"; t.name = "Orbcomm";
    t.minMhz = 136; t.maxMhz = 139; t.defMhz = 137.25;
    t.sampleRate = 1e6; t.basebandHz = 0; t.bandwidthMhz = 0.6;
    t.minSampleRate = 100e3;
    t.tuneOffsetHz = 25e3;
    return t;
}

const OrbcommPreset* orbcommPresets(int& count) {
    static const OrbcommPreset p[] = {{"137.2500", 137.25}, {"137.2875", 137.2875}, {"137.3125", 137.3125}, {"137.4625", 137.4625},
                                      {"137.6625", 137.6625}, {"137.7375", 137.7375}, {"137.5 (band)", 137.5}};
    count = (int)(sizeof p / sizeof p[0]);
    return p;
}

namespace {
class OrbcommSynth : public ModeSynth {
public:
    explicit OrbcommSynth(const orbcomm::SignalConfig& c) : s_(c) {}
    double sampleRate() const override { return s_.config().sampleRate; }
    void generate(cf32* out, size_t n) override { s_.generate(out, n); }
private:
    orbcomm::Signal s_;
};
} // namespace

std::unique_ptr<ModeSynth> makeOrbcommSynth(const SynthConfig& cfg, double sampleRate) {
    orbcomm::SignalConfig c;
    c.sampleRate = sampleRate > 0 ? sampleRate : 1e6;
    c.channels = cfg.modeOpt[0] == 1 ? 1 : 2;
    const bool ramp = cfg.modeOpt[1] == 1;
    const double leg = cfg.modeVal[0] > 0 ? cfg.modeVal[0] : 30;
    c.ch[0].offsetHz = -25e3; c.ch[0].satId = 23; c.ch[0].raanDeg = 20; c.ch[0].phaseDeg = 40;
    c.ch[1].offsetHz = 25e3; c.ch[1].satId = 41; c.ch[1].raanDeg = 80; c.ch[1].phaseDeg = 200;
    c.ch[0].dopplerHz = cfg.cfoHz + (ramp ? -4000 : 1200);
    c.ch[0].dopplerEndHz = cfg.cfoHz + 4000;
    c.ch[1].dopplerHz = cfg.cfoHz + (ramp ? 3000 : -2100);
    c.ch[1].dopplerEndHz = cfg.cfoHz - 3000;
    c.ch[0].rampSec = c.ch[1].rampSec = ramp ? leg : 0;
    c.badFraction = std::clamp(cfg.modeOpt[2], 0, 50) / 100.0;
    c.seed = (uint32_t)std::max(1, cfg.modeOpt[3]);
    c.snrDb = cfg.snrDb;
    c.clockPpm = cfg.modeVal[1] + cfg.sroPpm;
    return std::make_unique<OrbcommSynth>(c);
}

} // namespace dect2

// PACTOR-I monitor receiver (see pactor_rx.h).
#include "dect2/pactor/pactor_rx.h"
#include "dect2/pactor/pactor_proto.h"
#include "dect2/pactor/pactor_tel.h"
#include "dect2/marine/marine_dsp.h"
#include "../audio/ham_listen.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
using cd = std::complex<double>;
constexpr double kFs = 8000;
constexpr int kRingN = 1 << 16, kMask = kRingN - 1;
constexpr int kFftN = 4096, kFftHop = 2048;
constexpr int kHop = 8;                       // envelope every millisecond
constexpr int kBitSlow = 80, kBitFast = 40;   // samples per bit at 100 and 200 baud
constexpr int kPktSamples = 7680;             // 0.96 s
constexpr int kCsSamples = 960;               // 0.12 s
constexpr int kSearch = 30;                   // timing search, +- ms

double nowUtc() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

void fft(std::vector<cd>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const cd wl = std::polar(1.0, -2 * kPi / (double)len);
        for (size_t i = 0; i < n; i += len) {
            cd w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

bool looksLikeCall(const std::string& t) {
    if (t.size() < 4 || t.size() > 8) return false;
    size_t i = 0;
    int pre = 0;
    while (i < t.size() && ((t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= '0' && t[i] <= '9')) && pre < 3) { if (t[i] >= '0' && t[i] <= '9' && pre > 0) break; i++; pre++; }
    if (i == 0 || i >= t.size() || !(t[i] >= '0' && t[i] <= '9')) return false;
    i++;
    size_t suf = 0;
    while (i < t.size() && t[i] >= 'A' && t[i] <= 'Z') { i++; suf++; }
    return i == t.size() && suf >= 1 && suf <= 3;
}
} // namespace

struct PactorReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    PactorTelemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false};
    // feed thread
    double curRate = 0;
    bool running = false;
    marine::Front front;
    hfdig::UsbAudio usb;
    std::atomic<int> demodSet{HfDemodUsb};
    hfdig::Listen listen;
    std::vector<cf32> base;
    std::vector<float> aud;
    std::vector<float> ring;
    int64_t na = 0;
    double secPow = 0;
    int secN = 0;
    double t0 = 0;
    // tone pair
    std::vector<double> win;
    bool locked = false;
    double lockLow = 0, candLow = 0;
    int candCnt = 0;
    int64_t lastActivity = 0;
    float lastSnr = 0;
    // envelope
    cd oh{1, 0}, ol{1, 0}, wh{1, 0}, wl{1, 0};
    cd zh{0, 0}, zl{0, 0}, histH[4], histL[4];
    int histN = 0;
    double floorE = 1;
    std::vector<double>* collect = nullptr;   // first pass over the stored audio: the energies, to start the noise level from
    bool on = false;
    int64_t burstStart = 0, lastOn = 0;
    int offCnt = 0;
    // results
    std::vector<PactorFrameLog> frames;
    std::vector<PactorStation> stations;
    std::string text;
    std::vector<float> levelHist;
    uint64_t okPk = 0, badPk = 0, csCount = 0, memSaves = 0, repeats = 0, fastPk = 0, slowPk = 0, changeovers = 0, connects = 0;
    bool fecMode = false;
    int sender = 1;
    int lastTextSender = 0;
    double lastFrameSec = -1e9;
    bool changed = true;
    int64_t nIn = 0;
    double nextReport = 0;
    // packet bookkeeping
    bool haveLast = false;
    std::vector<uint8_t> lastKey;
    double lastPktTime = -1e9;
    bool csSinceLast = true;
    int noCsRun = 0;
    // memory ARQ
    std::vector<float> badSoft;
    double badTime = -1e9;
    // region prefix sums
    int64_t r0 = 0;
    std::vector<cd> PH, PL;

    Impl() {
        ring.assign(kRingN, 0.f);
        win.resize(kFftN);
        for (int i = 0; i < kFftN; i++) win[(size_t)i] = 0.5 - 0.5 * std::cos(2 * kPi * i / kFftN);
    }

    void say(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }
    double timeOf(int64_t sample) const { return (double)sample / kFs; }

    void resetState() {
        std::function<void(const std::string&)> cb;
        double off;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; }
        running = curRate >= pactorTuning().minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        usb.reset(); listen.reset();
        std::fill(ring.begin(), ring.end(), 0.f);
        na = 0; secPow = 0; secN = 0;
        locked = false; candCnt = 0; on = false; offCnt = 0; histN = 0;
        nIn = 0; nextReport = 0;
        t0 = nowUtc();
        lastFrameSec = -1e9;
        badSoft.clear(); haveLast = false; csSinceLast = true; noCsRun = 0;
        { std::lock_guard<std::mutex> lk(mu); pub.inputRate = curRate; }
        changed = true;
        if (cb && curRate > 0) cb(running ? "PACTOR-I: 100 / 200 baud FSK, 200 Hz shift, on the upper sideband (experimental, monitor only)"
                                          : "PACTOR-I: the sample rate is too low (250 kHz at least)");
    }

    // ------------------------------------------------------------ audio
    void process(const float* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            ring[(size_t)(na & kMask)] = x[i];
            na++;
            secPow += (double)x[i] * x[i]; secN++;
            if (secN >= 8000) {
                levelHist.push_back((float)(10 * std::log10(secPow / secN + 1e-12)));
                if (levelHist.size() > kPactorHist) levelHist.erase(levelHist.begin());
                secPow = 0; secN = 0; changed = true;
            }
            if (locked) envSample(x[i], na - 1);
            if (na >= kFftN && (na % kFftHop) == 0) pairSearch();
        }
    }

    // ------------------------------------------------------------ the tone pair
    void pairSearch() {
        std::vector<cd> a((size_t)kFftN);
        for (int i = 0; i < kFftN; i++) a[(size_t)i] = cd(ring[(size_t)((na - kFftN + i) & kMask)] * win[(size_t)i], 0);
        fft(a);
        const int nb = kFftN / 2;
        std::vector<double> P((size_t)nb);
        for (int k = 0; k < nb; k++) P[(size_t)k] = std::norm(a[(size_t)k]);
        std::vector<double> tmp(P.begin() + 154, P.begin() + 1690);
        std::nth_element(tmp.begin(), tmp.begin() + (long)tmp.size() / 2, tmp.end());
        const double med = tmp[tmp.size() / 2] + 1e-18;
        const double bin = kFs / kFftN;
        const int dk = (int)std::lround(200.0 / bin);
        double best = 0;
        int bk = -1, bk2 = -1;
        for (int k = 77; k < 1843; k++) {
            if (P[(size_t)k] < P[(size_t)k - 1] || P[(size_t)k] < P[(size_t)k + 1]) continue;
            if (P[(size_t)k] < 40 * med) continue;
            int k2 = -1;
            double p2 = 0;
            for (int j = k + dk - 3; j <= k + dk + 3 && j < nb - 1; j++)
                if (P[(size_t)j] >= P[(size_t)j - 1] && P[(size_t)j] >= P[(size_t)j + 1] && P[(size_t)j] > p2) { p2 = P[(size_t)j]; k2 = j; }
            if (k2 < 0 || p2 < 40 * med) continue;
            const double sc = std::min(P[(size_t)k], p2);
            if (sc > best) { best = sc; bk = k; bk2 = k2; }
        }
        if (bk < 0) {
            candCnt = 0;
            if (locked && na - lastActivity > 8 * (int64_t)kFs && !on) { locked = false; changed = true; }
            return;
        }
        auto refine = [&](int k) {
            const double l0 = std::log(P[(size_t)k - 1] + 1e-18), l1 = std::log(P[(size_t)k] + 1e-18), l2 = std::log(P[(size_t)k + 1] + 1e-18);
            const double den = l0 - 2 * l1 + l2;
            const double d = den < -1e-9 ? 0.5 * (l0 - l2) / den : 0;
            return (k + std::max(-0.5, std::min(0.5, d))) * bin;
        };
        const double f1 = refine(bk), f2 = refine(bk2);
        const double low = 0.5 * (f1 + f2) - 100.0;
        if (candCnt > 0 && std::fabs(low - candLow) < 6) { candCnt++; candLow = 0.5 * (candLow + low); }
        else { candCnt = 1; candLow = low; }
        lastActivity = std::max(lastActivity, na);
        // the noise level of the envelope: the median bin is the noise power per bin; one hop of 32 samples sums to 32 sigma^2 per tone
        const double sig2 = med / 0.69 / (kFftN * 0.375);
        if (candCnt >= 2 && (!locked || (std::fabs(candLow - lockLow) > 8 && candCnt >= 3 && !on))) lockPair(candLow, 64.0 * sig2);
        const double sn = best / med;
        lastSnr = (float)std::min(40.0, 10 * std::log10(std::max(1.0, sn)));
    }

    void lockPair(double low, double noiseE) {
        locked = true; lockLow = low; floorE = std::max(noiseE, 1e-12);
        wl = std::polar(1.0, -2 * kPi * low / kFs);
        wh = std::polar(1.0, -2 * kPi * (low + 200.0) / kFs);
        const int64_t start = std::max<int64_t>(0, ((na - 4 * (int64_t)kFs) / kHop) * kHop);
        oh = std::polar(1.0, -2 * kPi * (low + 200.0) * (double)start / kFs);
        ol = std::polar(1.0, -2 * kPi * low * (double)start / kFs);
        zh = zl = cd(0, 0); histN = 0; on = false; offCnt = 0;
        changed = true;
        say("PACTOR-I: tone pair " + std::to_string((int)std::lround(low)) + " / " + std::to_string((int)std::lround(low + 200)) + " Hz");
        {   // first pass: the noise level is the lower quartile of the energies (mean of two exponentials = quartile / 0.38), whatever the signal does
            std::vector<double> es;
            collect = &es;
            for (int64_t i = start; i < na; i++) envSample(ring[(size_t)(i & kMask)], i);
            collect = nullptr;
            if (es.size() > 50) {
                std::nth_element(es.begin(), es.begin() + (long)es.size() / 4, es.end());
                floorE = std::max(es[es.size() / 4] / 0.38, 1e-12);
            }
            oh = std::polar(1.0, -2 * kPi * (low + 200.0) * (double)start / kFs);
            ol = std::polar(1.0, -2 * kPi * low * (double)start / kFs);
            zh = zl = cd(0, 0); histN = 0;
        }
        for (int64_t i = start; i < na; i++) envSample(ring[(size_t)(i & kMask)], i);
    }

    // ------------------------------------------------------------ bursts
    void envSample(float x, int64_t idx) {
        zh += (double)x * oh; zl += (double)x * ol;
        oh *= wh; ol *= wl;
        if ((idx % kHop) != kHop - 1) return;
        oh /= std::abs(oh); ol /= std::abs(ol);
        histH[histN & 3] = zh; histL[histN & 3] = zl; histN++;
        zh = zl = cd(0, 0);
        if (histN < 4) return;
        const double E = std::norm(histH[0] + histH[1] + histH[2] + histH[3]) + std::norm(histL[0] + histL[1] + histL[2] + histL[3]);
        if (collect) { collect->push_back(E); return; }
        if (!on) {
            if (E < 3.0 * floorE) floorE += (E - floorE) * 0.01;   // the mean noise energy, signal excluded
            if (E > 6.0 * floorE) { on = true; burstStart = idx - 32; lastOn = idx; offCnt = 0; }
        } else {
            if (E > 3.0 * floorE) { lastOn = idx; offCnt = 0; }
            else if (++offCnt >= 24) { on = false; endBurst(burstStart, lastOn); }
            else if (idx - burstStart > 6 * (int64_t)kFs) { on = false; endBurst(burstStart, idx); }
        }
    }

    void endBurst(int64_t s, int64_t e) {
        if (e - s < 560) return;
        lastActivity = na;
        walk(std::max<int64_t>(s, 0), e);
    }

    // ------------------------------------------------------------ the bits of a region
    void prefix(int64_t a, int64_t b) {
        r0 = std::max<int64_t>(a, std::max<int64_t>(0, na - kRingN + 8));
        const int64_t r1 = std::min(b, na);
        const size_t L = (size_t)std::max<int64_t>(0, r1 - r0);
        PH.assign(L + 1, cd(0, 0)); PL.assign(L + 1, cd(0, 0));
        cd ph = std::polar(1.0, -2 * kPi * (lockLow + 200.0) * (double)r0 / kFs), pl = std::polar(1.0, -2 * kPi * lockLow * (double)r0 / kFs);
        const cd w1 = wh, w2 = wl;
        for (size_t i = 0; i < L; i++) {
            const double v = ring[(size_t)((r0 + (int64_t)i) & kMask)];
            PH[i + 1] = PH[i] + v * ph; PL[i + 1] = PL[i] + v * pl;
            ph *= w1; pl *= w2;
            if ((i & 255) == 255) { ph /= std::abs(ph); pl /= std::abs(pl); }
        }
    }
    bool inside(int64_t a, int64_t b) const { return a >= r0 && b <= r0 + (int64_t)PH.size() - 1; }
    // soft value of the window [a, b): +1 the upper tone, -1 the lower tone; mh, ml the tone magnitudes
    double soft(int64_t a, int64_t b, double* mh = nullptr, double* ml = nullptr) const {
        const double h = std::abs(PH[(size_t)(b - r0)] - PH[(size_t)(a - r0)]), l = std::abs(PL[(size_t)(b - r0)] - PL[(size_t)(a - r0)]);
        if (mh) *mh = h;
        if (ml) *ml = l;
        return (h - l) / (h + l + 1e-12);
    }
    std::vector<int> offsets() const {
        std::vector<int> v;
        v.push_back(0);
        for (int o = 1; o <= kSearch; o++) { v.push_back(o * kHop); v.push_back(-o * kHop); }
        return v;
    }

    struct PktHit { pactor::Packet p; int64_t pos = 0; bool fast = false, inverted = false; float snr = 0; int crcMode = 0; };

    bool tryPacket(int64_t pos, PktHit& hit) {
        for (int off : offsets()) {
            for (int sp = 0; sp < 2; sp++) {
                const int bl = sp ? kBitFast : kBitSlow, nb = sp ? pactor::kBitsFast : pactor::kBitsSlow;
                const int64_t a = pos + off;
                if (!inside(a, a + (int64_t)bl * nb)) continue;
                std::vector<double> s((size_t)nb);
                double eon = 0, eoff = 0;
                for (int i = 0; i < nb; i++) {
                    double mh, ml;
                    s[(size_t)i] = soft(a + (int64_t)i * bl, a + (int64_t)(i + 1) * bl, &mh, &ml);
                    eon += std::max(mh, ml) * std::max(mh, ml); eoff += std::min(mh, ml) * std::min(mh, ml);
                }
                for (int pol = 0; pol < 2; pol++) {
                    std::vector<uint8_t> bits((size_t)nb);
                    for (int i = 0; i < nb; i++) bits[(size_t)i] = (uint8_t)((s[(size_t)i] > 0) ^ pol);
                    int he = 9, cm = 0;
                    pactor::Packet p;
                    if (pactor::packetParse(bits, p, &he, &cm) && he <= 1) {
                        hit.p = p; hit.pos = a; hit.fast = sp == 1; hit.inverted = pol == 1; hit.crcMode = cm;
                        hit.snr = (float)std::min(40.0, 10 * std::log10((eon + 1e-12) / (eoff + 1e-12)));
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // best-guess soft bits of a packet that failed its CRC. Without a reference: the speed and timing with the most confident bits over the whole packet
    // and the alternating 0101.. of the header 55 / AA. With the soft bits of an earlier bad packet: the hypothesis that lines up with it best
    // (a repeat of the same packet), when that is clearly better than chance; otherwise as without.
    bool bestSoft(int64_t pos, std::vector<float>& out, int64_t* where, const std::vector<float>* ref = nullptr) {
        double bestH = -1, bestHdr = 0, bestConf = 0, bestR = 0;
        std::vector<float> outH, outR;
        int64_t whereH = pos, whereR = pos;
        for (int off : offsets()) for (int sp = 0; sp < 2; sp++) {
            const int bl = sp ? kBitFast : kBitSlow, nb = sp ? pactor::kBitsFast : pactor::kBitsSlow;
            const int64_t a = pos + off;
            if (!inside(a, a + (int64_t)bl * nb)) continue;
            std::vector<float> v((size_t)nb);
            double conf = 0, hc = 0, rc = 0;
            for (int i = 0; i < nb; i++) {
                v[(size_t)i] = (float)soft(a + (int64_t)i * bl, a + (int64_t)(i + 1) * bl);
                conf += std::fabs(v[(size_t)i]);
                if (i < 8) hc += ((i & 1) ? -1.0 : 1.0) * v[(size_t)i];
            }
            conf /= nb; hc = std::fabs(hc) / 8;
            if (ref && ref->size() == (size_t)nb) {
                for (int i = 0; i < nb; i++) rc += (*ref)[(size_t)i] * v[(size_t)i];
                rc = std::fabs(rc) / nb;
                if (rc > bestR) { bestR = rc; outR = v; whereR = a; }
            }
            if (conf + hc > bestH) { bestH = conf + hc; bestHdr = hc; bestConf = conf; outH = v; whereH = a; }
        }
        if (bestR > 0.2) { out = outR; *where = whereR; return true; }
        out = outH; *where = whereH;
        return bestConf > 0.3 && bestHdr > 0.2;
    }

    struct CsHit { int n = 0, dist = 99; int64_t pos = 0; bool inverted = false; };
    bool tryCS(int64_t pos, CsHit& hit) {
        for (int off : offsets()) {
            const int64_t a = pos + off;
            if (!inside(a, a + 12 * kBitSlow)) continue;
            double s[12];
            for (int i = 0; i < 12; i++) s[i] = soft(a + (int64_t)i * kBitSlow, a + (int64_t)(i + 1) * kBitSlow);
            for (int pol = 0; pol < 2; pol++) {
                uint8_t b[12];
                for (int i = 0; i < 12; i++) b[i] = (uint8_t)((s[i] > 0) ^ pol);
                int d = 0;
                const int n = pactor::csMatch(b, &d);
                if (n && d < hit.dist) { hit.n = n; hit.dist = d; hit.pos = a; hit.inverted = pol == 1; }
            }
        }
        return hit.n && hit.dist <= 1;
    }

    // ------------------------------------------------------------ walking a burst
    void walk(int64_t s, int64_t e) {
        prefix(s - 400, e + 600);
        int64_t pos = s;
        int guard = 0;
        while (guard++ < 12) {
            const int64_t rem = e - pos;
            if (rem < 720) break;
            PktHit hit;
            if (rem >= 6400 && tryPacket(pos, hit)) { gotPacket(hit); pos = hit.pos + kPktSamples; continue; }
            CsHit cs;
            if (rem >= 720 && tryCS(pos, cs)) {
                const int64_t next = cs.pos + kCsSamples, nrem = e - next;
                PktHit peek;
                // the burst is a control signal and a packet long: the packet may be damaged (its CRC fails), the control signal is still real
                if (nrem < 720 || (nrem >= 6400 && tryPacket(next, peek)) || (nrem >= 7300 && nrem <= 8000)) { gotCs(cs); pos = next; continue; }
            }
            if (rem >= 6800) {
                std::vector<float> soft;
                int64_t where = pos;
                gotBad(pos, bestSoft(pos, soft, &where, badSoft.empty() ? nullptr : &badSoft) ? &soft : nullptr);
                pos += kPktSamples;
                continue;
            }
            break;
        }
    }

    void addFrame(PactorFrameLog f) {
        frames.insert(frames.begin(), std::move(f));
        if (frames.size() > kPactorFramesMax) frames.pop_back();
        changed = true;
    }
    void addStation(const std::string& call, const char* how, int64_t utc) {
        for (auto& st : stations) if (st.call == call) { st.count++; st.lastSeen = utc; if (std::string(how) == "called in a connect packet") st.how = how; return; }
        PactorStation st;
        st.call = call; st.count = 1; st.firstSeen = st.lastSeen = utc; st.how = how;
        stations.insert(stations.begin(), st);
        if (stations.size() > kPactorStationsMax) stations.pop_back();
    }

    void appendText(const std::string& t) {
        if (t.empty()) return;
        if (lastTextSender != sender) { text += (text.empty() ? "" : "\n") + std::string("[") + std::to_string(sender) + "] "; lastTextSender = sender; }
        text += t;
        if (text.size() > kPactorTextMax) text.erase(0, text.size() - kPactorTextMax);
    }

    void gotPacket(const PktHit& h, bool mem = false) {
        const double ts = timeOf(h.pos);
        PactorFrameLog f;
        f.utc = (int64_t)(t0 + ts); f.timeSec = ts;
        f.baud = h.fast ? 200 : 100;
        f.header = h.p.header;
        const pactor::Status st = pactor::statusParse(h.p.status);
        f.count = st.count; f.huffman = st.huffman; f.breakIn = st.breakIn; f.qrt = st.qrt;
        f.crcOk = true; f.memArq = mem; f.inverted = h.inverted; f.snrDb = h.snr; f.toneHz = lockLow; f.sender = sender;
        std::vector<uint8_t> key = h.p.data;
        key.push_back(h.p.header); key.push_back(h.p.status);
        const bool same = haveLast && key == lastKey && ts - lastPktTime < 12.0;
        std::string call;
        char hx[8];
        for (size_t i = 0; i < h.p.data.size(); i++) { snprintf(hx, sizeof hx, "%02X%s", h.p.data[i], i + 1 < h.p.data.size() ? " " : ""); f.hex += hx; }
        if (pactor::callParse(h.p.data, call) && h.p.header == pactor::kHeaderA && !h.fast) {
            f.kind = st.qrt ? "QRT" : "connect"; f.call = call; f.text = call;
            if (!same) { connects++; addStation(call, "called in a connect packet", f.utc); }
        } else {
            f.kind = "data";
            if (st.huffman) {
                std::vector<uint8_t> bits = pactor::bytesToBits(h.p.data);
                f.text = pactor::huffDecode(bits);
            } else {
                f.text = pactor::asciiText(h.p.data);
                while (!f.text.empty() && (f.text.back() == ' ' || f.text.back() == '\0')) f.text.pop_back();
            }
            if (st.qrt) f.kind = "QRT";
        }
        if (same) {
            f.repeat = true; repeats++;
            if (csSinceLast == false) f.fec = true;
        } else if (f.kind == "data") {
            appendText(f.text);
            // calls named in the text
            std::string tok;
            for (char c : f.text + " ") {
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) tok += c;
                else { if (looksLikeCall(tok)) addStation(tok, "named in text", f.utc); tok.clear(); }
            }
        }
        if (!csSinceLast) { if (++noCsRun >= 3) fecMode = true; } else noCsRun = 0;
        if (h.fast) fastPk++; else slowPk++;
        okPk++;
        haveLast = true; lastKey = key; lastPktTime = ts; csSinceLast = false;
        lastFrameSec = ts;
        badSoft.clear();
        addFrame(std::move(f));
    }

    void gotCs(const CsHit& c) {
        PactorFrameLog f;
        const double ts = timeOf(c.pos);
        f.utc = (int64_t)(t0 + ts); f.timeSec = ts; f.baud = 100;
        f.kind = "CS" + std::to_string(c.n);
        f.crcOk = c.dist == 0; f.inverted = c.inverted; f.sender = sender; f.toneHz = lockLow;
        static const char* kMean[4] = {"acknowledge", "reject / busy", "changeover", "speed change"};
        f.text = kMean[c.n - 1];
        if (c.n == 3) { changeovers++; sender = sender == 1 ? 2 : 1; f.text += " (the other side sends next)"; }
        csCount++; csSinceLast = true; fecMode = false; noCsRun = 0;
        lastFrameSec = ts;
        addFrame(std::move(f));
    }

    void gotBad(int64_t pos, std::vector<float>* soft) {
        const double ts = timeOf(pos);
        if (soft && !badSoft.empty() && badSoft.size() == soft->size() && ts - badTime < 8.0) {
            double corr = 0;
            for (int i = 0; i < 8; i++) corr += badSoft[(size_t)i] * (*soft)[(size_t)i];
            {   // the header decides how the two copies line up (the tone polarity swaps every cycle); both ways are tried, the CRC decides
                std::vector<float> keep;
                for (int pass = 0; pass < 2; pass++) {
                    const float sg = ((pass == 0) == (corr > 0)) ? 1.f : -1.f;
                    std::vector<float> c(soft->size());
                    for (size_t i = 0; i < c.size(); i++) c[i] = badSoft[i] + sg * (*soft)[i];
                    if (pass == 0) keep = c;
                    for (int pol = 0; pol < 2; pol++) {
                        std::vector<uint8_t> bits(c.size());
                        for (size_t i = 0; i < c.size(); i++) bits[i] = (uint8_t)((c[i] > 0) ^ pol);
                        pactor::Packet p;
                        int he = 9;
                        if (pactor::packetParse(bits, p, &he) && he <= 1) {
                            PktHit h; h.p = p; h.pos = pos; h.fast = soft->size() == (size_t)pactor::kBitsFast; h.inverted = false; h.snr = 0;
                            memSaves++;
                            gotPacket(h, true);
                            badTime = -1e9;
                            return;
                        }
                    }
                }
                badSoft = keep; badTime = ts; badPk++;
                logBad(ts, soft->size() == (size_t)pactor::kBitsFast);
                return;
            }
        }
        badPk++;
        if (soft) { badSoft = *soft; badTime = ts; } else badSoft.clear();
        logBad(ts, soft && soft->size() == (size_t)pactor::kBitsFast);
    }
    void logBad(double ts, bool fast) {
        PactorFrameLog f;
        f.utc = (int64_t)(t0 + ts); f.timeSec = ts; f.kind = "bad packet"; f.baud = fast ? 200 : 100; f.sender = sender; f.toneHz = lockLow;
        addFrame(std::move(f));
    }

    // ------------------------------------------------------------ report
    void report() {
        std::lock_guard<std::mutex> lk(mu);
        pub.seq++;
        const double secs = (double)nIn / std::max(curRate, 1.0);
        pub.timeSec = secs;
        pub.state = !running ? 0 : (secs - lastFrameSec < 30 ? 2 : (locked ? 1 : 0));
        pub.cfoHz = locked ? lockLow + 100.0 - 1500.0 : 0;
        pub.snrDb = lastSnr;
        pub.levelDb = levelHist.empty() ? -200.f : levelHist.back();
        pub.dataValid = pub.state == 2;
        pub.blocksOk = okPk; pub.blocksBad = badPk;
        pub.pairLowHz = locked ? lockLow : 0;
        pub.csCount = csCount; pub.memArqSaves = memSaves; pub.repeats = repeats; pub.fastPackets = fastPk; pub.slowPackets = slowPk;
        pub.changeovers = changeovers; pub.connects = connects; pub.fecMode = fecMode;
        pub.hasFullHuffman = pactor::huffComplete();
        if (changed) { pub.frames = frames; pub.stations = stations; pub.text = text; pub.levelHist = levelHist; changed = false; }
    }
};

PactorReceiver::PactorReceiver() : p_(std::make_unique<Impl>()) {}
PactorReceiver::~PactorReceiver() = default;

void PactorReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void PactorReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool PactorReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= pactorTuning().minSampleRate - 1; }
void PactorReceiver::reset() { p_->resetReq = true; }

void PactorReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) { std::lock_guard<std::mutex> lk(m.mu); m.front.setOffsetHz(m.offsetHz); }
    if (m.clearReq.exchange(false)) { m.frames.clear(); m.stations.clear(); m.text.clear(); m.levelHist.clear(); m.badSoft.clear(); m.lastTextSender = 0; m.changed = true; }
    if (m.curRate <= 0) return;
    m.nIn += (int64_t)n;
    if (m.running) {
        m.base.clear();
        m.front.process(x, n, m.base);
        for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        m.aud.clear();
        double pw = 0;
        if (const int d = hfDemodResolve(m.demodSet.load(), 0); d != m.usb.demod()) m.usb.setDemod(d);
        m.usb.process(m.base.data(), m.base.size(), m.aud, pw);
        m.listen.play(m.aud.data(), m.aud.size());
        m.process(m.aud.data(), m.aud.size());
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

bool PactorReceiver::telemetry(PactorTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void PactorReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void PactorReceiver::clearResults() {
    p_->clearReq = true;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.frames.clear(); p_->pub.stations.clear(); p_->pub.text.clear(); p_->pub.levelHist.clear();
    p_->pub.seq++;
}
void PactorReceiver::flush() {
    Impl& m = *p_;
    if (!m.running) return;
    if (m.on) { m.on = false; m.endBurst(m.burstStart, m.lastOn); }
    m.changed = true;
    m.report();
}
void PactorReceiver::setListen(bool on) { p_->listen.on = on; }
void PactorReceiver::setDemod(int d) { p_->demodSet = d; }
int PactorReceiver::demod() const { return p_->demodSet.load(); }
bool PactorReceiver::listening() const { return p_->listen.on.load(); }
void PactorReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void PactorReceiver::setMuted(bool m) { p_->listen.muted = m; }
void PactorReceiver::setSilent(bool s) { p_->listen.silent = s; }
void PactorReceiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }

ModeTuning pactorTuning() {
    ModeTuning t;
    t.stdMode = 205; t.id = "pactor"; t.name = "PACTOR-I";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 14.080;   // a starting point in the digital part of 20 m (upper sideband dial)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.004;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;                              // as the other HF digital modes: the radio sits 20 kHz above the dial
    return t;
}

const PactorPreset* pactorPresets(int& count) {
    static const PactorPreset kP[] = {{"80 m", 3.590}, {"40 m", 7.045}, {"30 m", 10.140}, {"20 m", 14.080}, {"17 m", 18.100}, {"15 m", 21.090}, {"10 m", 28.120}};
    count = (int)(sizeof kP / sizeof *kP);
    return kP;
}

} // namespace dect2

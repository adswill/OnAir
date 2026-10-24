// dPMR receiver (see dpmr_rx.h).
#include "dect2/dpmr/dpmr_rx.h"
#include "dect2/dsp/cic3.h"
#include "dect2/results/results_io.h"
#include "dect2/dpmr/dpmr_tel.h"
#include "dect2/dpmr/dpmr_proto.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include "dect2/audio/audioout.h"
#include "dect2/voice/voice_monitor.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <complex>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kFs = 48000;            // demodulator rate: 20 samples per symbol
constexpr int kSps = 20;
constexpr int kYMask = 32767;
constexpr size_t kMaxEvents = 200;
constexpr double kUnit = 350.0;          // Hz per symbol level (+3 = 1050 Hz)

float pearson(const float* v, const int* s, int n) {
    double mv = 0, ms = 0;
    for (int i = 0; i < n; i++) { mv += v[i]; ms += s[i]; }
    mv /= n; ms /= n;
    double c = 0, a = 0, b = 0;
    for (int i = 0; i < n; i++) { const double x = v[i] - mv, y = s[i] - ms; c += x * y; a += x * x; b += y * y; }
    return a > 0 && b > 0 ? (float)(c / std::sqrt(a * b)) : 0.f;
}
void lsFit(const float* v, const int* s, int n, double& g, double& d) {   // v = g * s + d
    double ms = 0, mv = 0;
    for (int i = 0; i < n; i++) { ms += s[i]; mv += v[i]; }
    ms /= n; mv /= n;
    double c = 0, a = 0;
    for (int i = 0; i < n; i++) { c += (s[i] - ms) * (v[i] - mv); a += (s[i] - ms) * (s[i] - ms); }
    g = a > 0 ? c / a : 0; d = mv - g * ms;
}
int nearestLevel(double v) { return v >= 2 ? 3 : v >= 0 ? 1 : v >= -2 ? -1 : -3; }
int pmrChannelOf(double mhz) {
    const double k = (mhz - 446.103125) / 0.00625;
    const long r = std::lround(k);
    return std::fabs(k - (double)r) < 0.05 && r >= 0 && r <= 15 ? (int)r + 1 : 0;
}
} // namespace

struct DpmrReceiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0, tunedSet = 0;
    std::atomic<bool> resetReq{true}, scanSet{false};
    std::function<void(const std::string&)> log;
    std::function<void(const float*, size_t)> tapSet, tap;
    std::function<void(const uint8_t*)> vtapSet, vtap;
    std::atomic<bool> tapReq{false};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    voice::Monitor* monSet = nullptr;   // setVoiceMonitor; else voice::monitor("dpmr") while not silent
    voice::CallInfo playing;            // the call whose voice is in aud8
    voice::Monitor* activeMonitor() { return monSet ? monSet : silent.load() ? nullptr : &voice::monitor("dpmr"); }
    DpmrTelemetry pub, tel;

    double rate = 0, offset = 0;
    int baseCh = 0, curCh = 0;       // the PMR446 channel tuned to, and the one listened to (scan)
    double lastHop = 0;
    double power = 0; int64_t nPower = 0, nIn = 0;
    double nextReport = 0;
    double sigSec() const { return rate > 0 ? (double)nIn / rate : 0; }

    // ------------------------------------------------------------ front end: mix, CIC, windowed-sinc to 48 kHz, FM discriminator
    int D = 1;
    double fd = 0, inv = 1;
    std::complex<double> lo{1, 0}, loStep{1, 0};
    int loCount = 0;
    Cic3 cic;   // the decimator (dsp/cic3.h: exact for as long as it runs)
    int decCount = 0;
    std::vector<cf32> dbuf;
    int64_t dN = 0;
    double rp = 0, rstep = 0;
    int M = 0, nPh = 64;
    std::vector<float> fir;
    cf32 prevZ{0, 0};

    double hopHz() const { return baseCh && curCh ? (curCh - baseCh) * 6250.0 : 0; }
    void initFront() {
        D = std::max(1, (int)std::floor(rate / 100000.0));
        fd = rate > 0 ? rate / D : 0;
        inv = 1.0 / ((double)D * D * D);
        lo = 1; loCount = 0;
        loStep = rate > 0 ? std::polar(1.0, -2 * kPi * (offset + hopHz()) / rate) : std::complex<double>(1, 0);
        cic.reset();
        decCount = 0;
        dbuf.assign(8192, cf32(0, 0)); dN = 0;
        rstep = fd / kFs;
        M = std::max(8, (int)std::ceil(5.5 * fd / 3000.0 / 2));   // a 6.25 kHz raster: the next channel must stay out
        fir.assign((size_t)nPh * 2 * M, 0.f);
        const double fc = 3600.0 / std::max(1.0, fd);
        for (int ph = 0; ph < nPh; ph++) {
            const double frac = (double)ph / nPh;
            double sum = 0;
            for (int k = 0; k < 2 * M; k++) {
                const double t = (double)(k - M + 1) - frac;
                const double x = 2 * fc * t;
                const double s = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                const double w = 0.42 + 0.5 * std::cos(kPi * t / M) + 0.08 * std::cos(2 * kPi * t / M);
                const double v = std::fabs(t) < M ? 2 * fc * s * w : 0;
                fir[(size_t)ph * 2 * M + k] = (float)v;
                sum += v;
            }
            for (int k = 0; k < 2 * M; k++) fir[(size_t)ph * 2 * M + k] /= (float)sum;
        }
        rp = M + 2;
        prevZ = cf32(0, 0);
    }

    void input(std::complex<double> x) {
        const std::complex<double> v = x * lo;
        lo *= loStep;
        if (++loCount >= 4096) { loCount = 0; lo /= std::abs(lo); }
        cic.integrate(v);
        if (++decCount < D) return;
        decCount = 0;
        const std::complex<double> c = cic.comb();
        dbuf[(size_t)(dN & 8191)] = cf32((float)(c.real() * inv), (float)(c.imag() * inv));
        dN++;
        while (dN > (int64_t)std::floor(rp) + M) {
            const int64_t base = (int64_t)std::floor(rp);
            const int ph = std::min(nPh - 1, (int)std::lround((rp - (double)base) * nPh));
            const float* h = &fir[(size_t)ph * 2 * M];
            cf32 acc(0, 0);
            const int64_t i0 = base - M + 1;
            for (int k = 0; k < 2 * M; k++) acc += dbuf[(size_t)((i0 + k) & 8191)] * h[k];
            rp += rstep;
            const cf32 d = acc * std::conj(prevZ);
            prevZ = acc;
            push((float)(std::atan2(d.imag(), d.real()) * kFs / (2 * kPi)));
            step();
        }
    }

    // ------------------------------------------------------------ symbol filter and the frame state machine
    std::vector<float> y = std::vector<float>(kYMask + 1, 0.f);
    int64_t yN = 0;
    // Symbol filter: a windowed-sinc low pass to 1500 Hz (the 2400 baud raised-cosine signal reaches 1440 Hz). The discriminator's noise
    // grows with frequency, and the half-symbol box used before let much of it through: the threshold moved by about 5 dB.
    static constexpr int sD = 2 * kSps;          // the filter's delay in samples
    std::vector<float> sh, shist;      // taps, history (doubled)
    int shPos = 0;
    void initSym() {
        constexpr double lpf = 1500.0;
        const int n = 2 * sD + 1;
        sh.assign((size_t)n, 0.f);
        double sum = 0;
        for (int k = 0; k < n; k++) {
            const double t = k - sD, x = 2 * lpf / kFs * t;
            const double sc = t == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double w = 0.42 + 0.5 * std::cos(kPi * t / (sD + 1)) + 0.08 * std::cos(2 * kPi * t / (sD + 1));
            sh[(size_t)k] = (float)(sc * w); sum += sh[(size_t)k];
        }
        for (float& v : sh) v = (float)(v / sum);
        shist.assign(2 * (size_t)n, 0.f); shPos = 0;
    }
    void push(float f) {
        if (sh.empty()) initSym();
        const int n = (int)sh.size();
        shist[(size_t)shPos] = shist[(size_t)(shPos + n)] = f;
        shPos = (shPos + 1) % n;
        const float* x = shist.data() + shPos;
        float acc = 0;
        for (int k = 0; k < n; k++) acc += x[k] * sh[(size_t)(n - 1 - k)];
        y[(size_t)(yN & kYMask)] = acc;
        yN++;
    }
    float at(int64_t i) const { return y[(size_t)((i + sD) & kYMask)]; }
    void symsAt(int64_t first, int n, float* v) const { for (int k = 0; k < n; k++) v[k] = at(first + (int64_t)k * kSps); }

    enum Mode { Search, Header, Sync, Frame, EndWait } mode = Search;
    enum SyncT { S1, S2, S3, S4 };
    int64_t candN = -1; float candR = 0; int candT = 0;
    int64_t mark = 0;            // Header / EndWait: the last sync symbol; Frame: the frame's first symbol; Sync: the expected frame start
    bool packetHdr = false, fromSearch = false, fly = false;
    int frameIdx = 0, miss = 0, bad = 0;
    double g = kUnit, d = 0;
    float pol = 1;               // the polarity of the last good frames (FS1 and FS4 differ only by it)

    bool levelOk(double gg) const { return std::fabs(gg) > 0.4 * kUnit && std::fabs(gg) < 1.8 * kUnit; }
    bool symsMatch(const float* v, const int* s, int n, double gg, double dd, int tol) const {
        int e = 0;
        for (int k = 0; k < n; k++) if (nearestLevel((v[k] - dd) / gg) != s[k]) e++;
        return e <= tol;
    }
    void fitSync(int64_t first, const int* s, int n) {
        float v[32]; double gg, dd;
        symsAt(first, n, v);
        lsFit(v, s, n, gg, dd);
        if (levelOk(gg) && (mode == Search || gg * g > 0)) { g = mode == Search ? gg : 0.5 * g + 0.5 * gg; d = mode == Search ? dd : 0.5 * d + 0.5 * dd; }
    }

    void search(int64_t last) {
        static const int* pats[4] = {dpmr::kFs1, dpmr::kFs2, dpmr::kFs3, dpmr::kFs4};
        float v[dpmr::kFsLong];
        for (int t0 = 0; t0 < 3; t0++) {   // FS4 is FS1 upside down: which one it is follows from the polarity last seen
            const int n = t0 == S2 ? dpmr::kFsShort : dpmr::kFsLong;
            const int64_t first = last - (int64_t)(n - 1) * kSps;
            symsAt(first, n, v);
            float r = pearson(v, pats[t0], n);
            int t = t0;
            if (t0 == S1 && r * pol < 0) { t = S4; r = -r; }
            const float thr = t == S2 ? 0.9f : 0.85f;
            if (std::fabs(r) > thr && std::fabs(r) > candR) {
                double gg, dd;
                lsFit(v, pats[t], n, gg, dd);
                if (levelOk(gg) && symsMatch(v, pats[t], n, gg, dd, t == S2 ? 0 : 1)) { candN = last; candR = std::fabs(r); candT = t; }
            }
        }
        if (candN < 0 || last - candN <= kSps / 2) return;
        const int64_t e = candN; const int t = candT;
        candN = -1; candR = 0;
        const int n = t == S2 ? dpmr::kFsShort : dpmr::kFsLong;
        fitSync(e - (int64_t)(n - 1) * kSps, pats[t], n);
        fly = false; miss = 0; bad = 0;
        if (t == S1 || t == S4) { tel.fs1 += t == S1; tel.fs4 += t == S4; packetHdr = t == S4; mark = e; mode = Header; }
        else if (t == S3) { tel.fs3++; mark = e; mode = EndWait; }
        else { tel.fs2++; mark = e - 11 * kSps; fromSearch = true; frameIdx = 0; mode = Frame; }
    }

    void step() {
        const int64_t last = yN - 1 - 2 * sD;
        if (last < 64 * kSps) return;
        switch (mode) {
        case Search: search(last); break;
        case Header:
            if (last >= mark + 132 * kSps) { decodeHeader(mark); mark += 133 * kSps; frameIdx = 0; fromSearch = false; mode = Sync; }
            break;
        case EndWait:
            if (last >= mark + 36 * kSps) { decodeEnd(mark + kSps); mode = Search; }
            break;
        case Frame:
            if (last >= mark + 191 * kSps) {
                int fn = -1;
                const bool ok = decodeFrame(mark, fn);
                if (fromSearch && !ok) { mode = Search; break; }
                fromSearch = false;
                bad = ok ? 0 : bad + 1;
                if (bad >= 3) { lost(); mode = Search; break; }
                frameIdx = (ok && fn >= 0 ? fn : frameIdx) + 1;
                mark += 192 * kSps; mode = Sync;
            }
            break;
        case Sync: {
            const int W = kSps / 4;
            if (last < mark + 23 * kSps + W) break;
            float v[dpmr::kFsLong];
            int ccs[dpmr::kCcSym];
            const bool even = frameIdx % 2 == 0;
            const bool haveCc = tel.cc >= 0;
            if (haveCc) dpmr::ccSymbols(tel.cc, ccs);
            const int* p2 = even ? dpmr::kFs2 : haveCc ? ccs : nullptr;
            const float pol = g > 0 ? 1.f : -1.f;
            float b2 = -2, b3 = -2; int64_t n2 = mark, n3 = mark;
            for (int64_t s = mark - W; s <= mark + W; s++) {
                if (p2) { symsAt(s, 12, v); const float r = pearson(v, p2, 12) * pol; if (r > b2) { b2 = r; n2 = s; } }
                symsAt(s, 24, v); const float r3 = pearson(v, dpmr::kFs3, 24) * pol; if (r3 > b3) { b3 = r3; n3 = s; }
            }
            if (b3 > 0.75f && b3 >= b2) { tel.fs3++; fitSync(n3, dpmr::kFs3, 24); mark = n3 + 23 * kSps; mode = EndWait; break; }
            if (p2 && b2 > 0.6f) { if (even) tel.fs2++; fitSync(n2, p2, 12); mark = n2; fly = false; miss = 0; mode = Frame; break; }
            if (!p2) { fly = false; mode = Frame; break; }             // a colour code frame before the colour code is known
            if (++miss <= 2) { fly = true; mode = Frame; break; }      // flywheel over a damaged sync
            lost(); mode = Search;
            break;
        }
        }
    }

    // ------------------------------------------------------------ frames
    double lastGood = -10;
    double berErr = 0, berBits = 0, snrAvg = 0;
    uint32_t callBer = 0, callBits = 0;
    int idHalves[4] = {-1, -1, -1, -1};
    bool sdOn = false; DpmrCall sd; int sdBlocks = 0;
    std::unique_ptr<NxdnAmbe> ambe;
    std::vector<float> aud8, pcmOut;
    float upPrev = 0;
    std::unique_ptr<AudioOut> audioOut;

    void addBer(int e, int n) { if (e < 0) return; berErr += e; berBits += n; callBer += (uint32_t)e; callBits += (uint32_t)n; }
    void event(const std::string& s) {
        char b[24];
        const time_t t = time(nullptr);
        struct tm gt;
#ifdef _WIN32
        gmtime_s(&gt, &t);
#else
        gmtime_r(&t, &gt);
#endif
        snprintf(b, sizeof b, "%02d:%02d:%02d  ", gt.tm_hour, gt.tm_min, gt.tm_sec);
        tel.events.insert(tel.events.begin(), b + s);
        if (tel.events.size() > kMaxEvents) tel.events.pop_back();
    }
    void pushLog(const DpmrCall& c) {
        tel.log.insert(tel.log.begin(), c);
        if (tel.log.size() > kDpmrLogMax) tel.log.pop_back();
    }
    void good(double snr) {
        lastGood = sigSec();
        tel.inverted = g < 0;
        pol = g < 0 ? -1.f : 1.f;
        tel.deviationHz = (float)(3 * std::fabs(g));
        tel.cfoHz = d;
        snrAvg = snrAvg == 0 ? snr : 0.8 * snrAvg + 0.2 * snr;
        tel.snrDb = (float)snrAvg;
    }
    // hard bits of n symbols from the first one, and their SNR
    double bitsAt(int64_t first, int nSym, uint8_t* bits) {
        double mse = 0;
        for (int k = 0; k < nSym; k++) {
            const double x = (at(first + (int64_t)k * kSps) - d) / g;
            const int l = nearestLevel(x);
            mse += (x - l) * (x - l);
            bits[2 * k] = l < 0; bits[2 * k + 1] = std::abs(l) == 3;
        }
        return 10 * std::log10(5.0 / std::max(1e-4, mse / std::max(1, nSym)));
    }
    // soft values of the same bits (> 0 a 1): the first bit of a dibit is 1 below zero, the second 1 on an outer level
    void softAt(int64_t first, int nSym, float* soft) const {
        for (int k = 0; k < nSym; k++) {
            const float x = (float)((at(first + (int64_t)k * kSps) - d) / g);
            soft[2 * k] = -x; soft[2 * k + 1] = std::fabs(x) - 2.f;
        }
    }
    // a block of Hamming words, soft decision (about 1.5 dB over the hard decoder)
    int blockAt(int64_t first, int nSym, int nWords, uint8_t* info) {
        float soft[288];
        softAt(first, nSym, soft);
        return dpmr::blockDecodeSoft(soft, nWords, info);
    }
    int ccAt(int64_t first) {
        float v[dpmr::kCcSym];
        for (int k = 0; k < dpmr::kCcSym; k++) v[k] = (float)((at(first + (int64_t)k * kSps) - d) / g);
        float sc = 0;
        const int cc = dpmr::ccDecode(v, &sc);
        return sc > 0.6f ? cc : -1;
    }
    DpmrCall fresh(int kind) {
        DpmrCall n;
        n.wallTime = (int64_t)time(nullptr); n.startSec = sigSec(); n.kind = kind; n.cc = tel.cc; n.channel = curCh;
        return n;
    }

    void decodeHeader(int64_t e) {
        tel.frames++;
        uint8_t air[dpmr::kHiBits], info[80];
        dpmr::Header h;
        bool ok = false; double snr = 0;
        for (int flip = 0; flip < 2 && !ok; flip++) {
            if (flip) { g = -g; packetHdr = !packetHdr; }   // FS1 seen as FS4 (or the other way): the polarity was wrong
            for (int part = 0; part < 2 && !ok; part++) {
                snr = bitsAt(e + (int64_t)(1 + 72 * part) * kSps, 60, air);
                const int fixed = blockAt(e + (int64_t)(1 + 72 * part) * kSps, 60, 10, info);
                if (fixed >= 0 && dpmr::headerParse(info, h)) { ok = true; addBer(fixed, 120); }
            }
            if (!ok) {
                // HI1 repeats HI0: both copies' soft values added are 3 dB stronger than either
                float s0[120], s1[120];
                softAt(e + kSps, 60, s0);
                softAt(e + 73 * kSps, 60, s1);
                for (int i = 0; i < 120; i++) s0[i] += s1[i];
                const int fixed = dpmr::blockDecodeSoft(s0, 10, info);
                if (fixed >= 0 && dpmr::headerParse(info, h)) { ok = true; addBer(fixed, 240); }
            }
        }
        if (!ok) { g = -g; packetHdr = !packetHdr; }
        const int cc = ccAt(e + 61 * kSps);
        if (cc >= 0) tel.cc = cc;
        if (!ok) { tel.hdrBad++; counts(); return; }
        tel.hdrOk++;
        good(snr);
        counts();
        char b[220];
        snprintf(b, sizeof b, "%s %s: %s -> %s, %s, %s, CC %d%s", packetHdr ? "FS4 packet header" : "header", dpmr::hdrTypeName(h.type), dpmrId(h.own).c_str(), dpmrId(h.called).c_str(),
                 dpmr::callTypeName(h.callType), dpmr::commModeName(h.commMode), tel.cc, h.emergency ? ", EMERGENCY" : "");
        event(b);
        if (h.type == dpmr::kHdrStatus) {
            DpmrCall n = fresh(1);
            n.src = h.own; n.dst = h.called; n.callType = h.callType; n.commMode = h.commMode; n.emergency = h.emergency; n.endSeen = true;
            n.text = "status " + std::to_string(h.extra);
            pushLog(n);
            return;
        }
        if (h.type != dpmr::kHdrComm) return;
        if (h.commMode == dpmr::kData1 || h.commMode == dpmr::kData2 || h.commMode == dpmr::kPacket) {
            endData();
            sdOn = true; sd = fresh(h.commMode == dpmr::kPacket ? 3 : 2);
            sd.src = h.own; sd.dst = h.called; sd.callType = h.callType; sd.commMode = h.commMode; sd.emergency = h.emergency;
            sdBlocks = h.extra;
            return;
        }
        if (tel.now.active && tel.now.src == h.own && tel.now.dst == h.called) return;
        startCall(h.own, h.called, h.callType, h.commMode, h.emergency, false);
    }

    void counts() {
        tel.blocksOk = tel.hdrOk + tel.cchOk + tel.endOk + tel.dataOk;
        tel.blocksBad = tel.hdrBad + tel.cchBad + tel.endBad + tel.dataBad;
        tel.berPct = berBits > 0 ? (float)(100 * berErr / berBits) : 0.f;
        if (tel.now.active) { tel.now.durSec = sigSec() - tel.now.startSec; tel.now.berPct = callBits ? 100.f * callBer / callBits : 0.f; }
    }

    bool decodeFrame(int64_t s, int& fn) {
        // the decision-directed level over the frame's own symbols first
        {
            float v[dpmr::kFrameSym]; int lv[dpmr::kFrameSym];
            symsAt(s, dpmr::kFrameSym, v);
            for (int k = 0; k < dpmr::kFrameSym; k++) lv[k] = nearestLevel((v[k] - d) / g);
            double gg, dd;
            lsFit(v, lv, dpmr::kFrameSym, gg, dd);
            if (gg * g > 0 && std::fabs(gg) > 0.5 * std::fabs(g) && std::fabs(gg) < 2 * std::fabs(g)) { g = gg; d = dd; }
        }
        tel.frames++;
        if (frameIdx % 2 == 1 || fromSearch) { const int cc = ccAt(s); if (cc >= 0 && frameIdx % 2 == 1) tel.cc = cc; }
        uint8_t air[dpmr::kTchBits], info[192];
        const double snr = bitsAt(s + 12 * kSps, 36, air);
        dpmr::Cch c;
        const int fixed = blockAt(s + 12 * kSps, 36, 6, info);
        const bool ok = fixed >= 0 && dpmr::cchParse(info, c);
        if (ok) { tel.cchOk++; addBer(fixed, 72); fn = c.frame; tel.slowData = c.slowData; good(snr); }
        else tel.cchBad++;
        if (fromSearch && !ok) { counts(); return false; }
        if (fromSearch) frameIdx = c.frame;   // late entry: the frame number places it in the superframe
        bitsAt(s + 48 * kSps, 144, air);
        const int mode = ok ? c.commMode : sdOn ? sd.commMode : tel.now.active ? tel.now.commMode : -1;
        if (mode == dpmr::kData1 || mode == dpmr::kData2 || mode == dpmr::kPacket) {
            const int fx = blockAt(s + 48 * kSps, 144, 24, info);
            int blk = 0; std::string txt;
            if (fx >= 0 && dpmr::dataParse(info, blk, txt)) {
                tel.dataOk++; addBer(fx, 288);
                if (sdOn && blk * 21 >= (int)sd.text.size()) {
                    if ((int)sd.text.size() < blk * 21) sd.text.append((size_t)(blk * 21) - sd.text.size(), '?');
                    sd.text += txt;
                }
            } else tel.dataBad++;
        } else if (mode >= 0) {
            if (ok && !tel.now.active && fromSearch) startCall(-1, -1, c.callType, c.commMode, c.emergency, true);
            if (ok && tel.now.active) {
                idHalves[c.frame & 3] = c.idPart;
                if (idHalves[0] >= 0 && idHalves[1] >= 0) tel.now.dst = ((int64_t)idHalves[0] << 12) | idHalves[1];
                if (idHalves[2] >= 0 && idHalves[3] >= 0) tel.now.src = ((int64_t)idHalves[2] << 12) | idHalves[3];
                if (tel.now.cc < 0) tel.now.cc = tel.cc;
            }
            if (tel.now.active && (ok || !fly)) {
                uint8_t pn[288];
                dpmr::pn9(pn, 288);
                for (int i = 0; i < 288; i++) air[i] ^= pn[i];
                for (int vf = 0; vf < 4; vf++) voiceFrame(air + 72 * vf);
            }
        }
        counts();
        return ok;
    }

    void decodeEnd(int64_t first) {
        uint8_t air[dpmr::kEndBits], info[48];
        const double snr = bitsAt(first, 36, air);
        dpmr::End e;
        const int fixed = blockAt(first, 36, 6, info);
        tel.frames++;
        if (fixed >= 0 && dpmr::endParse(info, e)) {
            tel.endOk++; addBer(fixed, 72); good(snr);
            if (sdOn) endData();
            else if (tel.now.active) { if (tel.now.src < 0) tel.now.src = e.own; endCall(true); }
            event("end frame from " + dpmrId(e.own));
        } else { tel.endBad++; if (sdOn) endData(); else if (tel.now.active) endCall(true); }
        counts();
    }

    void startCall(int64_t src, int64_t dst, int type, int commMode, bool emer, bool late) {
        if (tel.now.active) endCall(false);
        DpmrCall n = fresh(0);
        n.active = true; n.src = src; n.dst = dst; n.callType = type; n.commMode = commMode; n.emergency = emer; n.lateEntry = late;
        tel.now = n;
        for (int& h : idHalves) h = -1;
        callBer = callBits = 0;
        if (late) tel.lateEntries++;
        ambe = tel.ambe ? std::make_unique<NxdnAmbe>() : nullptr;
        if (late) event("voice joined late (CCH superframe)");
    }
    void endCall(bool seen) {
        if (!tel.now.active) return;
        flushAudio();
        tel.now.active = false;
        tel.now.endSeen = seen;
        tel.now.durSec = sigSec() - tel.now.startSec;
        pushLog(tel.now);
        char b[160];
        snprintf(b, sizeof b, "dPMR call %s -> %s (%s, CC %d), %.1f s%s", dpmrId(tel.now.src).c_str(), dpmrId(tel.now.dst).c_str(), dpmr::callTypeName(tel.now.callType), tel.now.cc,
                 tel.now.durSec, seen ? "" : ", signal lost");
        if (log) log(b);
    }
    void endData() {
        if (!sdOn) return;
        sdOn = false;
        sd.durSec = sigSec() - sd.startSec; sd.endSeen = true;
        pushLog(sd);
        event(std::string(sd.kind == 3 ? "packet data " : "short message ") + dpmrId(sd.src) + " -> " + dpmrId(sd.dst) + ": " + sd.text);
    }
    void lost() {
        endCall(false);
        endData();
    }

    void voiceFrame(const uint8_t vb[72]) {
        tel.now.voiceFrames++;
        tel.voiceFrames++;
        if (vtap) vtap(vb);
        voice::Monitor* mon = activeMonitor();
        if (mon) {   // the voice monitor (voice_monitor.h) lists the call and says whether it is played
            voice::CallInfo ci;
            ci.src = tel.now.src > 0 ? (uint32_t)tel.now.src : 0; ci.dst = tel.now.dst > 0 ? (uint32_t)tel.now.dst : 0;
            ci.group = tel.now.callType != dpmr::kIndividual; ci.emergency = tel.now.emergency; ci.codecMissing = !ambe || !ambe->ok();
            ci.errorRate = callBits > 200 ? (float)callBer / callBits : -1.f;
            if (tel.now.cc >= 0) ci.access = "CC " + std::to_string(tel.now.cc);
            if (tel.now.channel > 0) ci.access += (ci.access.empty() ? "" : ", ") + std::string("PMR446 ch ") + std::to_string(tel.now.channel);
            if (mon->offer(ci) != voice::Verdict::Play) return;
            playing = ci;
        }
        if (!ambe || !ambe->ok()) return;
        int16_t pcm[160];
        ambe->decode(vb, pcm);
        for (int16_t s : pcm) aud8.push_back(s / 32768.f);
        tel.voiceDecoded++;
        tel.now.voice = true;
        if (aud8.size() >= 1600) flushAudio();
    }

    void flushAudio() {
        if (aud8.empty()) return;
        if (tap) tap(aud8.data(), aud8.size());
        voice::Monitor* mon = activeMonitor();
        if (mon) mon->audio(playing, aud8.data(), aud8.size(), 1, 8000);   // the call's recording (when on)
        if (!silent.load() && mon) {
            pcmOut.clear();
            for (float s : aud8) {
                for (int k = 1; k <= 6; k++) { const float v = upPrev + (s - upPrev) * k / 6.f; pcmOut.push_back(v); pcmOut.push_back(v); }
                upPrev = s;
            }
            voice::ensureOutput(audioOut, *mon);   // on the mode's output device
            audioOut->setVolume(volume.load() * mon->volume()); audioOut->setMuted(muted.load());
            if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();
            audioOut->write(pcmOut.data(), (int)(pcmOut.size() / 2));
        }
        aud8.clear();
    }

    void resetChain() { mode = Search; candN = -1; candR = 0; g = kUnit; d = 0; std::fill(y.begin(), y.end(), 0.f); yN = 0; initSym(); }

    // The call log and the decoded headers stay through resets: Clear and a loaded file reach the feed thread's copy through these requests (mu), and the
    // published report at once
    bool clearReq = false, loadReq = false;
    DpmrTelemetry loaded;
    double loadedAt = 0;                   // the report's clock the loaded lists are on
    static void moveLists(DpmrTelemetry& to, DpmrTelemetry& from) {
        to.log = std::move(from.log); to.events = std::move(from.events);
        from.log.clear(); from.events.clear();
    }
    static void shiftTimes(DpmrTelemetry& t, double dt) {
        for (auto& c : t.log) c.startSec += dt;
    }
    void takeRequests() {   // feed thread
        std::lock_guard<std::mutex> lk(mu);
        if (clearReq) { DpmrTelemetry e; moveLists(tel, e); clearReq = false; }
        if (loadReq) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); loadReq = false; }
    }
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; baseCh = pmrChannelOf(tunedSet); }
        if (tel.now.active) endCall(false);
        curCh = baseCh;
        initFront();
        resetChain();
        power = 0; nPower = 0; nIn = 0; nextReport = 0.25 * rate; lastHop = 0;
        lastGood = -10; berErr = berBits = 0; snrAvg = 0; sdOn = false;
        const uint64_t seq = tel.seq;
        DpmrTelemetry keep;                          // the lists stay (clearResults): the clock starts again, they move back by the time it had reached
        moveLists(keep, tel);
        shiftTimes(keep, -tel.timeSec);
        tel = DpmrTelemetry(); tel.seq = seq;
        moveLists(tel, keep);
        tel.inputRate = rate;
        tel.channel = curCh;
        tel.ambe = nxdnAmbeAvailable(&tel.ambePath);
    }

    void scanStep() {
        tel.scanning = scanSet.load() && baseCh > 0;
        if (!tel.scanning) {
            if (curCh != baseCh) { curCh = baseCh; tel.channel = curCh; initFront(); resetChain(); }
            return;
        }
        const double t = sigSec();
        if (mode != Search || tel.now.active || sdOn || t - lastGood < 1.0 || t - lastHop < 0.5) return;
        lastHop = t;
        curCh = curCh % 16 + 1;
        tel.channel = curCh;
        tel.cc = -1;
        initFront();
        resetChain();
    }

    void report() {
        if (tapReq.exchange(false)) { std::lock_guard<std::mutex> lk(mu); tap = tapSet; vtap = vtapSet; }
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        if (!tel.ambe && tel.seq % 40 == 0) tel.ambe = nxdnAmbeAvailable(&tel.ambePath);
        if (tel.now.active) tel.now.durSec = sigSec() - tel.now.startSec;
        if (sdOn && sigSec() - sd.startSec > 10) endData();
        if (tel.now.active && mode == Search && sigSec() - lastGood > 1.0) endCall(false);
        scanStep();
        tel.state = tel.now.active ? 2 : sigSec() - lastGood < 1.5 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

DpmrReceiver::DpmrReceiver() : p_(std::make_unique<Impl>()) {}
DpmrReceiver::~DpmrReceiver() = default;

void DpmrReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void DpmrReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void DpmrReceiver::setChannel(double mhz) { std::lock_guard<std::mutex> lk(p_->mu); if (mhz != p_->tunedSet) { p_->tunedSet = mhz; p_->resetReq = true; } }
void DpmrReceiver::setScan(bool on) { p_->scanSet = on; }
bool DpmrReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= dpmrTuning().minSampleRate - 1; }
void DpmrReceiver::reset() { p_->resetReq = true; }

void DpmrReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    m.takeRequests();
    if (m.tapReq.load()) { std::lock_guard<std::mutex> lk(m.mu); m.tap = m.tapSet; m.vtap = m.vtapSet; m.tapReq = false; }
    if (m.rate <= 0 || n == 0) return;
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
        m.input(std::complex<double>(re, im));
        m.nIn++;
        if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); }
    }
    m.nPower += (int64_t)n;
}

void DpmrReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    DpmrTelemetry e;
    Impl::moveLists(p_->pub, e);
    p_->clearReq = true; p_->loadReq = false;
    p_->pub.seq++;
}
std::string DpmrReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.log, p_->pub.events);
}
bool DpmrReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    DpmrTelemetry l;
    if (!results::unpack(blob, t0, l.log, l.events)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    DpmrTelemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->loadReq = true;
    p_->pub.seq++;
    return true;
}
bool DpmrReceiver::telemetry(DpmrTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void DpmrReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void DpmrReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void DpmrReceiver::setMuted(bool m) { p_->muted = m; }
void DpmrReceiver::setSilent(bool s) { p_->silent = s; }
void DpmrReceiver::setVoiceMonitor(voice::Monitor* m) { p_->monSet = m; }
void DpmrReceiver::setAudioTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tapSet = std::move(cb); p_->tapReq = true; }
void DpmrReceiver::setVoiceTap(std::function<void(const uint8_t bits[72])> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->vtapSet = std::move(cb); p_->tapReq = true; }

ModeTuning dpmrTuning() {
    ModeTuning t;
    t.stdMode = 47; t.id = "dpmr"; t.name = "dPMR";
    t.minMhz = 30; t.maxMhz = 1000; t.defMhz = 446.103125;   // PMR446 digital channel 1
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.00625;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike
    return t;
}

} // namespace dect2

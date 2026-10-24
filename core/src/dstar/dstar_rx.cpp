// D-STAR receiver (see dstar_rx.h).
#include "dect2/dstar/dstar_rx.h"
#include "dect2/dsp/cic3.h"
#include "dect2/results/results_io.h"
#include "dect2/dstar/dstar_tel.h"
#include "dect2/dstar/dstar_proto.h"
#include "dect2/dstar/dstar_ambe.h"
#include "dect2/audio/audioout.h"
#include "dect2/voice/voice_monitor.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kFs = 48000;            // demodulator rate, 10 samples per bit
constexpr int kSps = 10;
constexpr int kYMask = 16383;
constexpr int kHdrSyncLen = 39;          // the last 24 bits of the bit sync and the 15-bit frame sync
constexpr int kSuperSamples = dstar::kSuperframe * dstar::kFrameBits * kSps;   // 420 ms from one data sync to the next
constexpr size_t kMaxMsgs = 200, kMaxPos = 500;

int8_t gHdrT[kHdrSyncLen], gDataT[24];
struct TInit {
    TInit() {
        for (int i = 0; i < 24; i++) gHdrT[i] = (i % 2 == 0) ? 1 : -1;
        for (int i = 0; i < 15; i++) gHdrT[24 + i] = dstar::kFrameSync[i] ? 1 : -1;
        for (int i = 0; i < 24; i++) gDataT[i] = dstar::kDataSync[i] ? 1 : -1;
    }
} gTInit;

float pearson(const float* v, const int8_t* s, int n) {
    double mv = 0, ms = 0;
    for (int i = 0; i < n; i++) { mv += v[i]; ms += s[i]; }
    mv /= n; ms /= n;
    double c = 0, a = 0, b = 0;
    for (int i = 0; i < n; i++) { const double x = v[i] - mv, y = s[i] - ms; c += x * y; a += x * x; b += y * y; }
    return a > 0 && b > 0 ? (float)(c / std::sqrt(a * b)) : 0.f;
}
// least squares v = g * s + d
void lsFit(const float* v, const int8_t* s, int n, double& g, double& d) {
    double ms = 0, mv = 0;
    for (int i = 0; i < n; i++) { ms += s[i]; mv += v[i]; }
    ms /= n; mv /= n;
    double c = 0, a = 0;
    for (int i = 0; i < n; i++) { c += (s[i] - ms) * (v[i] - mv); a += (s[i] - ms) * (s[i] - ms); }
    g = a > 0 ? c / a : 0; d = mv - g * ms;
}
std::string trimmed(std::string s) {
    for (auto& c : s) if ((unsigned char)c < 32 || (unsigned char)c > 126) c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
} // namespace

struct DstarReceiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true};
    std::function<void(const std::string&)> log;
    std::function<void(const float*, size_t)> tapSet, tap;
    std::function<void(const uint8_t*)> vtapSet, vtap;
    std::atomic<bool> tapReq{false};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    voice::Monitor* monSet = nullptr;   // setVoiceMonitor; else voice::monitor("dstar") while not silent
    voice::CallInfo playing;            // the call whose voice is in aud8
    voice::Monitor* activeMonitor() { return monSet ? monSet : silent.load() ? nullptr : &voice::monitor("dstar"); }
    DstarTelemetry pub;

    double rate = 0, offset = 0;
    double power = 0; int64_t nPower = 0, nIn = 0;
    double nextReport = 0;

    // ------------------------------------------------------------ front end (as the M17 receiver's): the spectrum as it comes, and mirrored
    struct Front {
        Impl* m = nullptr;
        int idx = 0;
        int D = 1;
        double fd = 0;
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
        float box[kSps] = {}; int boxPos = 0; float boxSum = 0;
        std::vector<float> ybuf = std::vector<float>(kYMask + 1, 0.f);
        int64_t yN = 0;

        struct Cand { int64_t n = -1; float r = 0; };
        Cand hc, dc_;
        struct SyncSeen { int64_t n; int pol; };
        std::vector<SyncSeen> syncs;
        float yAt(int64_t i) const { return ybuf[(size_t)(i & kYMask)]; }
        void init(double rate, double offset) {
            D = std::max(1, (int)std::floor(rate / 100000.0));
            fd = rate > 0 ? rate / D : 0;
            lo = 1; loCount = 0;
            loStep = rate > 0 ? std::polar(1.0, -2 * kPi * offset / rate) : std::complex<double>(1, 0);
            cic.reset();
            decCount = 0;
            dbuf.assign(8192, cf32(0, 0)); dN = 0;
            rstep = fd / kFs;
            M = std::max(8, (int)std::ceil(5.5 * fd / 5000.0 / 2));
            fir.assign((size_t)nPh * 2 * M, 0.f);
            // the channel filter: +-4.5 kHz holds the GMSK signal (+-3 kHz) with a 2 kHz tuning error; the +-7 kHz used before let in noise that
            // the discriminator turns into clicks (about 2 dB at the threshold)
            const double fc = 4500.0 / std::max(1.0, fd);
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
            std::fill(box, box + kSps, 0.f); boxPos = 0; boxSum = 0;
            std::fill(ybuf.begin(), ybuf.end(), 0.f); yN = 0;
            hc = Cand(); dc_ = Cand(); syncs.clear();
        }

        void input(std::complex<double> x, double inv) {
            const std::complex<double> v = x * lo;
            lo *= loStep;
            if (++loCount >= 4096) { loCount = 0; lo /= std::abs(lo); }
            cic.integrate(v);
            if (++decCount >= D) {
                decCount = 0;
                const std::complex<double> c = cic.comb();
                pushDecimated(cf32((float)(c.real() * inv), (float)(c.imag() * inv)));
            }
        }

        void pushDecimated(cf32 z) {
            dbuf[(size_t)(dN & 8191)] = z;
            dN++;
            while (dN > (int64_t)std::floor(rp) + M) {
                const int64_t base = (int64_t)std::floor(rp);
                const double frac = rp - (double)base;
                const int ph = std::min(nPh - 1, (int)std::lround(frac * nPh));
                const float* h = &fir[(size_t)ph * 2 * M];
                cf32 acc(0, 0);
                const int64_t i0 = base - M + 1;
                for (int k = 0; k < 2 * M; k++) acc += dbuf[(size_t)((i0 + k) & 8191)] * h[k];
                rp += rstep;
                demod(acc);
            }
        }

        float pwr = 0;               // the channel power, averaged over about 10 ms
        void demod(cf32 z) {
            pwr += (std::norm(z) - pwr) * (1.f / 480.f);
            const cf32 d = z * std::conj(prevZ);
            prevZ = z;
            const float f = (float)(std::atan2(d.imag(), d.real()) * kFs / (2 * kPi));
            boxSum += f - box[boxPos];
            box[boxPos] = f;
            boxPos = (boxPos + 1) % kSps;
            if ((yN & 4095) == 0) { boxSum = 0; for (float v : box) boxSum += v; }   // no drift of the running sum
            ybuf[(size_t)(yN & kYMask)] = boxSum / kSps;
            yN++;
            m->onSample(idx);
        }

    };
    Front fr[2];
    int lp = 0;                      // the front end the call is read from

    // ------------------------------------------------------------ bits
    enum Mode { Search, Hdr, Frames } mode = Search;
    double p = 0;                    // the next bit's centre (sample index)
    double dc = 0, g = 1;            // offset (Hz) and level (Hz) of the bits
    int pol = 1, dprev = -1;
    float hsoft[dstar::kHeaderBits];
    int nb = 0;
    uint8_t fb[dstar::kFrameBits];
    float fz[dstar::kFrameBits];
    int fi = 0;                      // frame number since the header (0 carries the data sync)
    int syncMissRun = 0, weakRun = 0;
    double callPwr = 0;              // the channel power of the call's good frames (the carrier stopping is a drop of it)
    double lastAny = -10;
    double snrAvg = 0, berAvg = 0;

    // ------------------------------------------------------------ the call
    DstarTelemetry tel;
    bool txOn = false;
    uint64_t hdrErr = 0, hdrBits = 0, syncErr = 0, syncBits = 0;
    uint8_t half1[3] = {}; bool half1Ok = false;
    char text[20]; int textMask = 0; bool textLogged = false;
    std::string gpsBuf;
    std::vector<uint8_t> hdrBuf;
    std::string lastPosCall; double lastPosLat = 1e9, lastPosLon = 1e9, lastPosSec = -1e9;
    std::unique_ptr<DstarAmbe> ambe;
    std::unique_ptr<AudioOut> audioOut;
    std::vector<float> aud8, pcmOut;
    std::vector<std::vector<uint8_t>> pend;   // the voice of weak frames, held back
    float upPrev = 0;

    double sigSec() const { return (double)fr[0].yN / kFs; }
    float yAt(int64_t i) const { return fr[lp].yAt(i); }
    float yAtF(double t) const {
        const int64_t i = (int64_t)std::floor(t);
        const float f = (float)(t - (double)i);
        return yAt(i) * (1 - f) + yAt(i + 1) * f;
    }

    // ------------------------------------------------------------ acquisition

    void onSample(int k) {
        Front& F = fr[k];
        const int64_t n = F.yN - 1;
        if (mode != Search) {
            if (k != lp) return;
            while (p + 6 <= (double)n && mode != Search) readBit();
            return;
        }
        using Cand = Front::Cand;
        // the bit sync and the frame sync: a header follows
        if (n >= (int64_t)(kHdrSyncLen * kSps)) {
            float v[kHdrSyncLen];
            for (int i = 0; i < kHdrSyncLen; i++) v[i] = F.yAt(n - (int64_t)(kHdrSyncLen - 1 - i) * kSps);
            const float r = pearson(v, gHdrT, kHdrSyncLen);
            if (std::fabs(r) >= 0.8f) {
                if (F.hc.n >= 0 && n - F.hc.n <= 4 * kSps) { if (std::fabs(r) > std::fabs(F.hc.r)) F.hc = {n, r}; }
                else F.hc = {n, r};
            }
            if (F.hc.n >= 0 && n - F.hc.n > 4 * kSps) {
                const Cand c = F.hc;
                F.hc = Cand();
                lp = k;
                if (startHeader(c.n)) return;
            }
        }
        // the data sync: two of them 420 ms apart are a call already going on
        if (n >= 24 * kSps) {
            float v[24];
            for (int i = 0; i < 24; i++) v[i] = F.yAt(n - (int64_t)(23 - i) * kSps);
            const float r = pearson(v, gDataT, 24);
            if (std::fabs(r) >= 0.85f) {
                if (F.dc_.n >= 0 && n - F.dc_.n <= 4 * kSps) { if (std::fabs(r) > std::fabs(F.dc_.r)) F.dc_ = {n, r}; }
                else F.dc_ = {n, r};
            }
            if (F.dc_.n >= 0 && n - F.dc_.n > 4 * kSps) {
                const Cand c = F.dc_;
                F.dc_ = Cand();
                const int cp = c.r > 0 ? 1 : -1;
                bool late = false;
                for (const auto& s : F.syncs) if (s.pol == cp && std::llabs(c.n - s.n - kSuperSamples) <= 4) late = true;
                F.syncs.erase(std::remove_if(F.syncs.begin(), F.syncs.end(), [&](const Front::SyncSeen& s) { return c.n - s.n > kSuperSamples + 50; }), F.syncs.end());
                F.syncs.push_back({c.n, cp});
                if (late) { lp = k; startLate(c.n); }
            }
        }
    }

    // level and offset from the sync bits; false when they do not look like a D-STAR signal
    bool levels(int64_t nLast, const int8_t* t, int len) {
        float v[kHdrSyncLen];
        for (int i = 0; i < len; i++) v[i] = yAt(nLast - (int64_t)(len - 1 - i) * kSps);
        double gg, dd;
        lsFit(v, t, len, gg, dd);
        if (std::fabs(gg) < 300 || std::fabs(gg) > 2500) return false;   // 1.2 kHz peak deviation; about 0.7 of it on alternating bits
        // the bits themselves must be the pattern (a correlation peak on the bit sync alone, a bit or two off, is not)
        int err = 0;
        for (int i = 0; i < len; i++) err += ((v[i] - dd) * gg > 0) != (t[i] > 0);
        if (err > 2) return false;
        pol = gg > 0 ? 1 : -1;
        g = std::fabs(gg); dc = dd;
        tel.deviationHz = (float)g;
        tel.inverted = (pol < 0) != (lp == 1);
        return true;
    }

    bool startHeader(int64_t cn) {
        if (!levels(cn, gHdrT, kHdrSyncLen)) return false;
        p = (double)cn + kSps;
        dprev = -1;   // the frame sync ends on a 0
        nb = 0;
        mode = Hdr;
        lastAny = sigSec();
        return true;
    }

    void startLate(int64_t cn) {
        if (!levels(cn, gDataT, 24)) return;
        p = (double)cn + kSps;
        dprev = -1;   // the data sync ends on a 0
        if (txOn) endTx(false);
        beginTx((double)(cn - (int64_t)(dstar::kFrameBits * kSps)) / kFs);
        tel.now.lateEntry = true; tel.lateEntries++;
        tel.now.syncOk = 2;
        fi = 1; nb = 0; syncMissRun = 0; weakRun = 0;
        mode = Frames;
        lastAny = sigSec();
        if (log) log("D-STAR: a call already going on (late entry)");
    }

    // ------------------------------------------------------------ bits

    void readBit() {
        const float y = yAtF(p);
        const float z = (float)((y - dc) * pol / g);
        const int d = z >= 0 ? 1 : -1;
        if (d != dprev) {   // zero-crossing timing: halfway between two different bits the signal should be at the offset
            const float mid = (float)((yAtF(p - kSps / 2.0) - dc) * pol / g);
            const float e = std::max(-1.f, std::min(1.f, mid * d));
            p -= 0.6 * e;
        }
        dprev = d;
        dc += 0.003 * ((y - dc) - pol * g * d);
        g += 0.003 * (std::fabs(y - dc) - g);
        g = std::max(g, 50.0);
        p += kSps;
        if (mode == Hdr) {
            hsoft[nb++] = std::max(-2.f, std::min(2.f, z));
            if (nb == dstar::kHeaderBits) onHeader();
            return;
        }
        fb[nb] = d > 0; fz[nb] = z; nb++;
        if (nb == dstar::kFrameBits) { nb = 0; onFrame(); }
    }

    void onHeader() {
        uint8_t h[dstar::kHeaderBytes];
        const int err = dstar::headerDecode(hsoft, h);
        dstar::Header hd;
        const bool ok = dstar::headerUnpack(h, hd);
        if (txOn) endTx(false);
        beginTx(p / kFs - (double)(dstar::kHeaderBits + 15 + 64) * kSps / kFs);
        hdrErr = (uint64_t)err; hdrBits = dstar::kHeaderBits;
        if (ok) { tel.headersOk++; applyHeader(hd, false); }
        else tel.headersBad++;
        fi = 0; nb = 0; syncMissRun = 0; weakRun = 0;
        mode = Frames;
        updateBer();
    }

    void onFrame() {
        int endErr = 0;
        for (int i = 0; i < 48; i++) endErr += fb[i] != dstar::kEndPattern[i];
        if (endErr <= 4) { tel.endPatterns++; endTx(true); return; }
        // the quality of this frame
        double m = 0, v = 0;
        for (int i = 0; i < dstar::kFrameBits; i++) m += std::fabs(fz[i]);
        m /= dstar::kFrameBits;
        for (int i = 0; i < dstar::kFrameBits; i++) { const double e = std::fabs(fz[i]) - m; v += e * e; }
        v /= dstar::kFrameBits;
        const double snr = 10 * std::log10(m * m / std::max(v, 1e-9));
        snrAvg += 0.1 * (snr - snrAvg);
        tel.snrDb = (float)snrAvg;
        tel.cfoHz = lp == 1 ? -dc : dc;
        // a frame that does not look like bits: three in a row end the call, and the two before are taken back; the slow data of a weak
        // frame is not used
        // the carrier gone: the discriminator gives noise, or (in the narrow channel filter, where noise looks almost like bits) the
        // power falls to a quarter of the call's
        const double pw = fr[lp].pwr;
        const bool weak = snr < 4 || m > 1.8 || m < 0.4 || (callPwr > 0 && pw < 0.25 * callPwr);
        if (!weak) callPwr = callPwr > 0 ? callPwr + 0.05 * (pw - callPwr) : pw;
        if (weak) {
            if (++weakRun >= 3) {
                pend.clear();
                const uint32_t back = std::min<uint32_t>(2, tel.now.voiceFrames);
                tel.now.voiceFrames -= back; tel.voiceFrames -= back; tel.blocksOk -= back;
                tel.now.durSec = tel.now.voiceFrames * 0.02;
                endTx(false);
                return;
            }
            half1Ok = false;
        } else weakRun = 0;
        // voice
        tel.voiceFrames++; tel.blocksOk++;
        tel.now.voiceFrames++;
        tel.now.durSec = tel.now.voiceFrames * 0.02;
        if (weak) pend.emplace_back(fb, fb + dstar::kVoiceBits);   // held back until a good frame shows the call goes on
        else { for (auto& q : pend) voice(q.data()); pend.clear(); voice(fb); }
        lastAny = sigSec();
        const int idx = fi % dstar::kSuperframe;
        fi++;
        if (idx == 0) {
            int e = 0;
            for (int i = 0; i < 24; i++) e += fb[72 + i] != dstar::kDataSync[i];
            syncErr += (uint64_t)std::min(e, 12); syncBits += 24;
            updateBer();
            half1Ok = false;
            if (e <= 6) { tel.now.syncOk++; syncMissRun = 0; }
            else {
                tel.now.syncMiss++; tel.blocksBad++;
                if (++syncMissRun >= 2) { endTx(false); return; }
            }
            return;
        }
        if (weak) return;
        uint8_t b[3];
        dstar::bitsToBytes(fb + 72, 3, b);
        for (int k = 0; k < 3; k++) b[k] ^= dstar::kSlowScramble[k];
        if (((idx - 1) & 1) == 0) { memcpy(half1, b, 3); half1Ok = true; return; }
        if (!half1Ok) return;
        half1Ok = false;
        const uint8_t blk[6] = {half1[0], half1[1], half1[2], b[0], b[1], b[2]};
        slowBlock(blk);
    }

    void updateBer() {
        const uint64_t bits = hdrBits + syncBits;
        tel.now.berPct = bits ? (float)(100.0 * (double)(hdrErr + syncErr) / (double)bits) : 0.f;
        berAvg += 0.2 * (tel.now.berPct - berAvg);
        tel.berPct = (float)berAvg;
    }

    // ------------------------------------------------------------ slow data

    void slowBlock(const uint8_t b[6]) {
        const int type = b[0] >> 4;
        if (type == 0x4) {
            const int i = b[0] & 0x0F;
            if (i > 3) return;
            memcpy(text + 5 * i, b + 1, 5);
            textMask |= 1 << i;
            tel.now.textBlocks = textMask;
            std::string s;
            for (int k = 0; k < 4; k++) s += (textMask & (1 << k)) ? std::string(text + 5 * k, 5) : std::string(5, ' ');
            tel.now.text = trimmed(s);
            if (textMask == 15 && !textLogged && !tel.now.text.empty()) {
                textLogged = true;
                DstarMessage msg; msg.wallTime = tel.now.wallTime; msg.my = tel.now.my; msg.ur = tel.now.ur; msg.text = tel.now.text;
                tel.messages.insert(tel.messages.begin(), msg);
                if (tel.messages.size() > kMaxMsgs) tel.messages.pop_back();
                if (log) log("D-STAR message from " + (msg.my.empty() ? std::string("?") : msg.my) + ": " + msg.text);
            }
        } else if (type == 0x3) {
            const int len = std::min(5, b[0] & 0x0F);
            for (int k = 0; k < len; k++) {
                const char c = (char)b[1 + k];
                if (c == '$' && !gpsBuf.empty() && gpsBuf != "$") { gpsLine(gpsBuf); gpsBuf.clear(); }   // a new sentence: the last one lost its end in a missing block
                if (c == '\r' || c == '\n') { if (!gpsBuf.empty()) gpsLine(gpsBuf); gpsBuf.clear(); }
                else if (gpsBuf.size() < 200) gpsBuf += c;
                else gpsBuf.clear();
            }
        } else if (type == 0x5) {
            const int len = std::min(5, b[0] & 0x0F);
            hdrBuf.insert(hdrBuf.end(), b + 1, b + 1 + len);
            if (hdrBuf.size() > 2 * (size_t)dstar::kHeaderBytes) hdrBuf.erase(hdrBuf.begin(), hdrBuf.end() - dstar::kHeaderBytes);
            if (hdrBuf.size() >= (size_t)dstar::kHeaderBytes) {
                dstar::Header hd;
                if (dstar::headerUnpack(hdrBuf.data() + hdrBuf.size() - dstar::kHeaderBytes, hd)) {
                    hdrBuf.clear();
                    if (!tel.now.headerOk) { tel.slowHeaders++; applyHeader(hd, true); }
                }
            }
        } else if (type == 0xC) {
            tel.now.squelch = b[1];
        }
    }

    void gpsLine(const std::string& line) {
        dstar::Fix f;
        const bool ok = line.compare(0, 2, "$$") == 0 ? dstar::parseDprs(line, f) : dstar::parseNmea(line, f);
        tel.now.gps = trimmed(line);
        if (!ok) {
            // a sentence without a fix ($GPGSV ...) is not an error: only the checksum or the format
            if (line.size() > 6 && (line.compare(3, 3, "RMC") == 0 || line.compare(3, 3, "GGA") == 0 || line.compare(0, 2, "$$") == 0)) tel.gpsBad++;
            return;
        }
        tel.gpsLines++;
        tel.now.hasPos = true; tel.now.lat = f.lat; tel.now.lon = f.lon;
        const std::string call = f.aprs ? f.call : tel.now.my;
        const double now = sigSec();
        // GGA and RMC of one fix come together: merge into the last position of this station when it has not moved
        if (!tel.positions.empty() && tel.positions[0].call == call && std::fabs(tel.positions[0].lat - f.lat) < 1e-5 && std::fabs(tel.positions[0].lon - f.lon) < 1e-5 && now - lastPosSec < 30) {
            DstarPosition& q = tel.positions[0];
            if (f.altValid) { q.altM = f.altM; q.altValid = true; }
            if (f.velValid) { q.speedKmh = f.speedKmh; q.course = f.course; q.velValid = true; }
            return;
        }
        if (call == lastPosCall && std::fabs(f.lat - lastPosLat) < 1e-5 && std::fabs(f.lon - lastPosLon) < 1e-5 && now - lastPosSec < 30) return;
        DstarPosition q;
        q.wallTime = (int64_t)time(nullptr); q.call = call; q.lat = f.lat; q.lon = f.lon; q.altM = f.altM; q.altValid = f.altValid;
        q.speedKmh = f.speedKmh; q.course = f.course; q.velValid = f.velValid; q.dprs = f.aprs; q.comment = f.comment;
        tel.positions.insert(tel.positions.begin(), q);
        if (tel.positions.size() > kMaxPos) tel.positions.pop_back();
        lastPosCall = call; lastPosLat = f.lat; lastPosLon = f.lon; lastPosSec = now;
    }

    // ------------------------------------------------------------ the call

    void beginTx(double startSec) {
        txOn = true;
        tel.now = DstarCall();
        tel.now.active = true;
        tel.now.wallTime = (int64_t)time(nullptr);
        tel.now.startSec = startSec;
        hdrErr = hdrBits = syncErr = syncBits = 0;
        callPwr = 0;
        half1Ok = false;
        memset(text, ' ', sizeof text); textMask = 0; textLogged = false;
        gpsBuf.clear(); hdrBuf.clear();
        ambe.reset();
    }

    void applyHeader(const dstar::Header& h, bool fromSlow) {
        DstarCall& n = tel.now;
        n.headerOk = true; n.headerFromSlow = fromSlow;
        memcpy(n.flag, h.flag, 3);
        n.flags = dstar::flagText(h.flag);
        n.my = h.my; n.sfx = h.sfx; n.ur = h.ur; n.rpt1 = h.rpt1; n.rpt2 = h.rpt2;
        for (auto& m : tel.messages) if (m.wallTime == n.wallTime && m.my.empty()) { m.my = n.my; m.ur = n.ur; }
        if (log) {
            char b[200];
            snprintf(b, sizeof b, "D-STAR: %s%s%s -> %s via %s / %s%s", n.my.c_str(), n.sfx.empty() ? "" : "/", n.sfx.c_str(), n.ur.c_str(), n.rpt1.c_str(), n.rpt2.c_str(),
                     fromSlow ? " (header from the slow data)" : "");
            log(b);
        }
    }

    void endTx(bool endSeen) {
        mode = Search;
        for (auto& f : fr) { f.hc = Front::Cand(); f.dc_ = Front::Cand(); f.syncs.clear(); }
        if (!txOn) return;
        txOn = false;
        DstarCall& n = tel.now;
        n.active = false; n.endSeen = endSeen;
        for (auto& q : pend) voice(q.data());
        pend.clear();
        flushAudio();
        if (!n.headerOk && n.syncOk == 0) return;   // nothing that checked: not a call
        tel.log.insert(tel.log.begin(), n);
        if (tel.log.size() > kDstarLogMax) tel.log.pop_back();
    }

    // ------------------------------------------------------------ voice

    void voice(const uint8_t* bits) {
        if (vtap) vtap(bits);
        voice::Monitor* mon = activeMonitor();
        if (mon) {   // the voice monitor (voice_monitor.h) lists the call and says whether it is played
            voice::CallInfo ci;
            ci.srcText = tel.now.my; if (!tel.now.sfx.empty() && tel.now.sfx != "    ") ci.srcText += "/" + tel.now.sfx;
            ci.dstText = tel.now.ur; ci.group = true;
            ci.codecMissing = !tel.ambe;
            ci.errorRate = tel.now.berPct > 0 ? tel.now.berPct / 100.f : -1.f;
            if (!tel.now.rpt1.empty()) ci.access = "RPT " + tel.now.rpt1 + (tel.now.rpt2.empty() ? "" : " / " + tel.now.rpt2);
            if (mon->offer(ci) != voice::Verdict::Play) return;
            playing = ci;
        }
        if (!tel.ambe || (silent.load() && !tap)) return;
        if (!ambe) ambe = std::make_unique<DstarAmbe>();
        if (!ambe->ok()) return;
        int16_t pcm[160];
        ambe->decode(bits, pcm);
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

    // The call log, messages and positions stay through resets: Clear and a loaded file reach the feed thread's copy through these requests (mu), and the
    // published report at once
    bool clearReq = false, loadReq = false;
    DstarTelemetry loaded;
    double loadedAt = 0;                   // the report's clock the loaded lists are on
    static void moveLists(DstarTelemetry& to, DstarTelemetry& from) {
        to.log = std::move(from.log); to.messages = std::move(from.messages); to.positions = std::move(from.positions);
        from.log.clear(); from.messages.clear(); from.positions.clear();
    }
    static void shiftTimes(DstarTelemetry& t, double dt) {
        for (auto& c : t.log) c.startSec += dt;
    }
    void takeRequests() {   // feed thread
        std::lock_guard<std::mutex> lk(mu);
        if (clearReq) { DstarTelemetry e; moveLists(tel, e); clearReq = false; }
        if (loadReq) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); loadReq = false; }
    }
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        if (txOn) endTx(false);
        for (int k = 0; k < 2; k++) { fr[k].m = this; fr[k].idx = k; fr[k].init(rate, offset); }
        mode = Search; lp = 0;
        power = 0; nPower = 0; nIn = 0; nextReport = 0.25 * rate;
        lastAny = -10; snrAvg = 0; berAvg = 0;
        const uint64_t seq = tel.seq;
        DstarTelemetry keep;                          // the lists stay (clearResults): the clock starts again, they move back by the time it had reached
        moveLists(keep, tel);
        shiftTimes(keep, -tel.timeSec);
        tel = DstarTelemetry(); tel.seq = seq;
        moveLists(tel, keep);
        tel.inputRate = rate;
        tel.ambe = dstarAmbeAvailable(&tel.ambePath);
    }

    void report() {
        if (tapReq.exchange(false)) { std::lock_guard<std::mutex> lk(mu); tap = tapSet; vtap = vtapSet; }
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        if (!tel.ambe && tel.seq % 40 == 0) tel.ambe = dstarAmbeAvailable(&tel.ambePath);   // installed while running ("Look again")
        tel.state = txOn ? 2 : sigSec() - lastAny < 2 ? 1 : 0;
        tel.dataValid = txOn;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

DstarReceiver::DstarReceiver() : p_(std::make_unique<Impl>()) {}
DstarReceiver::~DstarReceiver() = default;

void DstarReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void DstarReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
bool DstarReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= dstarTuning().minSampleRate - 1; }
void DstarReceiver::reset() { p_->resetReq = true; }

void DstarReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    m.takeRequests();
    if (m.tapReq.load()) { std::lock_guard<std::mutex> lk(m.mu); m.tap = m.tapSet; m.vtap = m.vtapSet; m.tapReq = false; }
    if (m.rate <= 0 || n == 0) return;
    const double inv = 1.0 / ((double)m.fr[0].D * m.fr[0].D * m.fr[0].D);
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
        m.fr[0].input(std::complex<double>(re, im), inv);
        m.fr[1].input(std::complex<double>(re, -im), inv);   // the mirrored spectrum (I and Q swapped, or an inverted tuner)
        m.nIn++;
        if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); }
    }
    m.nPower += (int64_t)n;
}

void DstarReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    DstarTelemetry e;
    Impl::moveLists(p_->pub, e);
    p_->clearReq = true; p_->loadReq = false;
    p_->pub.seq++;
}
std::string DstarReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.log, p_->pub.messages, p_->pub.positions);
}
bool DstarReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    DstarTelemetry l;
    if (!results::unpack(blob, t0, l.log, l.messages, l.positions)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    DstarTelemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->loadReq = true;
    p_->pub.seq++;
    return true;
}
bool DstarReceiver::telemetry(DstarTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void DstarReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void DstarReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void DstarReceiver::setMuted(bool m) { p_->muted = m; }
void DstarReceiver::setSilent(bool s) { p_->silent = s; }
void DstarReceiver::setVoiceMonitor(voice::Monitor* m) { p_->monSet = m; }
void DstarReceiver::setAudioTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tapSet = std::move(cb); p_->tapReq = true; }
void DstarReceiver::setVoiceTap(std::function<void(const uint8_t bits[72])> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->vtapSet = std::move(cb); p_->tapReq = true; }

ModeTuning dstarTuning() {
    ModeTuning t;
    t.stdMode = 35; t.id = "dstar"; t.name = "D-STAR";
    t.minMhz = 28; t.maxMhz = 1300; t.defMhz = 145.375;   // the 2 m digital voice calling channel of the IARU Region 1 band plan
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.00625;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike
    return t;
}

} // namespace dect2

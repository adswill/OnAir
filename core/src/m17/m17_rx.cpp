// M17 receiver (see m17_rx.h).
#include "dect2/m17/m17_rx.h"
#include "dect2/dsp/cic3.h"
#include "dect2/results/results_io.h"
#include "dect2/m17/m17_tel.h"
#include "dect2/m17/m17_proto.h"
#include "dect2/m17/m17_codec2.h"
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
constexpr double kFs = 48000;            // demodulator rate, 10 samples per symbol
constexpr int kSps = 10;
constexpr int kFrame = 192 * kSps;       // samples per frame
constexpr int kYMask = 16383;
constexpr size_t kMaxMsgs = 200, kMaxPos = 500;
enum FrameType { FtLsf, FtStream, FtPacket, FtBert };

int8_t gSync[4][8], gEot[8];
struct SyncInit {
    SyncInit() {
        m17::syncSymbols(m17::kSyncLsf, gSync[FtLsf]); m17::syncSymbols(m17::kSyncStream, gSync[FtStream]);
        m17::syncSymbols(m17::kSyncPacket, gSync[FtPacket]); m17::syncSymbols(m17::kSyncBert, gSync[FtBert]);
        m17::syncSymbols(m17::kSyncEot, gEot);
    }
} gSyncInit;

float pearson(const float* v, const int8_t* s, int n) {
    double mv = 0, ms = 0;
    for (int i = 0; i < n; i++) { mv += v[i]; ms += s[i]; }
    mv /= n; ms /= n;
    double c = 0, a = 0, b = 0;
    for (int i = 0; i < n; i++) { const double x = v[i] - mv, y = s[i] - ms; c += x * y; a += x * x; b += y * y; }
    return a > 0 && b > 0 ? (float)(c / std::sqrt(a * b)) : 0.f;
}
inline float level(float z) { return z >= 2 ? 3.f : z >= 0 ? 1.f : z >= -2 ? -1.f : -3.f; }
std::string printable(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) if (p[i] >= 32 && p[i] < 127) s += (char)p[i];
    return s;
}
} // namespace

struct M17Receiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true};
    std::function<void(const std::string&)> log;
    std::function<void(const float*, size_t)> tapSet, tap;
    std::atomic<bool> tapReq{false};
    std::atomic<int> canFilter{-1};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    voice::Monitor* monSet = nullptr;   // setVoiceMonitor; else voice::monitor("m17") while not silent
    voice::CallInfo playing;            // the call whose voice is in aud8
    voice::Monitor* activeMonitor() { return monSet ? monSet : silent.load() ? nullptr : &voice::monitor("m17"); }
    M17Telemetry pub;

    double rate = 0, offset = 0;
    double power = 0; int64_t nPower = 0, nIn = 0;
    double nextReport = 0;

    // One demodulator: the spectrum as it comes, or mirrored (I and Q swapped), run side by side until one of them decodes
    struct Path {
        Impl* m = nullptr;
        bool mirror = false;
        // front end
        int D = 1;
        double fd = 0;
        std::complex<double> lo{1, 0}, loStep{1, 0};
        int loCount = 0;
        Cic3 cic;   // the decimator (dsp/cic3.h: exact for as long as it runs)
        int decCount = 0;
        std::vector<cf32> dbuf;          // decimated samples, ring
        int64_t dN = 0;                  // decimated samples so far
        double rp = 0, rstep = 0;        // resampler position (decimated index) and step
        int M = 0, nPh = 64;
        std::vector<float> fir;          // nPh * 2M
        cf32 prevZ{0, 0};
        std::vector<float> rrc, rrcHist;
        int rrcPos = 0;
        std::vector<float> ybuf = std::vector<float>(kYMask + 1, 0.f);
        int64_t yN = 0;

        // frames
        struct Cand { int64_t S; float r; int pair; };
        std::vector<Cand> cands;
        bool locked = false;
        int64_t nextS = 0;
        int expect = FtStream, fails = 0;
        int polarity = 0;                // 0 unknown, +1 normal, -1 mirrored
        double lastFrameSec = -10;
        float yAt(int64_t i) const { return ybuf[(size_t)(i & kYMask)]; }
        double sigSec() const { return (double)yN / kFs; }
        void init(double rate, double offset) {
            D = std::max(1, (int)std::floor(rate / 100000.0));
            fd = rate > 0 ? rate / D : 0;
            lo = 1; loCount = 0;
            loStep = rate > 0 ? std::polar(1.0, -2 * kPi * offset / rate) : std::complex<double>(1, 0);
            cic.reset();
            decCount = 0;
            dbuf.assign(8192, cf32(0, 0)); dN = 0;
            rstep = fd / kFs;
            // the channel filter: windowed sinc, 8 kHz cut, about 5 kHz transition
            M = std::max(8, (int)std::ceil(5.5 * fd / 5000.0 / 2));
            fir.assign((size_t)nPh * 2 * M, 0.f);
            const double fc = 8000.0 / fd;
            for (int ph = 0; ph < nPh; ph++) {
                const double frac = (double)ph / nPh;
                double sum = 0;
                for (int k = 0; k < 2 * M; k++) {
                    const double t = (double)(k - M + 1) - frac;   // tap k multiplies x[floor(p) - M + 1 + k]; t = its distance from p
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
            rrc = m17::rrcTaps(kSps, 8);
            double s = 0; for (float v : rrc) s += v;
            for (auto& v : rrc) v = (float)(v / s);   // unit gain at DC: the output is in Hz
            rrcHist.assign(rrc.size(), 0.f); rrcPos = 0;
            std::fill(ybuf.begin(), ybuf.end(), 0.f); yN = 0;
            cands.clear(); locked = false; fails = 0; polarity = 0;
            lastFrameSec = -10;
        }
        // one input sample (already conjugated on the mirrored path)
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

        // ------------------------------------------------------------ front end

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

        void demod(cf32 z) {
            const cf32 d = z * std::conj(prevZ);
            prevZ = z;
            const float f = (float)(std::atan2(d.imag(), d.real()) * kFs / (2 * kPi));
            rrcHist[(size_t)rrcPos] = f;
            rrcPos = (rrcPos + 1) % (int)rrc.size();
            float y = 0;
            const int L = (int)rrc.size();
            for (int k = 0; k < L; k++) y += rrc[(size_t)k] * rrcHist[(size_t)((rrcPos + k) % L)];
            ybuf[(size_t)(yN & kYMask)] = y;
            yN++;
            onSample();
        }

        // ------------------------------------------------------------ frames

        struct Fit { float z[192]; double g = 0, d = 0, var = 1e30; };

        // level and offset from the sync symbols, then twice from all the decided symbols
        void fit(int64_t S, const int8_t* sync, Fit& f) {
            float v[192];
            for (int i = 0; i < 192; i++) v[i] = yAt(S + (int64_t)i * kSps);
            auto ls = [&](const float* s, int n, double& g, double& d) {
                double ms = 0, mv = 0;
                for (int i = 0; i < n; i++) { ms += s[i]; mv += v[i]; }
                ms /= n; mv /= n;
                double c = 0, a = 0;
                for (int i = 0; i < n; i++) { c += (s[i] - ms) * (v[i] - mv); a += (s[i] - ms) * (s[i] - ms); }
                g = a > 0 ? c / a : 0; d = mv - g * ms;
            };
            float s[192];
            for (int i = 0; i < 8; i++) s[i] = sync[i];
            ls(s, 8, f.g, f.d);
            for (int it = 0; it < 2; it++) {
                if (std::fabs(f.g) < 1e-6) return;
                for (int i = 8; i < 192; i++) s[i] = level((float)((v[i] - f.d) / f.g));
                ls(s, 192, f.g, f.d);
            }
            if (std::fabs(f.g) < 1e-6) return;
            double e = 0;
            for (int i = 0; i < 192; i++) { const double r = v[i] - f.g * s[i] - f.d; e += r * r; }
            f.var = e / 192;
            for (int i = 0; i < 192; i++) f.z[i] = (float)((v[i] - f.d) / f.g);
        }

        // decode the frame at S as m->type t; acq: a stricter check (no frame expected here)
        bool tryFrame(int64_t S, int t, bool acq) {
            Fit f;
            fit(S, gSync[t], f);
            const int pol = f.g > 0 ? 1 : -1;
            if (polarity && pol != polarity) return false;
            const double dev = std::fabs(f.g) * 3;
            if (dev < 1000 || dev > 5000) return false;   // 2.4 kHz nominal
            float soft[368];
            m17::softBits(f.z + 8, soft);
            const float snr = (float)(10 * std::log10(5 * f.g * f.g / std::max(f.var, 1e-9)));
            if (acq && snr < 4) return false;
            const Path& other = m->paths[mirror ? 0 : 1];
            if (other.locked || sigSec() - other.lastFrameSec < 3) return false;   // the other reading of the spectrum is decoding
            bool ok = false;
            int corr = 0, bits = 368;
            if (t == FtLsf) {
                uint8_t lsf[30];
                ok = m17::decodeLsf(soft, lsf, corr);
                if (ok) m->onLsf(lsf, S, false);
            } else if (t == FtStream) {
                uint8_t lich[6], pl[16]; int ge; uint16_t fn;
                const bool lichOk = m17::decodeStream(soft, lich, ge, fn, pl, corr);
                bits = 368; corr += ge;
                ok = acq ? (lichOk && ge <= 6 && corr <= 40) : corr <= 70;
                if (ok) m->onStream(lich, lichOk, fn, pl, S);
            } else if (t == FtPacket) {
                uint8_t ch[25]; bool eof; int cnt;
                m17::decodePacket(soft, ch, eof, cnt, corr);
                ok = acq ? (corr <= 20 && (eof ? cnt >= 1 && cnt <= 25 : true)) : corr <= 70;
                if (ok) m->onPacket(ch, eof, cnt, S);
            } else {
                uint8_t pr[197];
                m17::decodeBert(soft, pr, corr);
                ok = acq ? polarity != 0 && corr <= 20 : corr <= 70;
                if (ok) { m->tel.bertFrames++; if (!m->txOn) m->beginTx(false, S); m->tel.now.type = "BERT"; m->tel.now.frames++; }
            }
            if (!ok) return false;
            polarity = pol;
            m->tel.inverted = (pol < 0) != mirror;
            m->tel.cfoHz = mirror ? -f.d : f.d;
            m->tel.deviationHz = (float)dev;
            m->tel.snrDb = snr;
            m->tel.blocksOk++;
            m->berAvg += 0.15 * ((double)corr / bits - m->berAvg);
            m->tel.berPct = (float)(100 * m->berAvg);
            m->txCorr += (uint64_t)corr; m->txBits += (uint64_t)bits;
            if (m->txOn) m->tel.now.berPct = m->txBits ? (float)(100.0 * (double)m->txCorr / (double)m->txBits) : 0.f;
            lastFrameSec = (double)S / kFs;
            m->lastAny = std::max(m->lastAny, lastFrameSec);
            return true;
        }

        void onSample() {
            const int64_t n = yN - 1;
            if (locked) {
                if (n < nextS + 191 * kSps + 2) return;
                // the end marker where a frame was due
                float v[8];
                for (int i = 0; i < 8; i++) v[i] = yAt(nextS + (int64_t)i * kSps);
                const float re = pearson(v, gEot, 8) * (float)(polarity ? polarity : 1);
                if (re > 0.85f) { locked = false; if (m->txOn) m->endTx(); return; }
                // the best timing within +-2 samples
                int64_t best = nextS; double bq = 1e30;
                for (int o = -2; o <= 2; o++) {
                    Fit f; fit(nextS + o, gSync[expect], f);
                    const double q = f.var / std::max(f.g * f.g, 1e-12);
                    if (q < bq && f.g * polarity > 0) { bq = q; best = nextS + o; }
                }
                if (tryFrame(best, expect, false)) fails = 0;
                else { fails++; if (m->txOn) m->tel.now.badFrames++; m->tel.blocksBad++; }
                nextS = best + kFrame;
                if (fails >= 3) { locked = false; if (m->txOn) m->endTx(); }
                return;
            }
            // acquisition: a sync burst whose first symbol is at S
            const int64_t S = n - 7 * kSps;
            if (S >= 0) {
                float v[8];
                for (int i = 0; i < 8; i++) v[i] = yAt(S + (int64_t)i * kSps);
                const float ra = pearson(v, gSync[FtLsf], 8), rb = pearson(v, gSync[FtPacket], 8);
                const float r = std::fabs(ra) >= std::fabs(rb) ? ra : rb;
                const int pair = std::fabs(ra) >= std::fabs(rb) ? 0 : 1;
                if (std::fabs(r) >= 0.9f) {
                    if (!cands.empty() && S - cands.back().S <= 5 && cands.back().pair == pair) { if (std::fabs(r) > std::fabs(cands.back().r)) cands.back() = {S, r, pair}; }
                    else { cands.push_back({S, r, pair}); if (cands.size() > 48) cands.erase(cands.begin()); }
                }
            }
            while (!cands.empty() && n >= cands.front().S + 191 * kSps + 2) {
                const Cand c = cands.front();
                cands.erase(cands.begin());
                // the readings this sync allows: pair 0 is LSF or (upside down) stream, pair 1 packet or BERT
                const int a = c.pair == 0 ? FtLsf : FtPacket, b = c.pair == 0 ? FtStream : FtBert;
                const int first = (c.r > 0) == (polarity >= 0) ? a : b, second = first == a ? b : a;
                bool ok = tryFrame(c.S, first, true);
                int got = first;
                if (!ok && polarity == 0) { ok = tryFrame(c.S, second, true); got = second; }
                if (!ok) continue;
                cands.clear();
                locked = true; fails = 0;
                nextS = c.S + kFrame;
                if (got == FtLsf) expect = m17::typeStream(m->type) ? FtStream : FtPacket;
                else expect = got;
                if (!m->txOn) locked = false;   // the frame ended the transmission
                break;
            }
        }

    };
    Path paths[2];
    double lastAny = -10;

    double berAvg = 0;

    // the transmission
    M17Telemetry tel;
    bool txOn = false, lsfKnown = false, typeKnown = false;
    uint16_t type = 0;
    uint8_t lichBuf[30] = {};
    int lichMask = 0;
    std::string textBlocks[4];
    int textGot = 0, textNeed = 0;
    std::vector<uint8_t> pkt;
    int pktNext = 0;
    uint64_t txCorr = 0, txBits = 0;
    int lastFnSeen = -1;
    std::unique_ptr<M17Codec2> c2;
    int c2Rate = 0;
    std::unique_ptr<AudioOut> audioOut;
    std::vector<float> aud8, pcmOut;
    float upPrev = 0;
    std::string lastPosSrc; double lastPosLat = 1e9, lastPosLon = 1e9, lastPosSec = -1e9;

    double sigSec() const { return paths[0].sigSec(); }

    // The transmission log, messages and positions stay through resets: Clear and a loaded file reach the feed thread's copy through these requests (mu), and the
    // published report at once
    bool clearReq = false, loadReq = false;
    M17Telemetry loaded;
    double loadedAt = 0;                   // the report's clock the loaded lists are on
    static void moveLists(M17Telemetry& to, M17Telemetry& from) {
        to.log = std::move(from.log); to.messages = std::move(from.messages); to.positions = std::move(from.positions);
        from.log.clear(); from.messages.clear(); from.positions.clear();
    }
    static void shiftTimes(M17Telemetry& t, double dt) {
        for (auto& c : t.log) c.startSec += dt;
    }
    void takeRequests() {   // feed thread
        std::lock_guard<std::mutex> lk(mu);
        if (clearReq) { M17Telemetry e; moveLists(tel, e); clearReq = false; }
        if (loadReq) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); loadReq = false; }
    }
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        for (int k = 0; k < 2; k++) { paths[k].m = this; paths[k].mirror = k == 1; paths[k].init(rate, offset); }
        power = 0; nPower = 0; nIn = 0; nextReport = 0.25 * rate;
        lastAny = -10;
        if (txOn) endTx();
        const uint64_t seq = tel.seq;
        M17Telemetry keep;                          // the lists stay (clearResults): the clock starts again, they move back by the time it had reached
        moveLists(keep, tel);
        shiftTimes(keep, -tel.timeSec);
        tel = M17Telemetry(); tel.seq = seq;
        moveLists(tel, keep);
        tel.inputRate = rate;
        tel.codec2 = m17Codec2Available(&tel.codec2Path);
    }

    // ------------------------------------------------------------ the transmission

    void beginTx(bool packet, int64_t S) {
        if (txOn) endTx();
        txOn = true; lsfKnown = false; typeKnown = false; type = 0; lichMask = 0;
        for (auto& b : textBlocks) b.clear();
        textGot = textNeed = 0;
        pkt.clear(); pktNext = 0; txCorr = txBits = 0; lastFnSeen = -1;
        c2.reset(); c2Rate = 0;
        tel.now = M17Transmission();
        tel.now.active = true; tel.now.packet = packet;
        tel.now.wallTime = (int64_t)time(nullptr);
        tel.now.startSec = (double)S / kFs;
    }

    void endTx() {
        if (!txOn) return;
        txOn = false;
        tel.now.active = false;
        if (!tel.now.packet && !tel.now.text.empty()) {
            M17Message m; m.wallTime = tel.now.wallTime; m.src = tel.now.src; m.dst = tel.now.dst; m.text = tel.now.text; m.can = tel.now.can; m.sms = false;
            tel.messages.insert(tel.messages.begin(), m);
            if (tel.messages.size() > kMaxMsgs) tel.messages.pop_back();
        }
        tel.log.insert(tel.log.begin(), tel.now);
        if (tel.log.size() > kM17LogMax) tel.log.pop_back();
        flushAudio();
    }

    void setType(uint16_t t) {
        type = t; typeKnown = true;
        tel.now.type = m17::typeText(t);
        tel.now.enc = m17::encText(t);
        tel.now.can = m17::typeCan(t);
    }

    void applyLsf(const m17::Lsf& l, bool fromLich) {
        const bool first = !lsfKnown;
        lsfKnown = true;
        setType(l.type);
        tel.now.src = m17::decodeCallsign(l.src);
        tel.now.dst = m17::decodeCallsign(l.dst);
        if (fromLich && first) { tel.now.lsfFromLich = true; tel.lichLsf++; }
        if (first && log) {
            char b[200];
            snprintf(b, sizeof b, "M17: %s -> %s, %s, CAN %d%s%s%s", tel.now.src.c_str(), tel.now.dst.c_str(), tel.now.type.c_str(), tel.now.can,
                     tel.now.enc.empty() ? "" : ", encrypted (", tel.now.enc.c_str(), tel.now.enc.empty() ? "" : ")");
            log(b);
        }
        if (!m17::typeStream(l.type) || m17::typeEnc(l.type) != 0) return;
        const int sub = m17::typeEncSub(l.type);
        if (sub == 0 && l.meta[0]) {
            const int need = l.meta[0] >> 4, which = l.meta[0] & 15;
            for (int k = 0; k < 4; k++) if (which & (1 << k)) { textBlocks[k] = std::string((const char*)l.meta + 1, 13); textGot |= 1 << k; }
            textNeed = need;
            std::string s;
            for (int k = 0; k < 4; k++) if (need & (1 << k)) s += (textGot & (1 << k)) ? textBlocks[k] : std::string(13, ' ');
            while (!s.empty() && s.back() == ' ') s.pop_back();
            tel.now.text = printable((const uint8_t*)s.data(), s.size());
        } else if (sub == 1) {
            const m17::Gnss g = m17::gnssUnpack(l.meta);
            if (g.posValid) {
                tel.now.hasGnss = true; tel.now.lat = g.lat; tel.now.lon = g.lon;
                const double now = sigSec();
                if (tel.now.src != lastPosSrc || std::fabs(g.lat - lastPosLat) > 1e-5 || std::fabs(g.lon - lastPosLon) > 1e-5 || now - lastPosSec > 30) {
                    M17Position p;
                    p.wallTime = (int64_t)time(nullptr); p.src = tel.now.src; p.lat = g.lat; p.lon = g.lon;
                    p.altM = g.altM; p.altValid = g.altValid; p.speedKmh = g.speedKmh; p.bearing = g.bearing; p.velValid = g.velValid; p.station = g.station;
                    tel.positions.insert(tel.positions.begin(), p);
                    if (tel.positions.size() > kMaxPos) tel.positions.pop_back();
                    lastPosSrc = tel.now.src; lastPosLat = g.lat; lastPosLon = g.lon; lastPosSec = now;
                }
            }
        } else if (sub == 2) {
            uint64_t a = 0, b = 0;
            for (int i = 0; i < 6; i++) { a = a << 8 | l.meta[i]; b = b << 8 | l.meta[6 + i]; }
            tel.now.ext1 = m17::decodeCallsign(a);
            tel.now.ext2 = m17::decodeCallsign(b);
        }
    }

    void onLsf(const uint8_t lsf30[30], int64_t S, bool) {
        m17::Lsf l;
        m17::lsfUnpack(lsf30, l);
        tel.lsfOk++;
        beginTx(!m17::typeStream(l.type), S);
        applyLsf(l, false);
        memcpy(lichBuf, lsf30, 30);
    }

    void onStream(const uint8_t lich[6], bool lichOk, uint16_t fnw, const uint8_t pl[16], int64_t S) {
        if (!txOn) beginTx(false, S);
        tel.streamFrames++;
        tel.now.frames++;
        const int fn = fnw & 0x7FFF;
        tel.now.lastFn = (uint32_t)fn;
        tel.now.durSec = (double)(S + kFrame) / kFs - tel.now.startSec;
        const int cnt = lich[5] >> 5;
        if (lichOk && cnt <= 5) {
            if (cnt == 0) lichMask = 0;   // a superframe starts: the META may have changed since the last one
            memcpy(lichBuf + 5 * cnt, lich, 5);
            lichMask |= 1 << cnt;
            if (!typeKnown && (lichMask & 4)) setType((uint16_t)(lichBuf[12] << 8 | lichBuf[13]));
            if (cnt == 5 && lichMask == 0x3F) {
                m17::Lsf l;
                if (m17::lsfUnpack(lichBuf, l)) applyLsf(l, true);
                lichMask = 0;
            }
        }
        // the payload
        if (typeKnown && m17::typeEnc(type) == 0) {
            const int dt = m17::typeData(type);
            if (dt == 1 || dt == 3) {
                tel.now.data += printable(dt == 1 ? pl : pl + 8, dt == 1 ? 16 : 8);
                if (tel.now.data.size() > 240) tel.now.data.erase(0, tel.now.data.size() - 240);
            }
            const int can = canFilter.load();
            if ((dt == 2 || dt == 3) && (can < 0 || can == m17::typeCan(type))) voice(pl, dt == 3 ? 1600 : 3200, fn);
        } else if (typeKnown && (m17::typeData(type) == 2 || m17::typeData(type) == 3)) {
            if (voice::Monitor* mon = activeMonitor()) { voice::CallInfo ci = callInfo(); ci.encrypted = true; mon->offer(ci); }   // listed, never decoded
        }
        lastFnSeen = fn;
        if (fnw & 0x8000) { for (auto& q : paths) q.locked = false; endTx(); }
    }

    void onPacket(const uint8_t ch[25], bool eof, int cnt, int64_t S) {
        if (!txOn) beginTx(true, S);
        tel.packetFrames++;
        tel.now.frames++;
        tel.now.durSec = (double)(S + kFrame) / kFs - tel.now.startSec;
        if (!eof) {
            if (cnt != pktNext) { pktNext = -100; return; }   // a frame missing: the packet is lost
            pkt.insert(pkt.end(), ch, ch + 25);
            pktNext++;
            return;
        }
        if (pktNext >= 0) pkt.insert(pkt.end(), ch, ch + std::max(1, std::min(25, cnt)));
        const bool ok = pktNext >= 0 && pkt.size() >= 3 && m17::crc16(pkt.data(), pkt.size()) == 0;
        if (ok) {
            tel.packetsOk++;
            M17Message m;
            m.wallTime = tel.now.wallTime; m.src = tel.now.src; m.dst = tel.now.dst; m.can = tel.now.can < 0 ? 0 : tel.now.can;
            if (pkt[0] == 0x05) {
                size_t e = 1;
                while (e < pkt.size() - 2 && pkt[e]) e++;
                m.text = std::string((const char*)pkt.data() + 1, e - 1);
                tel.now.type = "SMS";
            } else {
                char b[64]; snprintf(b, sizeof b, "packet type 0x%02X, %zu bytes", pkt[0], pkt.size() - 2); m.text = b;
            }
            tel.now.text = m.text;
            tel.messages.insert(tel.messages.begin(), m);
            if (tel.messages.size() > kMaxMsgs) tel.messages.pop_back();
            if (log) log("M17 message from " + (m.src.empty() ? std::string("?") : m.src) + ": " + m.text);
        } else tel.packetsBad++;
        for (auto& q : paths) q.locked = false;
        endTx();
    }

    // ------------------------------------------------------------ voice

    voice::CallInfo callInfo() const {
        voice::CallInfo ci;
        ci.srcText = tel.now.src; ci.dstText = tel.now.dst; ci.group = true;
        if (tel.now.can >= 0) ci.access = "CAN " + std::to_string(tel.now.can);
        return ci;
    }

    void voice(const uint8_t pl[16], int rateBps, int fn) {
        voice::Monitor* mon = activeMonitor();
        if (mon) {   // the voice monitor (voice_monitor.h) lists the call and says whether it is played
            voice::CallInfo ci = callInfo();
            ci.codecMissing = !tel.codec2;
            if (mon->offer(ci) != voice::Verdict::Play) { lastFnSeen = -1; return; }
            playing = ci;
        }
        if (!tel.codec2) return;
        if (!c2 || c2Rate != rateBps) { c2 = std::make_unique<M17Codec2>(rateBps); c2Rate = rateBps; }
        if (!c2->ok()) return;
        const int spf = c2->samplesPerFrame();
        if (lastFnSeen >= 0) {   // frames missed: as much silence
            const int miss = std::min(5, std::max(0, fn - lastFnSeen - 1));
            for (int k = 0; k < miss * 320; k++) aud8.push_back(0.f);
        }
        std::vector<int16_t> pcm((size_t)spf);
        const int per = rateBps == 1600 ? 1 : 2;
        for (int k = 0; k < per; k++) {
            c2->decode(pl + 8 * k, pcm.data());
            for (int16_t s : pcm) aud8.push_back(s / 32768.f);
            tel.voiceFrames++;
        }
        tel.now.voice = true;
        flushAudio();
    }

    void flushAudio() {
        if (aud8.empty()) return;
        if (tapReq.exchange(false)) { std::lock_guard<std::mutex> lk(mu); tap = tapSet; }
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

    void report() {
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        if (txOn && !paths[0].locked && !paths[1].locked && sigSec() - lastAny > 0.5) endTx();
        tel.state = txOn ? 2 : sigSec() - lastAny < 2 ? 1 : 0;
        tel.dataValid = txOn;
        if (txOn && !tel.now.packet) tel.now.durSec = std::max(tel.now.durSec, sigSec() - tel.now.startSec);
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

M17Receiver::M17Receiver() : p_(std::make_unique<Impl>()) {}
M17Receiver::~M17Receiver() = default;

void M17Receiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void M17Receiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
bool M17Receiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= m17Tuning().minSampleRate - 1; }
void M17Receiver::reset() { p_->resetReq = true; }

void M17Receiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    m.takeRequests();
    if (m.rate <= 0 || n == 0) return;
    const double inv = 1.0 / ((double)m.paths[0].D * m.paths[0].D * m.paths[0].D);
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
        // both readings of the spectrum (the frames of one are ignored while the other decodes)
        m.paths[0].input(std::complex<double>(re, im), inv);
        m.paths[1].input(std::complex<double>(re, -im), inv);
        m.nIn++;
        if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); }
    }
    m.nPower += (int64_t)n;
}

void M17Receiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    M17Telemetry e;
    Impl::moveLists(p_->pub, e);
    p_->clearReq = true; p_->loadReq = false;
    p_->pub.seq++;
}
std::string M17Receiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.log, p_->pub.messages, p_->pub.positions);
}
bool M17Receiver::loadResults(const std::string& blob) {
    double t0 = 0;
    M17Telemetry l;
    if (!results::unpack(blob, t0, l.log, l.messages, l.positions)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    M17Telemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->loadReq = true;
    p_->pub.seq++;
    return true;
}
bool M17Receiver::telemetry(M17Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void M17Receiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void M17Receiver::setCanFilter(int can) { p_->canFilter = can < 0 ? -1 : std::min(15, can); }
void M17Receiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void M17Receiver::setMuted(bool m) { p_->muted = m; }
void M17Receiver::setSilent(bool s) { p_->silent = s; }
void M17Receiver::setVoiceMonitor(voice::Monitor* m) { p_->monSet = m; }
void M17Receiver::setAudioTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tapSet = std::move(cb); p_->tapReq = true; }

ModeTuning m17Tuning() {
    ModeTuning t;
    t.stdMode = 33; t.id = "m17"; t.name = "M17";
    t.minMhz = 28; t.maxMhz = 1300; t.defMhz = 433.475;   // the 70 cm M17 frequency suggested for IARU Region 1 (IARU VHF+ newsletter 92, June 2023)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.0125;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike
    return t;
}

} // namespace dect2

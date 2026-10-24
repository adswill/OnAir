// NXDN receiver (see nxdn_rx.h).
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/dsp/cic3.h"
#include "dect2/results/results_io.h"
#include "dect2/nxdn/nxdn_tel.h"
#include "dect2/nxdn/nxdn_proto.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include "dect2/voicekeys/voice_decrypt.h"
#include "dect2/audio/audioout.h"
#include "dect2/voice/voice_monitor.h"
#include <algorithm>
#include <atomic>
#include <cmath>
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
constexpr double kFs = 48000;            // demodulator rate: 20 samples per NXDN48 symbol, 10 per NXDN96 symbol
constexpr int kYMask = 32767;
constexpr size_t kMaxEvents = 200;

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
} // namespace

struct NxdnReceiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true};
    std::function<void(const std::string&)> log;
    std::function<void(const float*, size_t)> tapSet, tap;
    std::function<void(const uint8_t*)> vtapSet, vtap;
    std::atomic<bool> tapReq{false};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    voice::Monitor* monSet = nullptr;   // setVoiceMonitor; else voice::monitor("nxdn") while not silent
    voice::CallInfo playing;            // the call whose voice is in aud8
    voice::Monitor* activeMonitor() { return monSet ? monSet : silent.load() ? nullptr : &voice::monitor("nxdn"); }
    NxdnTelemetry pub, tel;

    double rate = 0, offset = 0;
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

    void initFront() {
        D = std::max(1, (int)std::floor(rate / 100000.0));
        fd = rate > 0 ? rate / D : 0;
        inv = 1.0 / ((double)D * D * D);
        lo = 1; loCount = 0;
        loStep = rate > 0 ? std::polar(1.0, -2 * kPi * offset / rate) : std::complex<double>(1, 0);
        cic.reset();
        decCount = 0;
        dbuf.assign(8192, cf32(0, 0)); dN = 0;
        rstep = fd / kFs;
        M = std::max(8, (int)std::ceil(5.5 * fd / 6000.0 / 2));
        fir.assign((size_t)nPh * 2 * M, 0.f);
        const double fc = 7500.0 / std::max(1.0, fd);
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
        if (++decCount >= D) {
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
                const float f = (float)(std::atan2(d.imag(), d.real()) * kFs / (2 * kPi));
                for (auto& c2 : ch) c2.push(f);
                for (int k = 0; k < 2; k++) chainStep(k);
            }
        }
    }

    // ------------------------------------------------------------ one chain per symbol rate
    struct Chain {
        int baud = 2400, sps = 20;
        std::vector<float> y = std::vector<float>(kYMask + 1, 0.f);
        int64_t yN = 0;
        enum { Search, Track } mode = Search;
        int64_t candN = -1; float candR = 0;
        int64_t fs = 0;          // the sample of the last FSW symbol of the current frame
        bool decoded = false;    // the current frame's payload is done
        double g = 1, d = 0;     // level (signed: polarity) and offset, Hz
        int miss = 0;
        bool fly = false;
        int bad = 0;             // frames in a row without a good CRC        // the current frame's sync was not found (carried over)
        // Symbol filter: a windowed-sinc low pass to 0.625 of the symbol rate (1500 Hz for NXDN48, 3000 Hz for NXDN96; the raised-cosine
        // signal reaches 0.6 of it). The discriminator's noise grows with frequency, and the half-symbol box used before let much of it
        // through: the threshold moved by about 5 dB.
        int sD = 40;                     // the filter's delay in samples (two symbols)
        std::vector<float> sh, shist;
        int shPos = 0;
        void init(int b) {
            baud = b; sps = (int)(kFs / b);
            sD = 2 * sps;
            const int n = 2 * sD + 1;
            const double lpf = 0.625 * b;
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
            std::fill(y.begin(), y.end(), 0.f); yN = 0;
            mode = Search; candN = -1; candR = 0; miss = 0; decoded = false;
        }
        void push(float f) {
            if (sh.empty()) init(baud);
            const int n = (int)sh.size();
            shist[(size_t)shPos] = shist[(size_t)(shPos + n)] = f;
            shPos = (shPos + 1) % n;
            const float* x = shist.data() + shPos;
            float acc = 0;
            for (int k = 0; k < n; k++) acc += x[k] * sh[(size_t)(n - 1 - k)];
            y[(size_t)(yN & kYMask)] = acc;
            yN++;
        }
        // the sample a symbol's centre lands on: the filter output lags by about half its length
        float at(int64_t i) const { return y[(size_t)((i + sD) & kYMask)]; }
        void fswAt(int64_t e, float* v) const { for (int k = 0; k < nxdn::kFswSym; k++) v[k] = at(e - (int64_t)(nxdn::kFswSym - 1 - k) * sps); }
    };
    Chain ch[2];
    int lastChain = -1;          // the chain (rate) whose frames last passed a CRC: only it ends a call when its signal goes

    bool syncOk(const Chain& c, const float* v, double g, double d) const {
        const double unit = c.baud == 4800 ? 800.0 : 350.0;
        if (std::fabs(g) < 0.4 * unit || std::fabs(g) > 1.8 * unit) return false;
        for (int k = 0; k < nxdn::kFswSym; k++) if (nearestLevel((v[k] - d) / g) != nxdn::kFsw[k]) return false;
        return true;
    }

    void chainStep(int idx) {
        Chain& c = ch[idx];
        const int64_t last = c.yN - 1 - 2 * c.sD;   // newest sample whose symbol filter is complete
        const int64_t need = (int64_t)(nxdn::kFswSym) * c.sps;
        if (last < need) return;
        float v[nxdn::kFswSym];
        if (c.mode == Chain::Search) {
            c.fswAt(last, v);
            const float r = pearson(v, nxdn::kFsw, nxdn::kFswSym);
            if (std::fabs(r) > 0.85f && std::fabs(r) > c.candR) {
                double g, d;
                lsFit(v, nxdn::kFsw, nxdn::kFswSym, g, d);
                if (syncOk(c, v, g, d)) { c.candN = last; c.candR = std::fabs(r); }
            }
            if (c.candN >= 0 && last - c.candN > c.sps / 2) {
                c.fs = c.candN; c.candN = -1; c.candR = 0;
                c.fswAt(c.fs, v);
                lsFit(v, nxdn::kFsw, nxdn::kFswSym, c.g, c.d);
                c.mode = Chain::Track; c.miss = 0; c.decoded = false; c.fly = false; c.bad = 0;
            }
            return;
        }
        // tracking
        if (!c.decoded && last >= c.fs + (int64_t)nxdn::kPayloadSym * c.sps) {
            c.decoded = true;
            c.bad = decodeFrame(c) ? 0 : c.bad + 1;
            if (c.bad >= 3) { c.mode = Chain::Search; if (lastChain == idx) { lastChain = -1; lost(); } return; }   // following noise, or not NXDN
        }
        const int W = std::max(2, c.sps / 4);
        const int64_t nominal = c.fs + (int64_t)nxdn::kFrameSym * c.sps;
        if (last < nominal + W) return;
        float best = -2; int64_t bn = nominal; double bg = c.g, bd = c.d;
        for (int64_t e = nominal - W; e <= nominal + W; e++) {
            c.fswAt(e, v);
            const float r = pearson(v, nxdn::kFsw, nxdn::kFswSym) * (c.g > 0 ? 1.f : -1.f);
            if (r > best) { best = r; bn = e; }
        }
        c.fswAt(bn, v);
        lsFit(v, nxdn::kFsw, nxdn::kFswSym, bg, bd);
        const double lr = std::fabs(bg / c.g);
        if (best > 0.6f && bg * c.g > 0 && (syncOk(c, v, bg, bd) || (best > 0.8f && lr > 0.6 && lr < 1.6))) { c.fs = bn; c.g = 0.5 * c.g + 0.5 * bg; c.d = 0.5 * c.d + 0.5 * bd; c.miss = 0; c.fly = false; }
        else if (++c.miss <= 2) { c.fs = nominal; c.fly = true; }   // flywheel over a damaged sync
        else { c.mode = Chain::Search; if (lastChain == idx) { lastChain = -1; lost(); } return; }
        c.decoded = false;
    }

    // ------------------------------------------------------------ frames
    double lastGood = -10;
    double berErr = 0, berBits = 0;
    double snrAvg = 0;
    nxdn::Lich lastLich;
    bool haveLich = false;
    uint8_t sf[4][18]; bool sfOk[4] = {};
    int sfRan = -1;
    // short data being received
    bool sdOn = false; int sdSrc = -1, sdDst = -1, sdType = 4, sdBlocks = 0; std::string sdText; double sdStart = 0; int64_t sdWall = 0; int sdBaud = 0;
    double statLast = -10; int statKey = -1;
    uint32_t callBer = 0, callBits = 0;
    std::unique_ptr<NxdnAmbe> ambe;
    // User-supplied-key decryption of the current call (see voice_decrypt.h). Null unless the call is encrypted AND a key the user loaded
    // matches it. The NXDN scrambler (15-bit LFSR) is publicly specified; DES/AES use the OFB keystream.
    std::unique_ptr<voicekeys::Decryptor> dec;
    std::vector<float> aud8, pcmOut;
    float upPrev = 0;
    std::unique_ptr<AudioOut> audioOut;

    void addBer(int e, int n) { berErr += e; berBits += n; callBer += (uint32_t)e; callBits += (uint32_t)n; }
    void event(const std::string& s) {
        char b[24];
        const time_t t = time(nullptr);
        struct tm g;
#ifdef _WIN32
        gmtime_s(&g, &t);
#else
        gmtime_r(&t, &g);
#endif
        snprintf(b, sizeof b, "%02d:%02d:%02d  ", g.tm_hour, g.tm_min, g.tm_sec);
        tel.events.insert(tel.events.begin(), b + s);
        if (tel.events.size() > kMaxEvents) tel.events.pop_back();
    }
    void pushLog(const NxdnCall& c) {
        tel.log.insert(tel.log.begin(), c);
        if (tel.log.size() > kNxdnLogMax) tel.log.pop_back();
    }

    bool decodeFrame(Chain& c) {
        float v[nxdn::kFrameSym];
        int lv[nxdn::kFrameSym];
        for (int k = 0; k < nxdn::kFswSym; k++) { v[k] = c.at(c.fs - (int64_t)(nxdn::kFswSym - 1 - k) * c.sps); lv[k] = nxdn::kFsw[k]; }
        for (int k = 0; k < nxdn::kPayloadSym; k++) v[nxdn::kFswSym + k] = c.at(c.fs + (int64_t)(k + 1) * c.sps);
        for (int k = nxdn::kFswSym; k < nxdn::kFrameSym; k++) lv[k] = nearestLevel((v[k] - c.d) / c.g);
        double g, d;
        lsFit(v, lv, nxdn::kFrameSym, g, d);   // the whole frame, decision directed
        if (g * c.g > 0 && std::fabs(g) > 0.5 * std::fabs(c.g) && std::fabs(g) < 2 * std::fabs(c.g)) { c.g = g; c.d = d; }
        double mse = 0;
        float s[nxdn::kPayloadSym];
        uint8_t pn[nxdn::kPayloadSym];
        nxdn::pn9(pn, nxdn::kPayloadSym);
        for (int k = 0; k < nxdn::kPayloadSym; k++) {
            const double x = (v[nxdn::kFswSym + k] - c.d) / c.g;
            const double e = x - nearestLevel(x);
            mse += e * e;
            s[k] = (float)(pn[k] ? -x : x);
        }
        mse /= nxdn::kPayloadSym;
        const float snr = (float)(10 * std::log10(5.0 / std::max(1e-4, mse)));
        tel.frames++;
        // LICH
        uint8_t lb = 0;
        for (int k = 0; k < 8; k++) lb = (uint8_t)((lb << 1) | (s[k] < 0 ? 1 : 0));
        nxdn::Lich l;
        bool lichOk = nxdn::lichDecode(lb, l);
        if (!lichOk) {
            // one parity bit: the least reliable symbol is the one most likely wrong
            int wk = 0;
            for (int k = 1; k < 8; k++) if (std::fabs(s[k]) < std::fabs(s[wk])) wk = k;
            if (std::fabs(s[wk]) < 1.f) { lb ^= (uint8_t)(0x80 >> wk); lichOk = nxdn::lichDecode(lb, l); }
        }
        if (lichOk) { lastLich = l; haveLich = true; }
        else { tel.lichBad++; if (!haveLich) return false; l = lastLich; }
        bool sacchOk = false;
        std::vector<std::vector<uint8_t>> voice;
        // soft bits of the 174 channel symbols: > 0 a 1
        float sb[348];
        for (int k = 0; k < 174; k++) {
            const float x = s[8 + k];
            sb[2 * k] = std::max(-3.f, std::min(3.f, -x));
            sb[2 * k + 1] = std::max(-1.f, std::min(1.f, std::fabs(x) - 2.f)) * 2.f;
        }
        bool any = false;
        const int idx = (int)(&c - ch);
        if (l.rfct == nxdn::kRcch) {
            uint8_t info[155]; int e = 0;
            if (nxdn::decode(nxdn::Ch::Cac, sb, info, &e)) {
                tel.cacOk++; addBer(e, 300); any = true;
                int ran = 0; for (int i = 2; i < 8; i++) ran = (ran << 1) | info[i];
                tel.ran = ran;
                nxdn::Msg m;
                if (nxdn::parseMsg(info + 8, 147, m)) onMsg(m, c, ran, false);
            } else tel.cacBad++;
        } else {
            uint8_t info[26]; int e = 0;
            if (nxdn::decode(nxdn::Ch::Sacch, sb, info, &e)) {
                tel.sacchOk++; addBer(e, 60); any = true; sacchOk = true;
                const int st = (info[0] << 1) | info[1];
                int ran = 0; for (int i = 2; i < 8; i++) ran = (ran << 1) | info[i];
                tel.ran = ran;
                if (l.fct == nxdn::kFctSacchSf) {
                    const int part = 3 - st;
                    if (part == 0) for (bool& o : sfOk) o = false;
                    memcpy(sf[part], info + 8, 18); sfOk[part] = true; sfRan = ran;
                    if (part == 3 && sfOk[0] && sfOk[1] && sfOk[2]) {
                        uint8_t mb[72];
                        for (int p = 0; p < 4; p++) memcpy(mb + 18 * p, sf[p], 18);
                        nxdn::Msg m;
                        if (nxdn::parseMsg(mb, 72, m)) onMsg(m, c, ran, true);
                        for (bool& o : sfOk) o = false;
                    }
                }
            } else { tel.sacchBad++; for (bool& o : sfOk) o = false; }
            for (int h = 0; h < 2; h++) {
                const float* hb = sb + 60 + 144 * h;
                const bool facch = l.opt == 0 || (l.opt == 1 && h == 0) || (l.opt == 2 && h == 1);
                if (facch) {
                    uint8_t fi[80]; int fe = 0;
                    if (nxdn::decode(nxdn::Ch::Facch1, hb, fi, &fe)) {
                        tel.facchOk++; addBer(fe, 144); any = true;
                        nxdn::Msg m;
                        if (nxdn::parseMsg(fi, 80, m)) onMsg(m, c, tel.ran, false);
                    } else tel.facchBad++;
                } else {
                    for (int vf = 0; vf < 2; vf++) {
                        uint8_t vb[72];
                        for (int i = 0; i < 72; i++) vb[i] = hb[72 * vf + i] > 0 ? 1 : 0;
                        voice.emplace_back(vb, vb + 72);
                    }
                }
            }
        }
        // voice only from frames that are surely ours: a good SACCH, or a found sync and LICH within a call
        // (a call is started from voice only with a found sync and a good SACCH: the 6-bit CRC alone passes noise once in 64)
        const bool sure = sacchOk && lichOk && !c.fly;
        if (sure || (lichOk && tel.now.active && lastChain == idx && (sacchOk || !c.fly))) for (const auto& vb : voice) voiceFrame(vb.data(), c, sure);
        tel.blocksOk = tel.sacchOk + tel.facchOk + tel.cacOk;
        tel.blocksBad = tel.sacchBad + tel.facchBad + tel.cacBad;
        if (any) {
            lastGood = sigSec();
            lastChain = idx;
            tel.baud = c.baud;
            tel.inverted = c.g < 0;
            tel.deviationHz = (float)(3 * std::fabs(c.g));
            tel.cfoHz = c.d;
            snrAvg = snrAvg == 0 ? snr : 0.8 * snrAvg + 0.2 * snr;
            tel.snrDb = (float)snrAvg;
        }
        tel.berPct = berBits > 0 ? (float)(100 * berErr / berBits) : 0.f;
        if (tel.now.active) { tel.now.durSec = sigSec() - tel.now.startSec; tel.now.berPct = callBits ? 100.f * callBer / callBits : 0.f; }
        return any;
    }

    void startCall(int src, int dst, int type, int cipher, int key, int ran, int baud, bool late) {
        if (tel.now.active) endCall(false);
        NxdnCall n;
        n.wallTime = (int64_t)time(nullptr); n.startSec = sigSec(); n.active = true;
        n.kind = 0; n.src = src; n.dst = dst; n.callType = type; n.cipher = cipher; n.keyId = key; n.ran = ran; n.baud = baud; n.lateEntry = late;
        tel.now = n;
        callBer = callBits = 0;
        if (late) tel.lateEntries++;
        ambe = tel.ambe ? std::make_unique<NxdnAmbe>() : nullptr;
        // Match a user key to this call (never a search). Nothing is decrypted when the user loaded no matching key.
        dec.reset();
        if (cipher != 0) {
            voicekeys::CallKeyInfo info;
            info.protocol = voicekeys::Protocol::Nxdn;
            info.keyId = key; info.talkgroup = type == 1 ? dst : -1; info.unitId = src;
            auto look = voicekeys::lookupForCall(info, true, 72);
            tel.now.decrypt = voicekeys::Decryptor::statusLine(look.status, look.keyName);
            if (look.decryptor) dec = std::move(look.decryptor);
            event("NXDN " + tel.now.decrypt);
        }
    }
    void endCall(bool rel) {
        if (!tel.now.active) return;
        flushAudio();
        tel.now.active = false;
        tel.now.endSeen = rel;
        tel.now.durSec = sigSec() - tel.now.startSec;
        pushLog(tel.now);
        char b[160];
        snprintf(b, sizeof b, "NXDN call %s -> %s (%s), %.1f s%s", nxdnId(tel.now.src).c_str(), nxdnId(tel.now.dst).c_str(), nxdn::callTypeName(tel.now.callType), tel.now.durSec,
                 rel ? "" : ", signal lost");
        if (log) log(b);
    }
    void endData() {
        if (!sdOn) return;
        sdOn = false;
        NxdnCall n;
        n.wallTime = sdWall; n.startSec = sdStart; n.durSec = sigSec() - sdStart; n.kind = 2; n.src = sdSrc; n.dst = sdDst; n.callType = sdType;
        n.text = sdText; n.baud = sdBaud; n.ran = tel.ran; n.endSeen = true;
        pushLog(n);
        event("short data " + nxdnId(sdSrc) + " -> " + nxdnId(sdDst) + ": " + sdText);
    }
    void lost() {
        endCall(false);
        endData();
        haveLich = false;
        for (bool& o : sfOk) o = false;
    }

    void onMsg(const nxdn::Msg& m, const Chain& c, int ran, bool fromSacch) {
        char b[200];
        switch (m.type) {
        case nxdn::kVcall: {
            const NxdnCall& n = tel.now;
            if (n.active && (n.src == m.src || n.src < 0) && (n.dst == m.dst || n.dst < 0)) {   // the call goes on (or its ids arrive)
                tel.now.src = m.src; tel.now.dst = m.dst; tel.now.callType = m.callType; tel.now.cipher = m.cipher; tel.now.keyId = m.keyId; tel.now.ran = ran;
                return;
            }
            startCall(m.src, m.dst, m.callType, m.cipher, m.keyId, ran, c.baud, fromSacch);
            snprintf(b, sizeof b, "VCALL %s %d -> %d%s%s", nxdn::callTypeName(m.callType), m.src, m.dst, m.cipher ? ", encrypted" : "", fromSacch ? " (joined late)" : "");
            event(b);
            break;
        }
        case nxdn::kTxRel:
            if (sdOn) { endData(); break; }
            if (tel.now.active) { endCall(true); snprintf(b, sizeof b, "TX_REL %d -> %d", m.src, m.dst); event(b); }
            break;
        case nxdn::kStatReq: {
            const int key = (m.src << 8) ^ (m.dst << 4) ^ m.status;
            if (key == statKey && sigSec() - statLast < 1.0) break;   // sent in both halves
            statKey = key; statLast = sigSec();
            NxdnCall n;
            n.wallTime = (int64_t)time(nullptr); n.startSec = sigSec(); n.kind = 1; n.src = m.src; n.dst = m.dst; n.callType = m.callType; n.ran = ran; n.baud = c.baud;
            n.text = "status " + std::to_string(m.status); n.endSeen = true;
            pushLog(n);
            snprintf(b, sizeof b, "status %d from %d to %d", m.status, m.src, m.dst);
            event(b);
            break;
        }
        case nxdn::kSdHeader:
            endData();
            sdOn = true; sdSrc = m.src; sdDst = m.dst; sdType = m.callType; sdBlocks = m.blocks; sdText.clear(); sdStart = sigSec(); sdWall = (int64_t)time(nullptr); sdBaud = c.baud;
            break;
        case nxdn::kSdData:
            if (sdOn && m.block * 8 >= (int)sdText.size()) {
                if ((int)sdText.size() < m.block * 8) sdText.append((size_t)(m.block * 8) - sdText.size(), '?');   // a lost block
                sdText += m.text;
                if (m.block + 1 >= sdBlocks) endData();
            }
            break;
        case nxdn::kSiteInfo: case nxdn::kSrvInfo: {
            NxdnSite& s = tel.siteInfo;
            const bool news = !s.valid || s.location != m.location;
            s.valid = true; s.location = m.location; s.system = nxdn::locSystem(m.location); s.site = nxdn::locSite(m.location); s.ran = ran;
            s.service = m.service; if (m.type == nxdn::kSiteInfo) s.channel = m.channel;
            s.wallTime = (int64_t)time(nullptr);
            if (news) { snprintf(b, sizeof b, "%s location 0x%06X (system %d, site %d), RAN %d", nxdn::msgName(m.type), m.location, s.system, s.site, ran); event(b); }
            break;
        }
        default: break;
        }
    }

    void voiceFrame(const uint8_t vb[72], const Chain& c, bool sure) {
        if (!tel.now.active && !sure) return;
        if (!tel.now.active) startCall(-1, -1, 1, 0, 0, tel.ran, c.baud, true);
        tel.now.voiceFrames++;
        tel.voiceFrames++;
        if (vtap) vtap(vb);
        // Encrypted, and the user loaded a matching key: decrypt the voice bits (never a search). Without a key the call is listed as
        // encrypted by the voice monitor and nothing is decoded.
        const uint8_t* frame = vb;
        uint8_t vbuf[72];
        bool clear = tel.now.cipher == 0;
        if (!clear && dec) {
            std::memcpy(vbuf, vb, 72);
            if (dec->decryptFrame(vbuf)) { frame = vbuf; clear = true; }   // false: unsupported algorithm
        }
        voice::Monitor* mon = activeMonitor();
        if (mon) {   // the voice monitor (voice_monitor.h) lists the call and says whether it is played
            voice::CallInfo ci;
            ci.src = tel.now.src > 0 ? (uint32_t)tel.now.src : 0; ci.dst = tel.now.dst > 0 ? (uint32_t)tel.now.dst : 0;
            ci.group = tel.now.callType <= 1; ci.encrypted = !clear; ci.codecMissing = !ambe || !ambe->ok();
            ci.errorRate = callBits > 200 ? (float)callBer / callBits : -1.f;
            if (tel.now.ran >= 0) ci.access = "RAN " + std::to_string(tel.now.ran);
            if (mon->offer(ci) != voice::Verdict::Play) return;
            playing = ci;
        }
        if (!ambe || !ambe->ok() || !clear) return;
        int16_t pcm[160];
        ambe->decode(frame, pcm);
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

    // The call log, the control messages and the site stay through resets: Clear and a loaded file reach the feed thread's copy through these requests (mu), and the
    // published report at once
    bool clearReq = false, loadReq = false;
    NxdnTelemetry loaded;
    double loadedAt = 0;                   // the report's clock the loaded lists are on
    static void moveLists(NxdnTelemetry& to, NxdnTelemetry& from) {
        to.log = std::move(from.log); to.events = std::move(from.events); to.siteInfo = from.siteInfo;
        from.log.clear(); from.events.clear();
    }
    static void shiftTimes(NxdnTelemetry& t, double dt) {
        for (auto& c : t.log) c.startSec += dt;
    }
    void takeRequests() {   // feed thread
        std::lock_guard<std::mutex> lk(mu);
        if (clearReq) { NxdnTelemetry e; moveLists(tel, e); clearReq = false; }
        if (loadReq) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); loadReq = false; }
    }
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        if (tel.now.active) endCall(false);
        initFront();
        ch[0].init(2400); ch[1].init(4800); lastChain = -1;
        power = 0; nPower = 0; nIn = 0; nextReport = 0.25 * rate;
        lastGood = -10; berErr = berBits = 0; snrAvg = 0; haveLich = false; sdOn = false; statKey = -1;
        for (bool& o : sfOk) o = false;
        const uint64_t seq = tel.seq;
        NxdnTelemetry keep;                // the lists stay (clearResults): the clock starts again, they move back by the time it had reached
        moveLists(keep, tel);
        shiftTimes(keep, -tel.timeSec);
        tel = NxdnTelemetry(); tel.seq = seq;
        moveLists(tel, keep);
        tel.inputRate = rate;
        tel.ambe = nxdnAmbeAvailable(&tel.ambePath);
    }

    void report() {
        if (tapReq.exchange(false)) { std::lock_guard<std::mutex> lk(mu); tap = tapSet; vtap = vtapSet; }
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        if (!tel.ambe && tel.seq % 40 == 0) tel.ambe = nxdnAmbeAvailable(&tel.ambePath);   // installed while running ("Look again")
        if (tel.now.active) tel.now.durSec = sigSec() - tel.now.startSec;
        if (sdOn && sigSec() - sdStart > 5) endData();
        tel.state = tel.now.active ? 2 : sigSec() - lastGood < 1.5 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

NxdnReceiver::NxdnReceiver() : p_(std::make_unique<Impl>()) {}
NxdnReceiver::~NxdnReceiver() = default;

void NxdnReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void NxdnReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
bool NxdnReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= nxdnTuning().minSampleRate - 1; }
void NxdnReceiver::reset() { p_->resetReq = true; }

void NxdnReceiver::feed(const cf32* x, size_t n) {
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

void NxdnReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    NxdnTelemetry e;
    Impl::moveLists(p_->pub, e);
    p_->clearReq = true; p_->loadReq = false;
    p_->pub.seq++;
}
std::string NxdnReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.log, p_->pub.events, p_->pub.siteInfo);
}
bool NxdnReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    NxdnTelemetry l;
    if (!results::unpack(blob, t0, l.log, l.events, l.siteInfo)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    NxdnTelemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->loadReq = true;
    p_->pub.seq++;
    return true;
}
bool NxdnReceiver::telemetry(NxdnTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void NxdnReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void NxdnReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void NxdnReceiver::setMuted(bool m) { p_->muted = m; }
void NxdnReceiver::setSilent(bool s) { p_->silent = s; }
void NxdnReceiver::setVoiceMonitor(voice::Monitor* m) { p_->monSet = m; }
void NxdnReceiver::setAudioTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tapSet = std::move(cb); p_->tapReq = true; }
void NxdnReceiver::setVoiceTap(std::function<void(const uint8_t bits[72])> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->vtapSet = std::move(cb); p_->tapReq = true; }

ModeTuning nxdnTuning() {
    ModeTuning t;
    t.stdMode = 40; t.id = "nxdn"; t.name = "NXDN";
    t.minMhz = 30; t.maxMhz = 1000; t.defMhz = 446.10625;   // a 6.25 kHz channel of the 446 MHz licence-free digital band
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.0125;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike
    return t;
}

} // namespace dect2

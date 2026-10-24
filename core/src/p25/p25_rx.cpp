// P25 Phase 1 receiver (see p25_rx.h).
#include "dect2/p25/p25_rx.h"
#include "dect2/dsp/cic3.h"
#include "dect2/results/results_io.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/p25/p25_proto.h"
#include "dect2/p25/p25_imbe.h"
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
#include <deque>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kFs = 48000;            // demodulator rate: 10 samples per symbol
constexpr int kSps = 10;
constexpr int kTaps = 161, kDelay = 80;   // the symbol filter: low pass to 3 kHz, just above the C4FM band edge (2880 Hz)
constexpr int kYMask = 32767;
constexpr double kUnit = 600.0;          // Hz per level
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
int hardDibit(float x) { return p25::symToDibit(nearestLevel(x)); }
uint64_t bitsVal(const uint8_t* b, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | b[i]; return v; }
// the 64 code words of a hexbit code (the first sent bit in the top bit), for the soft decision below
struct HexTab { uint32_t cw[64]; };
const HexTab& hamTab() { static const HexTab t = [] { HexTab h; for (int v = 0; v < 64; v++) h.cw[v] = p25::hamming10Encode(v); return h; }(); return t; }
const HexTab& golay18Tab() { static const HexTab t = [] { HexTab h; for (int v = 0; v < 64; v++) h.cw[v] = p25::golay18Encode(v); return h; }(); return t; }
// maximum likelihood: the hexbit whose code word (n bits) correlates best with the soft values (> 0 a 1)
int mlHex(const float* sb, int n, const HexTab& t) {
    int best = 0;
    float bm = -1e30f;
    for (int v = 0; v < 64; v++) {
        float m = 0;
        for (int k = 0; k < n; k++) m += ((t.cw[v] >> (n - 1 - k)) & 1) ? sb[k] : -sb[k];
        if (m > bm) { bm = m; best = v; }
    }
    return best;
}
} // namespace

struct P25Receiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true};
    std::function<void(const std::string&)> log;
    std::function<void(const float*, size_t)> tapSet, tap;
    std::function<void(const uint8_t*, bool)> vtapSet, vtap;
    std::atomic<bool> tapReq{false};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    voice::Monitor* monSet = nullptr;   // setVoiceMonitor; else voice::monitor("p25") while not silent
    voice::CallInfo playing;            // the call whose voice is in aud8
    voice::Monitor* activeMonitor() { return monSet ? monSet : silent.load() ? nullptr : &voice::monitor("p25"); }
    P25Telemetry pub, tel;

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
        M = std::max(8, (int)std::ceil(8.0 * fd / 6000.0 / 2));   // a sharper channel filter than NXDN's: C4FM reaches +-5 kHz
        fir.assign((size_t)nPh * 2 * M, 0.f);
        // the channel filter: +-6 kHz holds the C4FM signal (99 % within +-4 kHz) with a 2-3 kHz tuning error; the wider +-8.5 kHz used before let in
        // noise that the discriminator turns into symbol errors (about 1.5 dB at the threshold)
        const double fc = 6000.0 / std::max(1.0, fd);
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
                push((float)(std::atan2(d.imag(), d.real()) * kFs / (2 * kPi)));
                step();
            }
        }
    }

    // ------------------------------------------------------------ symbol filter and frame sync search
    float hist[2 * kTaps] = {}; int hPos = 0; float h[kTaps];
    void initFilter() {
        double sum = 0;
        for (int k = 0; k < kTaps; k++) {
            const double symK = 3000.0;   // 0.625 of the symbol rate: the C4FM pulse ends at 2880 Hz (3600 Hz before: 1 dB more noise)
            const double t = k - kDelay, x = 2 * symK / kFs * t;
            const double s = t == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double w = 0.42 + 0.5 * std::cos(kPi * t / (kDelay + 1)) + 0.08 * std::cos(2 * kPi * t / (kDelay + 1));
            h[k] = (float)(s * w); sum += h[k];
        }
        for (float& v : h) v = (float)(v / sum);
        for (float& v : hist) v = 0;
        hPos = 0;
    }
    std::vector<float> y = std::vector<float>(kYMask + 1, 0.f);
    int64_t yN = 0;
    int64_t candN = -1; float candR = 0; double candG = 0, candD = 0;
    struct Pending { int64_t fs; double g, d; double frac = 0; int duid = -1, nac = -1, nidErr = 0; };
    std::deque<Pending> pend;
    int64_t busyUntil = -1;      // the last sample of the last frame decoded: syncs found inside it are not real

    void push(float f) {
        hist[hPos] = hist[hPos + kTaps] = f;   // twice, so the newest kTaps are always in one run
        hPos = (hPos + 1) % kTaps;
        const float* x = hist + hPos;          // oldest first
        float acc = 0;
        for (int k = 0; k < kTaps; k++) acc += x[k] * h[kTaps - 1 - k];
        y[(size_t)(yN & kYMask)] = acc;
        yN++;
    }
    float at(int64_t i) const { return y[(size_t)((i + kDelay) & kYMask)]; }
    // frame position p (0 = the first sync symbol) of a frame whose last sync symbol is at sample fs
    float posAt(const Pending& f, int p, double tw = 0) const {   // tw: a timing correction in samples
        const double t = f.frac + tw;
        const int64_t i0 = f.fs + (int64_t)(p - (p25::kSyncSym - 1)) * kSps + (int64_t)std::floor(t);
        const float a = (float)(t - std::floor(t));
        return at(i0) * (1 - a) + at(i0 + 1) * a;
    }
    float syncR(int64_t e) const {
        float v[p25::kSyncSym];
        for (int k = 0; k < p25::kSyncSym; k++) v[k] = at(e - (int64_t)(p25::kSyncSym - 1 - k) * kSps);
        return std::fabs(pearson(v, p25::kSync, p25::kSyncSym));
    }

    bool syncOk(const float* v, double g, double d) const {
        if (std::fabs(g) < 0.4 * kUnit || std::fabs(g) > 1.8 * kUnit) return false;
        int bad = 0;   // the sync has only +3 and -3: its signs must be right, two may come out as +-1
        for (int k = 0; k < p25::kSyncSym; k++) { const double x = (v[k] - d) / g * p25::kSync[k]; if (x < 0) return false; if (x < 2) bad++; }
        if (bad > 4) return false;
        return true;
    }

    void step() {
        const int64_t last = yN - 1 - kDelay;
        if (last < (int64_t)p25::kSyncSym * kSps) return;
        float v[p25::kSyncSym];
        for (int k = 0; k < p25::kSyncSym; k++) v[k] = at(last - (int64_t)(p25::kSyncSym - 1 - k) * kSps);
        const float r = pearson(v, p25::kSync, p25::kSyncSym);
        if (std::fabs(r) > 0.8f && std::fabs(r) > candR) {
            double g, d;
            lsFit(v, p25::kSync, p25::kSyncSym, g, d);
            if (syncOk(v, g, d)) { candN = last; candR = std::fabs(r); candG = g; candD = d; }
        }
        if (candN >= 0 && last - candN > kSps / 2) {
            const double r0 = candR, rm = syncR(candN - 1), rq = syncR(candN + 1), den = rm - 2 * r0 + rq;
            Pending f{candN, candG, candD};
            f.frac = den < 0 ? std::max(-0.5, std::min(0.5, 0.5 * (rm - rq) / den)) : 0;
            pend.push_back(f);
            candN = -1; candR = 0;
        }
        while (!pend.empty()) {
            Pending& f = pend.front();
            if (f.fs <= busyUntil) { pend.pop_front(); continue; }
            if (f.duid < 0) {
                if (last < f.fs + (int64_t)(57 - p25::kSyncSym) * kSps + 2) return;
                if (!nid(f)) { pend.pop_front(); continue; }
            }
            const int len = p25::frameLen(f.duid);
            if (last < f.fs + (int64_t)(len - p25::kSyncSym) * kSps + 2) return;
            const Pending fr = f;
            pend.pop_front();
            frame(fr, len);
        }
    }

    bool nid(Pending& f) {
        uint8_t b[64];
        int k = 0;
        for (int p = p25::kSyncSym; k < 32; p++) {
            if (p25::isStatusPos(p)) continue;
            const int dd = hardDibit((float)((posAt(f, p) - f.d) / f.g));
            b[2 * k] = (uint8_t)(dd >> 1); b[2 * k + 1] = (uint8_t)(dd & 1);
            k++;
        }
        int nac = 0, duid = 0, e = 0;
        const bool ok = p25::nidDecode(b, nac, duid, &e) && p25::frameLen(duid) > 0;
        if (!ok) { tel.nidBad++; tel.frames++; return false; }
        f.nac = nac; f.duid = duid; f.nidErr = e;
        return true;
    }

    // ------------------------------------------------------------ frames
    double lastGood = -10, lastTsbk = -10, lastVoice = -10;
    double berErr = 0, berBits = 0;
    double snrAvg = 0;
    std::vector<float> dl;       // the frame's data dibits (status symbols out) as normalised levels
    std::unique_ptr<P25Imbe> imbe;
    // User-supplied-key decryption of the current call (see voice_decrypt.h). Null unless the call is encrypted AND a key the user loaded
    // matches it. Never an auto-decrypter: when it is null the encrypted voice is reported and skipped, exactly as before.
    std::unique_ptr<voicekeys::Decryptor> dec;
    uint8_t curMi[9] = {};
    bool triedKey = false;              // we have already looked up a key for this encryption state (so we do not re-match every frame)
    std::string decStatus;              // "decrypted with key <name>" / "encrypted: add your key" / "key mismatch"
    std::vector<float> aud8, pcmOut;
    float upPrev = 0;
    std::unique_ptr<AudioOut> audioOut;
    struct Grant { int tg, src, ch; bool group; double last; };
    std::vector<Grant> grants;

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
    void pushLog(const P25Call& c) {
        tel.log.insert(tel.log.begin(), c);
        if (tel.log.size() > kP25LogMax) tel.log.pop_back();
    }
    double freqOf(int ch) const { const P25IdenRow& i = tel.idens[(ch >> 12) & 15]; return i.valid ? i.baseHz + (ch & 0xFFF) * i.spacingHz : 0; }
    void touchTg(int tg, int src, double freq, bool enc, bool emerg, bool grant, bool call) {
        if (tg <= 0) return;
        auto it = std::find_if(tel.talkgroups.begin(), tel.talkgroups.end(), [&](const P25Talkgroup& t) { return t.tg == tg; });
        P25Talkgroup t;
        if (it != tel.talkgroups.end()) { t = *it; tel.talkgroups.erase(it); }
        t.tg = tg;
        if (src >= 0) t.lastSrc = src;
        if (freq > 0) t.lastFreqHz = freq;
        t.encrypted = t.encrypted || enc; t.emergency = t.emergency || emerg;
        if (grant) t.grants++;
        if (call) t.calls++;
        t.lastWall = (int64_t)time(nullptr);
        t.active = true;
        tel.talkgroups.insert(tel.talkgroups.begin(), t);
        if (tel.talkgroups.size() > kP25TgMax) tel.talkgroups.pop_back();
    }

    void frame(const Pending& f, int len) {
        // levels of the whole frame, decision directed refit of the deviation and offset
        std::vector<float> v((size_t)len);
        std::vector<int> lv((size_t)len);
        double g = f.g, d = f.d;
        // the symbol clock drifts against ours over a long frame (100 ppm is 0.9 samples over an LDU), and the sync's own timing is off when
        // the carrier has only just come up: the offset and slope with the cleanest decisions
        double bestE = 1e30, slope = 0, shift = 0;
        for (int j = -4; j <= 4; j++)
            for (int k = -8; k <= 8; k++) {
                const double sh = j * 0.25, sl = k * 0.0002;   // samples, samples per symbol
                double e = 0;
                for (int p = 0; p < len; p += 3) { const double x = (posAt(f, p, sh + sl * (p - 23)) - d) / g, q = x - nearestLevel(x); e += q * q; }
                if (e < bestE) { bestE = e; slope = sl; shift = sh; }
            }
        for (int p = 0; p < len; p++) v[(size_t)p] = posAt(f, p, shift + slope * (p - 23));
        for (int p = 0; p < len; p++) lv[(size_t)p] = p < p25::kSyncSym ? p25::kSync[p] : nearestLevel((v[(size_t)p] - d) / g);
        double g2, d2;
        lsFit(v.data(), lv.data(), len, g2, d2);
        if (g2 * g > 0 && std::fabs(g2) > 0.5 * std::fabs(g) && std::fabs(g2) < 2 * std::fabs(g)) { g = g2; d = d2; }
        dl.clear();
        double mse = 0;
        for (int p = 0; p < len; p++) {
            const float x = (float)((v[(size_t)p] - d) / g);
            const double e = x - nearestLevel(x);
            mse += e * e;
            if (!p25::isStatusPos(p)) dl.push_back(x);
        }
        mse /= len;
        const float snr = (float)(10 * std::log10(5.0 / std::max(1e-4, mse)));
        tel.frames++;
        berErr += f.nidErr; berBits += 63;
        const size_t P = 56;   // the payload's first data dibit
        int used = 0;          // data dibits used (TSBK / PDU)
        bool good = true;
        switch (f.duid) {
        case p25::kHdu: tel.hdu++; hdu(f, P); break;
        case p25::kLdu1: case p25::kLdu2: (f.duid == p25::kLdu1 ? tel.ldu1 : tel.ldu2)++; ldu(f, P); break;
        case p25::kTdu: tel.tdu++; term(f, false); break;
        case p25::kTdulc: tel.tdulc++; term(f, true); break;
        case p25::kTsbk: used = tsbks(f, P); good = used > 0; break;
        case p25::kPdu: tel.pdu++; used = pdu(f, P); good = used > 0; break;
        default: break;
        }
        // the frame's extent: for TSBK / PDU only the blocks that decoded
        int extent = len;
        if (f.duid == p25::kTsbk || f.duid == p25::kPdu) {
            const int want = 56 + std::max(used, 1) * 98;
            int p = 0, k = 0;
            while (k < want) { if (!p25::isStatusPos(p)) k++; p++; }
            extent = (p + 35) / 36 * 36;
        }
        busyUntil = f.fs + (int64_t)(extent - p25::kSyncSym) * kSps - kSps / 2;
        if (good) {
            lastGood = sigSec();
            tel.nac = f.nac;
            tel.inverted = g < 0;
            tel.deviationHz = (float)(3 * std::fabs(g));
            tel.cfoHz = d;
            snrAvg = snrAvg == 0 ? snr : 0.8 * snrAvg + 0.2 * snr;
            tel.snrDb = (float)snrAvg;
        }
        tel.berPct = berBits > 0 ? (float)(100 * berErr / berBits) : 0.f;
        tel.blocksOk = tel.frames - tel.nidBad + tel.tsbkOk + tel.lcOk + tel.esOk + tel.hduOk;
        tel.blocksBad = tel.nidBad + tel.tsbkBad + tel.lcBad + tel.esBad + tel.hduBad;
        if (tel.now.active) tel.now.durSec = sigSec() - tel.now.startSec;
    }

    // 24 Hamming words (from 120 dibits taken in six pieces) -> RS(24,k)
    bool lduHex(size_t at0, const int* offs, int k, uint8_t* out) {
        // soft decision: each Hamming word is the hexbit whose code word correlates best with the soft values (repairs most double errors
        // the hard decoder gets wrong), so the Reed-Solomon code sees fewer wrong hexbits
        float sb[240];
        int n = 0;
        for (int piece = 0; piece < 6; piece++)
            for (int i = 0; i < 20; i++) { const float x = dl[at0 + (size_t)offs[piece] + (size_t)i]; sb[n++] = -x; sb[n++] = std::fabs(x) - 2.f; }
        uint8_t s[24];
        for (int w = 0; w < 24; w++) s[w] = (uint8_t)mlHex(sb + 10 * w, 10, hamTab());
        if (!p25::rsDecode(s, 24, k)) return false;
        memcpy(out, s, (size_t)k);
        return true;
    }

    void startCall(int nac, bool late) {
        if (tel.now.active) endCall(false);
        P25Call n;
        n.wallTime = (int64_t)time(nullptr); n.startSec = sigSec(); n.active = true; n.kind = 0; n.nac = nac; n.lateEntry = late;
        tel.now = n;
        if (late) tel.lateEntries++;
        imbe = tel.imbe ? std::make_unique<P25Imbe>() : nullptr;
        dec.reset(); triedKey = false; decStatus.clear();
        std::memset(curMi, 0, sizeof curMi);
    }

    // Build (or clear) the decryptor for the current call from the key the user loaded that matches it. Never searches: lookupForCall
    // consults the user's own table and, only for a call with no key id, the user's small enabled list in stored order.
    void rebuildDecryptor() {
        triedKey = true;
        dec.reset();
        if (!tel.now.encrypted) { decStatus.clear(); return; }
        voicekeys::CallKeyInfo info;
        info.protocol = voicekeys::Protocol::P25;
        info.keyId = tel.now.keyId; info.algId = tel.now.algId; info.nac = tel.now.nac;
        info.talkgroup = tel.now.group ? tel.now.dst : -1; info.unitId = tel.now.src;
        auto look = voicekeys::lookupForCall(info, true, 144, curMi, sizeof curMi);
        decStatus = voicekeys::Decryptor::statusLine(look.status, look.keyName);
        if (look.decryptor) dec = std::move(look.decryptor);
        tel.now.decrypt = decStatus;
        event("P25 " + decStatus);
    }
    void endCall(bool term) {
        if (!tel.now.active) return;
        flushAudio();
        tel.now.active = false;
        tel.now.endSeen = term;
        tel.now.durSec = sigSec() - tel.now.startSec;
        pushLog(tel.now);
        touchTg(tel.now.group ? tel.now.dst : -1, tel.now.src, 0, tel.now.encrypted, tel.now.emergency, false, true);
        char b[200];
        snprintf(b, sizeof b, "P25 call %s -> %s%s, %.1f s%s%s", p25Id(tel.now.src).c_str(), tel.now.group ? "TG " : "", p25Id(tel.now.dst).c_str(), tel.now.durSec,
                 tel.now.encrypted ? ", encrypted" : "", term ? "" : ", signal lost");
        event(b + 4);
        if (log) log(b);
    }

    void hdu(const Pending& f, size_t P) {
        uint8_t s[36];
        float sb[648];   // soft decision, as in lduHex: the closest of the 64 Golay (18,6) code words
        for (int i = 0; i < 324; i++) { const float x = dl[P + (size_t)i]; sb[2 * i] = -x; sb[2 * i + 1] = std::fabs(x) - 2.f; }
        for (int w = 0; w < 36; w++) s[w] = (uint8_t)mlHex(sb + 18 * w, 18, golay18Tab());
        if (!p25::rsDecode(s, 36, 20)) { tel.hduBad++; return; }
        tel.hduOk++;
        p25::Crypto c;
        p25::hduParse(s, c);
        startCall(f.nac, false);
        tel.now.dst = c.tg > 0 ? c.tg : -1; tel.now.group = true;
        tel.now.algId = c.algId; tel.now.keyId = c.keyId; tel.now.encrypted = c.algId != p25::kAlgClear;
        std::memcpy(curMi, c.mi, sizeof curMi);
        rebuildDecryptor();
        char b[160];
        snprintf(b, sizeof b, "HDU NAC %03X talkgroup %d%s", f.nac, c.tg, tel.now.encrypted ? "" : ", clear");
        std::string s2 = b;
        if (tel.now.encrypted) { snprintf(b, sizeof b, ", encrypted: %s (0x%02X), key %d", p25::algName(c.algId), c.algId, c.keyId); s2 += b; }
        event(s2);
        lastVoice = sigSec();
    }

    void ldu(const Pending& f, size_t P) {
        static const int vcOff[9] = {0, 72, 164, 256, 348, 440, 532, 624, 712};
        static const int hexOff[6] = {144, 236, 328, 420, 512, 604};
        const bool one = f.duid == p25::kLdu1;
        uint8_t hex[16];
        const bool ok = lduHex(P, hexOff, one ? 12 : 16, hex);
        if (one) {
            (ok ? tel.lcOk : tel.lcBad)++;
            p25::Lc l;
            if (ok && p25::lcParse(hex, l) && (l.lco == p25::kLcGroup || l.lco == p25::kLcUnit) && l.mfid <= 1) {
                const bool grp = l.lco == p25::kLcGroup;
                const int dst = grp ? l.tg : l.dst;
                P25Call& n = tel.now;
                if (!n.active || (n.dst >= 0 && n.dst != dst) || (n.src >= 0 && n.src != l.src)) {
                    startCall(f.nac, true);
                    char b[160];
                    snprintf(b, sizeof b, "LDU1 %s %d -> %d%s%s (joined late)", grp ? "group" : "unit to unit", l.src, dst, l.svcOpts & 0x80 ? ", EMERGENCY" : "", l.svcOpts & 0x40 ? ", encrypted" : "");
                    event(b);
                } else if (n.src < 0) {
                    char b[160];
                    snprintf(b, sizeof b, "LDU1 %s %d -> %d%s", grp ? "group" : "unit to unit", l.src, dst, l.svcOpts & 0x80 ? ", EMERGENCY" : "");
                    event(b);
                }
                tel.now.src = l.src; tel.now.dst = dst; tel.now.group = grp;
                tel.now.emergency = (l.svcOpts & 0x80) != 0;
                if ((l.svcOpts & 0x40) && !tel.now.encrypted) { tel.now.encrypted = true; if (!triedKey) rebuildDecryptor(); }
            }
        } else {
            (ok ? tel.esOk : tel.esBad)++;
            if (ok && tel.now.active) {
                p25::Crypto c;
                p25::esParse(hex, c);
                const bool algChanged = c.algId != tel.now.algId || c.keyId != tel.now.keyId;
                if (algChanged) {
                    tel.now.algId = c.algId; tel.now.keyId = c.keyId;
                    tel.now.encrypted = c.algId != p25::kAlgClear;
                    if (tel.now.encrypted) { char b[120]; snprintf(b, sizeof b, "LDU2 encrypted: %s (0x%02X), key %d", p25::algName(c.algId), c.algId, c.keyId); event(b); }
                }
                // The message indicator updates each LDU2: reseed the keystream (OFB / ADP) from it, or match a key the first time.
                std::memcpy(curMi, c.mi, sizeof curMi);
                if (algChanged || !triedKey) rebuildDecryptor();
                else if (dec) dec->setIv(curMi, sizeof curMi);
            }
        }
        if (!tel.now.active) return;
        lastVoice = sigSec();
        for (int k = 0; k < 9; k++) {
            uint8_t dib[72];
            for (int i = 0; i < 72; i++) dib[i] = (uint8_t)hardDibit(dl[P + (size_t)vcOff[k] + (size_t)i]);
            voiceFrame(dib);
        }
    }

    void term(const Pending& f, bool lc) {
        if (lc) {
            uint8_t bits[288];
            for (int i = 0; i < 144; i++) { const int dd = hardDibit(dl[56 + (size_t)i]); bits[2 * i] = (uint8_t)(dd >> 1); bits[2 * i + 1] = (uint8_t)(dd & 1); }
            uint8_t s[24];
            for (int w = 0; w < 12; w++) {
                const uint32_t cw = (uint32_t)bitsVal(bits + 24 * w, 24);
                int dd = p25::golay24Decode(cw);
                if (dd < 0) dd = (int)(cw >> 12);
                s[2 * w] = (uint8_t)(dd >> 6); s[2 * w + 1] = (uint8_t)(dd & 63);
            }
            const bool ok = p25::rsDecode(s, 24, 12);
            (ok ? tel.lcOk : tel.lcBad)++;
        }
        (void)f;
        if (tel.now.active) endCall(true);
    }

    int tsbks(const Pending& f, size_t P) {
        int n = 0;
        for (int blk = 0; blk < 3; blk++) {
            float lv[98];
            for (int i = 0; i < 98; i++) lv[i] = dl[P + (size_t)(98 * blk + i)];
            uint8_t bits[96];
            p25::Tsbk t;
            if (!p25::trellisDecode(lv, bits) || !p25::tsbkParse(bits, t)) { tel.tsbkBad++; break; }
            tel.tsbkOk++; n++;
            lastTsbk = sigSec();
            onTsbk(t, f.nac);
            if (t.last) break;
        }
        return n;
    }

    int pdu(const Pending& f, size_t P) {
        float lv[98];
        for (int i = 0; i < 98; i++) lv[i] = dl[P + (size_t)i];
        uint8_t b[96];
        if (!p25::trellisDecode(lv, b) || (uint16_t)bitsVal(b + 80, 16) != p25::crcCcitt(b, 80)) return 0;
        const int fmt = (int)bitsVal(b + 3, 5), sap = (int)bitsVal(b + 10, 6), llid = (int)bitsVal(b + 24, 24), blocks = (int)bitsVal(b + 49, 7);
        const bool out = b[2];
        P25Call c;
        c.wallTime = (int64_t)time(nullptr); c.startSec = sigSec(); c.kind = 2; c.nac = f.nac; c.endSeen = true;
        if (out) c.dst = llid; else c.src = llid;
        c.group = false;
        char t[160];
        snprintf(t, sizeof t, "data packet, SAP %d, %d blocks (%s, format 0x%02X)", sap, blocks, out ? "outbound" : "inbound", fmt);
        c.text = t;
        pushLog(c);
        snprintf(t, sizeof t, "PDU %s LLID %d, SAP %d, %d blocks", out ? "to" : "from", llid, sap, blocks);
        event(t);
        return 1;
    }

    void grant(int tg, int src, int ch, bool group, int opts) {
        const double now = sigSec();
        for (auto& g : grants)
            if (g.tg == tg && g.src == src && g.ch == ch && g.group == group && now - g.last < 4) {
                g.last = now;
                for (size_t i = 0; i < std::min<size_t>(tel.log.size(), 30); i++) {
                    P25Call& c = tel.log[i];
                    if (c.kind == 1 && c.dst == tg && c.src == src && c.group == group) { c.durSec = now - c.startSec; break; }
                }
                return;
            }
        grants.erase(std::remove_if(grants.begin(), grants.end(), [&](const Grant& g) { return now - g.last > 10; }), grants.end());
        grants.push_back({tg, src, ch, group, now});
        P25Call c;
        c.wallTime = (int64_t)time(nullptr); c.startSec = now; c.kind = 1; c.src = src; c.dst = tg; c.group = group; c.channel = ch; c.freqHz = freqOf(ch);
        c.emergency = (opts & 0x80) != 0; c.encrypted = (opts & 0x40) != 0; c.nac = tel.nac; c.endSeen = true;
        pushLog(c);
        if (group) touchTg(tg, src, c.freqHz, c.encrypted, c.emergency, true, false);
        char b[200];
        snprintf(b, sizeof b, "%s %d -> %s%d on channel %s%s%s%s", group ? "GRP_V_CH_GRANT" : "UU_V_CH_GRANT", src, group ? "TG " : "", tg, p25::channelText(ch).c_str(),
                 c.freqHz > 0 ? (" (" + std::to_string(c.freqHz / 1e6).substr(0, 10) + " MHz)").c_str() : "", c.emergency ? ", EMERGENCY" : "", c.encrypted ? ", encrypted" : "");
        event(b);
    }

    void onTsbk(const p25::Tsbk& t, int nac) {
        tel.control = true;
        {
            static std::atomic<uint64_t> seq{0};
            P25TsbkRec r;
            r.seq = ++seq; r.nac = nac; r.opcode = t.opcode; r.mfid = t.mfid; r.args = t.args; r.sigSec = sigSec();
            tel.tsbks.push_back(r);
            if (tel.tsbks.size() > kP25TsbkMax) tel.tsbks.erase(tel.tsbks.begin(), tel.tsbks.end() - (long)kP25TsbkMax);
        }
        if (t.mfid != 0 && t.mfid != 1) return;   // a manufacturer's own
        const uint64_t a = t.args;
        char b[200];
        switch (t.opcode) {
        case p25::kGrpVGrant: grant((int)((a >> 24) & 0xFFFF), (int)(a & 0xFFFFFF), (int)((a >> 40) & 0xFFFF), true, (int)(a >> 56)); break;
        case p25::kUuVGrant: grant((int)((a >> 24) & 0xFFFFFF), (int)(a & 0xFFFFFF), (int)((a >> 48) & 0xFFFF), false, 0); break;
        case p25::kGrpVGrantUpd: {
            const int ch1 = (int)((a >> 48) & 0xFFFF), g1 = (int)((a >> 32) & 0xFFFF), ch2 = (int)((a >> 16) & 0xFFFF), g2 = (int)(a & 0xFFFF);
            touchTg(g1, -1, freqOf(ch1), false, false, false, false);
            if (g2 != g1) touchTg(g2, -1, freqOf(ch2), false, false, false, false);
            break;
        }
        case p25::kIdenUp: case p25::kIdenUpVu: {
            const int id = (int)(a >> 60) & 15;
            P25IdenRow r;
            r.valid = true; r.vu = t.opcode == p25::kIdenUpVu;
            r.baseHz = (double)(a & 0xFFFFFFFFULL) * 5;
            r.spacingHz = (double)((a >> 32) & 0x3FF) * 125;
            if (r.vu) {
                const int bw = (int)(a >> 56) & 15, toff = (int)((a >> 42) & 0x3FFF);
                r.bwHz = bw == 4 ? 6250 : bw == 5 ? 12500 : 0;
                r.txOffsetHz = ((toff >> 13) & 1 ? 1 : -1) * (double)(toff & 0x1FFF) * r.spacingHz;
            } else {
                const int bw = (int)(a >> 51) & 0x1FF, toff = (int)((a >> 42) & 0x1FF);
                r.bwHz = bw * 125.0;
                r.txOffsetHz = ((toff >> 8) & 1 ? 1 : -1) * (double)(toff & 0xFF) * 250000;
            }
            P25IdenRow& o = tel.idens[id];
            const bool news = !o.valid || o.baseHz != r.baseHz || o.spacingHz != r.spacingHz;
            o = r;
            if (news) { snprintf(b, sizeof b, "%s %d: base %.5f MHz, spacing %.2f kHz, transmit offset %+.3f MHz", p25::opcodeName(t.opcode), id, r.baseHz / 1e6, r.spacingHz / 1e3, r.txOffsetHz / 1e6); event(b); }
            break;
        }
        case p25::kNetSts: {
            P25Site& s = tel.site;
            const int wacn = (int)((a >> 36) & 0xFFFFF), sys = (int)((a >> 24) & 0xFFF), ch = (int)((a >> 8) & 0xFFFF);
            const bool news = s.wacn != wacn || s.sysId != sys;
            s.valid = true; s.wacn = wacn; s.sysId = sys; s.channel = ch; s.freqHz = freqOf(ch); s.ssc = (int)(a & 0xFF); s.nac = nac; s.wallTime = (int64_t)time(nullptr);
            if (news) { snprintf(b, sizeof b, "NET_STS_BCST WACN %05X system %03X, NAC %03X, control channel %s", wacn, sys, nac, p25::channelText(ch).c_str()); event(b); }
            break;
        }
        case p25::kRfssSts: {
            P25Site& s = tel.site;
            const int sys = (int)((a >> 40) & 0xFFF), rf = (int)((a >> 32) & 0xFF), st = (int)((a >> 24) & 0xFF), ch = (int)((a >> 8) & 0xFFFF);
            const bool news = s.rfss != rf || s.site != st;
            s.valid = true; s.sysId = sys; s.rfss = rf; s.site = st; s.channel = ch; s.freqHz = freqOf(ch); s.ssc = (int)(a & 0xFF); s.nac = nac; s.wallTime = (int64_t)time(nullptr);
            if (news) { snprintf(b, sizeof b, "RFSS_STS_BCST system %03X, RFSS %d, site %d", sys, rf, st); event(b); }
            break;
        }
        case p25::kAdjSts: {
            P25Site s;
            s.valid = true; s.sysId = (int)((a >> 40) & 0xFFF); s.rfss = (int)((a >> 32) & 0xFF); s.site = (int)((a >> 24) & 0xFF);
            s.channel = (int)((a >> 8) & 0xFFFF); s.freqHz = freqOf(s.channel); s.ssc = (int)(a & 0xFF); s.wallTime = (int64_t)time(nullptr);
            s.wacn = tel.site.wacn;
            auto it = std::find_if(tel.adjacent.begin(), tel.adjacent.end(), [&](const P25Site& o) { return o.sysId == s.sysId && o.rfss == s.rfss && o.site == s.site; });
            if (it != tel.adjacent.end()) *it = s;
            else {
                tel.adjacent.push_back(s);
                snprintf(b, sizeof b, "ADJ_STS_BCST system %03X, RFSS %d, site %d on channel %s", s.sysId, s.rfss, s.site, p25::channelText(s.channel).c_str());
                event(b);
            }
            break;
        }
        default: break;
        }
    }

    void voiceFrame(const uint8_t dib[72]) {
        char fr[8][23];
        p25::imbeDeinterleave(dib, fr);
        tel.now.voiceFrames++;
        tel.voiceFrames++;
        bool clear = !tel.now.encrypted;
        // Encrypted, and the user loaded a matching key: decrypt the voice bits in place (never a search). Without a key, dec is null and
        // the frame is reported and skipped, exactly as before.
        if (!clear && dec) {
            uint8_t bits[144];
            p25::imbeFlatten(fr, bits);
            if (dec->decryptFrame(bits)) { p25::imbeUnflatten(bits, fr); clear = true; }
        }
        if (vtap) { uint8_t bits[144]; p25::imbeFlatten(fr, bits); vtap(bits, clear); }
        voice::Monitor* mon = activeMonitor();
        if (mon) {   // the voice monitor (voice_monitor.h) lists the call and says whether it is played
            voice::CallInfo ci;
            ci.src = tel.now.src > 0 ? (uint32_t)tel.now.src : 0; ci.dst = tel.now.dst > 0 ? (uint32_t)tel.now.dst : 0;
            ci.group = tel.now.group; ci.emergency = tel.now.emergency; ci.encrypted = !clear; ci.codecMissing = !imbe || !imbe->ok();
            ci.errorRate = berBits > 1000 ? (float)berErr / (float)berBits : -1.f;
            const int nac = tel.now.nac >= 0 ? tel.now.nac : tel.nac;
            if (nac >= 0) { char b[16]; snprintf(b, sizeof b, "NAC %03X", nac); ci.access = b; }
            if (mon->offer(ci) != voice::Verdict::Play) return;
            playing = ci;
        }
        if (!clear) return;   // encrypted with no usable key: reported, never decoded
        tel.now.clearFrames++;
        if (!imbe || !imbe->ok()) return;
        int16_t pcm[160];
        imbe->decode(fr, pcm);
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

    // The call log, talkgroups, control messages and the site with its neighbours stay through resets: Clear and a loaded file reach the feed thread's copy through these requests (mu), and the
    // published report at once
    bool clearReq = false, loadReq = false;
    P25Telemetry loaded;
    double loadedAt = 0;                   // the report's clock the loaded lists are on
    static void moveLists(P25Telemetry& to, P25Telemetry& from) {
        to.adjacent = std::move(from.adjacent); to.talkgroups = std::move(from.talkgroups); to.log = std::move(from.log); to.events = std::move(from.events); to.site = from.site;
        from.adjacent.clear(); from.talkgroups.clear(); from.log.clear(); from.events.clear();
    }
    static void shiftTimes(P25Telemetry& t, double dt) {
        for (auto& c : t.log) c.startSec += dt;
    }
    void takeRequests() {   // feed thread
        std::lock_guard<std::mutex> lk(mu);
        if (clearReq) { P25Telemetry e; moveLists(tel, e); clearReq = false; }
        if (loadReq) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); loadReq = false; }
    }
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        if (tel.now.active) endCall(false);
        initFront();
        initFilter();
        std::fill(y.begin(), y.end(), 0.f); yN = 0;
        candN = -1; candR = 0; pend.clear(); busyUntil = -1;
        power = 0; nPower = 0; nIn = 0; nextReport = 0.25 * rate;
        lastGood = lastTsbk = lastVoice = -10; berErr = berBits = 0; snrAvg = 0;
        grants.clear();
        const uint64_t seq = tel.seq;
        P25Telemetry keep;                          // the lists stay (clearResults): the clock starts again, they move back by the time it had reached
        moveLists(keep, tel);
        shiftTimes(keep, -tel.timeSec);
        tel = P25Telemetry(); tel.seq = seq;
        moveLists(tel, keep);
        tel.inputRate = rate;
        tel.imbe = p25ImbeAvailable(&tel.imbePath);
    }

    void report() {
        if (tapReq.exchange(false)) { std::lock_guard<std::mutex> lk(mu); tap = tapSet; vtap = vtapSet; }
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        if (!tel.imbe && tel.seq % 40 == 0) tel.imbe = p25ImbeAvailable(&tel.imbePath);   // installed while running ("Look again")
        if (tel.now.active && sigSec() - lastVoice > 1.0) endCall(false);
        if (tel.now.active) tel.now.durSec = sigSec() - tel.now.startSec;
        tel.control = sigSec() - lastTsbk < 3.0;
        const int64_t wall = (int64_t)time(nullptr);
        for (auto& t : tel.talkgroups) t.active = wall - t.lastWall < 3 || (tel.now.active && tel.now.group && tel.now.dst == t.tg);
        tel.state = tel.now.active ? 2 : sigSec() - lastGood < 1.5 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

P25Receiver::P25Receiver() : p_(std::make_unique<Impl>()) {}
P25Receiver::~P25Receiver() = default;

void P25Receiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void P25Receiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
bool P25Receiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= p25Tuning().minSampleRate - 1; }
void P25Receiver::reset() { p_->resetReq = true; }

void P25Receiver::feed(const cf32* x, size_t n) {
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

void P25Receiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    P25Telemetry e;
    Impl::moveLists(p_->pub, e);
    p_->clearReq = true; p_->loadReq = false;
    p_->pub.seq++;
}
std::string P25Receiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.adjacent, p_->pub.talkgroups, p_->pub.log, p_->pub.events, p_->pub.site);
}
bool P25Receiver::loadResults(const std::string& blob) {
    double t0 = 0;
    P25Telemetry l;
    if (!results::unpack(blob, t0, l.adjacent, l.talkgroups, l.log, l.events, l.site)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    P25Telemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->loadReq = true;
    p_->pub.seq++;
    return true;
}
bool P25Receiver::telemetry(P25Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void P25Receiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void P25Receiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void P25Receiver::setMuted(bool m) { p_->muted = m; }
void P25Receiver::setSilent(bool s) { p_->silent = s; }
void P25Receiver::setVoiceMonitor(voice::Monitor* m) { p_->monSet = m; }
void P25Receiver::setAudioTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tapSet = std::move(cb); p_->tapReq = true; }
void P25Receiver::setVoiceTap(std::function<void(const uint8_t bits[144], bool clear)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->vtapSet = std::move(cb); p_->tapReq = true; }

ModeTuning p25Tuning() {
    ModeTuning t;
    t.stdMode = 52; t.id = "p25"; t.name = "P25";
    t.minMhz = 30; t.maxMhz = 1000; t.defMhz = 851.0125;   // a channel of the 800 MHz public safety band (VHF, UHF and 700 MHz systems too)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.0125;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike
    return t;
}

} // namespace dect2

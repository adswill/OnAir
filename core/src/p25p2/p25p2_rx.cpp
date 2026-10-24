// P25 Phase 2 receiver (see p25p2_rx.h; the protocol layout is in p25p2_proto.h and is this program's own reading of public descriptions).
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/dsp/cic3.h"
#include "dect2/p25p2/p25p2_proto.h"
#include "dect2/p25/p25_proto.h"
#include "dect2/p25p2/p25p2_tel.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include "dect2/audio/audioout.h"
#include "dect2/voice/voice_monitor.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kFs = 48000;                 // working rate: 8 samples per symbol
constexpr int kSps = 8;
constexpr int kTaps = 65;                     // the root raised cosine matched filter, +-4 symbols
constexpr int kRing = 256, kRMask = kRing - 1;
constexpr uint64_t kMask40 = (1ULL << 40) - 1;
constexpr int kSyncMax = 4;                   // bit errors allowed in the 40-bit sync word
constexpr size_t kMaxEvents = 200;

float wrapPi(float a) { while (a > (float)kPi) a -= (float)(2 * kPi); while (a < -(float)kPi) a += (float)(2 * kPi); return a; }
} // namespace

struct P25p2Receiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true};
    std::function<void(const std::string&)> log;
    std::function<void(const float*, size_t)> tapSet, tap;
    std::function<void(int, const uint8_t*, bool)> vtapSet, vtap;
    std::atomic<bool> tapReq{false};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    std::atomic<int> listen{0};
    P25p2Telemetry pub, tel;

    double rate = 0, offset = 0;
    double power = 0; int64_t nPower = 0, nIn = 0;
    double nextReport = 0;
    double sigSec() const { return rate > 0 ? (double)nIn / rate : 0; }

    // ------------------------------------------------------------ front end: mix, CIC, windowed-sinc to 48 kHz (complex)
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
        M = std::max(8, (int)std::ceil(8.0 * fd / 6000.0 / 2));
        fir.assign((size_t)nPh * 2 * M, 0.f);
        const double fc = std::min(0.45, 7000.0 / std::max(1.0, fd));
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
                sample(acc);
            }
        }
    }

    // ------------------------------------------------------------ symbol filter, parallel differential demodulators, sync search
    float h[kTaps];
    cf32 zr[kRing], yr[kRing];
    float angr[kRing];
    uint64_t reg[kSps] = {};
    int64_t n = 0;                 // 48 kHz sample count
    double cfoStep = 0;            // rad per symbol of carrier offset
    bool win = false;
    int64_t winStart = 0;
    int winDist[kSps] = {};
    // the lock: bursts of 180 dibits from the sync word on (as XMC_SDR P25_P2_Decode)
    bool locked = false;
    int64_t nextDibitN = 0, syncEndPred = 0, pendingShift = 0;   // syncEndPred: a sample at which a sync word ends, modulo one burst
    int slotIdx = 0, slotPos = 0, dibSinceSync = 0;              // slotIdx: the slot of the superframe (0..11) the burst being received is taken to be
    uint8_t buf[p25p2::kBurstDibits];
    double lastSync = -10, snrAvg = 0;
    static constexpr int kLostDibits = 2 * p25p2::kSlotsPerSuperframe * p25p2::kSlotDibits;   // no sync word for two superframes: the lock is lost

    void initFilter() {
        double sum = 0;
        for (int k = 0; k < kTaps; k++) { h[k] = (float)p25p2::rrcImpulse((double)(k - kTaps / 2) / kSps, p25p2::kRollOff); sum += h[k]; }
        for (float& v : h) v = (float)(v / sum);
        for (auto& v : zr) v = cf32(0, 0);
        for (auto& v : yr) v = cf32(0, 0);
        for (auto& v : angr) v = 0;
        for (auto& r : reg) r = 0;
        n = 0; win = false; locked = false; cfoStep = 0; slotIdx = slotPos = dibSinceSync = 0;
    }

    void sample(cf32 z) {
        zr[n & kRMask] = z;
        cf32 y(0, 0);
        for (int k = 0; k < kTaps; k++) y += zr[(n - k) & kRMask] * h[k];
        yr[n & kRMask] = y;
        if (n >= kSps) {
            const cf32 d = y * std::conj(yr[(n - kSps) & kRMask]);
            const float ang = std::atan2(d.imag(), d.real());
            angr[n & kRMask] = ang;
            const float a2 = wrapPi(ang - (float)cfoStep);
            const int step = a2 >= (float)(kPi / 2) ? 3 : a2 >= 0 ? 1 : a2 >= -(float)(kPi / 2) ? -1 : -3;
            const int dib = p25p2::stepDibit(step);
            const int ph = (int)(n & 7);
            reg[ph] = ((reg[ph] << 2) | (uint64_t)dib) & kMask40;
            const int dist = __builtin_popcountll(reg[ph] ^ p25p2::kSyncWord);
            if (win) winDist[n - winStart] = dist;
            else if (dist <= kSyncMax) { win = true; winStart = n; for (int& v : winDist) v = 99; winDist[0] = dist; }
            if (win && n >= winStart + kSps - 1) { commitSync(); win = false; }
            if (locked && n == nextDibitN) { nextDibitN += kSps; takeDibit(dib); }
        }
        n++;
    }

    // the sync was seen between winStart and winStart + 7: the sample whose 20 residuals are the most alike is the symbol centre
    void commitSync() {
        int64_t bestN = -1;
        double bestVar = 1e9, bestMean = 0;
        for (int j = 0; j < kSps; j++) {
            if (winDist[j] > kSyncMax) continue;
            const int64_t m = winStart + j;
            double sx = 0, sy = 0;
            float res[p25p2::kIschDibits];
            for (int k = 0; k < p25p2::kIschDibits; k++) {
                const int64_t at = m - (int64_t)kSps * (p25p2::kIschDibits - 1 - k);
                const int d = (int)((p25p2::kSyncWord >> (38 - 2 * k)) & 3);
                res[k] = wrapPi(angr[at & kRMask] - (float)(p25p2::dibitStep(d) * kPi / 4));
                sx += std::cos(res[k]); sy += std::sin(res[k]);
            }
            const double mean = std::atan2(sy, sx);
            double var = 0;
            for (int k = 0; k < p25p2::kIschDibits; k++) { const double e = wrapPi(res[k] - (float)mean); var += e * e; }
            var /= p25p2::kIschDibits;
            if (var < bestVar) { bestVar = var; bestN = m; bestMean = mean; }
        }
        if (bestN < 0) return;
        tel.syncs++;
        lastSync = sigSec();
        const double snr = 10 * std::log10(1.0 / std::max(1e-4, bestVar));
        snrAvg = snrAvg == 0 ? snr : snrAvg + 0.3 * (snr - snrAvg);
        constexpr int64_t kB = (int64_t)p25p2::kBurstDibits * kSps;
        if (locked) {
            int64_t shift = ((bestN - syncEndPred) % kB + kB) % kB;
            if (shift > kB / 2) shift -= kB;
            if (std::llabs((long long)shift) <= 3) {
                pendingShift += shift;             // the symbol clock follows the sync, from the next burst on
                syncEndPred += shift;
                cfoStep += 0.5 * wrapPi((float)(bestMean - cfoStep));
                dibSinceSync = 0;
                return;
            }
            // a sync word off the burst boundary: the demodulator slipped and every burst since is misframed. Only an exact one re-frames a running
            // lock (a chance match in the payload is 2^-40 per position).
            if (winDist[bestN - winStart] != 0) return;
            tel.realigns++;
        } else cfoStep = bestMean;
        locked = true; dibSinceSync = 0;
        slotPos = p25p2::kIschDibits;            // the slot counter runs on: the I-ISCH puts it right
        p25p2::ischSync(buf);
        syncEndPred = bestN; pendingShift = 0;
        nextDibitN = bestN + kSps;
    }

    void takeDibit(int dib) {
        buf[slotPos++] = (uint8_t)dib;
        dibSinceSync++;
        if (slotPos < p25p2::kBurstDibits) return;
        processBurst();
        slotIdx = (slotIdx + 1) % p25p2::kSlotsPerSuperframe;
        slotPos = 0;
        nextDibitN += pendingShift; pendingShift = 0;
        if (dibSinceSync > kLostDibits) unlock();
    }

    void unlock() { locked = false; tel.unlocks++; slotPos = 0; }

    // ------------------------------------------------------------ events, tables
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
        if (log) log(s);
    }
    void pushLog(const P25p2Call& c) {
        tel.log.insert(tel.log.begin(), c);
        if (tel.log.size() > kP25p2LogMax) tel.log.pop_back();
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

    // ------------------------------------------------------------ calls
    double lastAct[2] = {-10, -10};
    struct Grant { int tg, src, ch; bool group; double last; };
    std::vector<Grant> grants;

    void startCall(int ci, int src, int dst, bool group, bool cryptoKnown, int alg, int svc, bool late) {
        P25p2Call& c = tel.now[ci];
        if (c.active) {
            if ((src < 0 || c.src == src) && (dst < 0 || c.dst == dst)) return;   // the same call
            if (late) return;
            endCall(ci, true);
        }
        c = P25p2Call();
        c.slot = ci + 1; c.active = true; c.wallTime = (int64_t)time(nullptr); c.startSec = sigSec();
        if (src < 0 && dst < 0 && hint[ci].dst >= 0) { src = hint[ci].src; dst = hint[ci].dst; group = hint[ci].group; }   // what the grant said
        c.src = src; c.dst = dst; c.group = group; c.lateEntry = late;
        c.cryptoKnown = cryptoKnown; c.algId = alg;
        c.encrypted = cryptoKnown && (alg != p25p2::kAlgClear && alg != 0);
        if (svc >= 0) { c.emergency = (svc & 0x80) != 0; if (svc & 0x40) { c.encrypted = true; c.cryptoKnown = true; } }
        lastAct[ci] = sigSec();
        if (group) touchTg(dst, src, 0, c.encrypted, c.emergency, false, true);
        char b[160];
        snprintf(b, sizeof b, "slot %d: %s %s -> %s%s%s%s", ci + 1, late ? "call joined" : "PTT", p25Id(src).c_str(), group ? "TG " : "", p25Id(dst).c_str(),
                 c.emergency ? ", EMERGENCY" : "", c.encrypted ? ", encrypted" : "");
        event(b);
    }
    void endCall(int ci, bool seen) {
        P25p2Call& c = tel.now[ci];
        if (!c.active) return;
        c.active = false; c.endSeen = seen;
        c.durSec = sigSec() - c.startSec;
        pushLog(c);
        char b[160];
        snprintf(b, sizeof b, "slot %d: %s, %.1f s, %u voice frames", ci + 1, seen ? "call ended" : "signal lost", c.durSec, (unsigned)c.voiceFrames);
        event(b);
        if (tel.audioSlot == ci + 1) tel.audioSlot = 0;
        held[ci].clear();
        ess[ci].nb = 0;
        flushAudio();
    }
    // fills in what a voice channel user message tells about the call on the slot (starting it when it is not known yet). Its service options can
    // say encrypted, never clear: the NSW PSN sends AES calls without the bit (the PTT or the ESS decides).
    void userMsg(int ci, int svc, int dst, int src, bool group) {
        lastAct[ci] = sigSec();
        P25p2Call& c = tel.now[ci];
        if (!c.active) { startCall(ci, src, dst, group, false, p25p2::kAlgClear, svc, true); return; }
        if (c.src < 0) c.src = src;
        if (c.dst < 0) { c.dst = dst; c.group = group; }
        else if (!group && c.group) { c.dst = dst; c.group = false; }   // the PTT carries a group address; a unit to unit user message corrects it
        c.emergency = c.emergency || (svc & 0x80) != 0;
        if (svc & 0x40) { c.encrypted = true; c.cryptoKnown = true; }
        if (group) touchTg(dst, src, 0, c.encrypted, c.emergency, false, false);
    }

    void grant(int tg, int src, int ch, bool group, int opts) {
        const double now = sigSec();
        for (auto& g : grants)
            if (g.tg == tg && g.src == src && g.ch == ch && g.group == group && now - g.last < 4) {
                g.last = now;
                if (freqOf(ch) > 0)   // the identifier update came after the first grant: the log entry gets its frequency now
                    for (auto& e : tel.log) if (e.kind == 1 && e.channel == ch && e.dst == tg && e.src == src && e.freqHz <= 0) { e.freqHz = freqOf(ch); break; }
                return;
            }
        grants.erase(std::remove_if(grants.begin(), grants.end(), [&](const Grant& g) { return now - g.last > 10; }), grants.end());
        grants.push_back({tg, src, ch, group, now});
        P25p2Call c;
        c.wallTime = (int64_t)time(nullptr); c.startSec = now; c.kind = 1; c.src = src; c.dst = tg; c.group = group; c.channel = ch; c.freqHz = freqOf(ch);
        c.emergency = (opts & 0x80) != 0; c.encrypted = (opts & 0x40) != 0; c.cryptoKnown = true; c.endSeen = true;
        pushLog(c);
        if (group) touchTg(tg, src, c.freqHz, c.encrypted, c.emergency, true, false);
        char b[200];
        snprintf(b, sizeof b, "%s %d -> %s%d on channel %s%s%s%s", group ? "GRP_V_CH_GRANT" : "UU_V_CH_GRANT", src, group ? "TG " : "", tg, p25::channelText(ch).c_str(),
                 c.freqHz > 0 ? (" (" + std::to_string(c.freqHz / 1e6).substr(0, 10) + " MHz)").c_str() : "", c.emergency ? ", EMERGENCY" : "", c.encrypted ? ", encrypted" : "");
        event(b);
    }

    // the TSBK-equivalent MAC messages: the 64 bits after the opcode have the layout of the Phase 1 TSBK arguments
    void controlMsg(int op, uint64_t a) {
        char b[200];
        switch (op) {
        case p25p2::kMsgGrpVGrant: grant((int)((a >> 24) & 0xFFFF), (int)(a & 0xFFFFFF), (int)((a >> 40) & 0xFFFF), true, (int)(a >> 56)); break;
        case p25p2::kMsgUuVGrant: grant((int)((a >> 24) & 0xFFFFFF), (int)(a & 0xFFFFFF), (int)((a >> 48) & 0xFFFF), false, 0); break;
        case p25p2::kMsgGrpVGrantUpd: {
            const int ch1 = (int)((a >> 48) & 0xFFFF), g1 = (int)((a >> 32) & 0xFFFF), ch2 = (int)((a >> 16) & 0xFFFF), g2 = (int)(a & 0xFFFF);
            touchTg(g1, -1, freqOf(ch1), false, false, false, false);
            if (g2 != g1) touchTg(g2, -1, freqOf(ch2), false, false, false, false);
            break;
        }
        case p25p2::kMsgIdenUp: case p25p2::kMsgIdenUpVu: {
            const int id = (int)(a >> 60) & 15;
            const bool vu = op == p25p2::kMsgIdenUpVu;
            P25IdenRow r;
            r.valid = true; r.vu = vu;
            r.baseHz = (double)(a & 0xFFFFFFFFULL) * 5;
            r.spacingHz = (double)((a >> 32) & 0x3FF) * 125;
            if (vu) {
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
            if (news) { snprintf(b, sizeof b, "%s %d: base %.5f MHz, spacing %.2f kHz, transmit offset %+.3f MHz", p25p2::macMsgName(op), id, r.baseHz / 1e6, r.spacingHz / 1e3, r.txOffsetHz / 1e6); event(b); }
            break;
        }
        case p25p2::kMsgNetSts: {
            P25Site& s = tel.site;
            const int wacn = (int)((a >> 36) & 0xFFFFF), sys = (int)((a >> 24) & 0xFFF), ch = (int)((a >> 8) & 0xFFFF);
            const bool news = s.wacn != wacn || s.sysId != sys;
            s.valid = true; s.wacn = wacn; s.sysId = sys; s.channel = ch; s.freqHz = freqOf(ch); s.ssc = (int)(a & 0xFF); s.wallTime = (int64_t)time(nullptr);
            if (news) { snprintf(b, sizeof b, "NET_STS_BCST WACN %05X system %03X, control channel %s", wacn, sys, p25::channelText(ch).c_str()); event(b); }
            break;
        }
        case p25p2::kMsgRfssSts: {
            P25Site& s = tel.site;
            const int sys = (int)((a >> 40) & 0xFFF), rf = (int)((a >> 32) & 0xFF), st = (int)((a >> 24) & 0xFF), ch = (int)((a >> 8) & 0xFFFF);
            const bool news = s.rfss != rf || s.site != st;
            s.valid = true; s.sysId = sys; s.rfss = rf; s.site = st; s.channel = ch; s.freqHz = freqOf(ch); s.ssc = (int)(a & 0xFF); s.wallTime = (int64_t)time(nullptr);
            if (news) { snprintf(b, sizeof b, "RFSS_STS_BCST system %03X, RFSS %d, site %d", sys, rf, st); event(b); }
            break;
        }
        case p25p2::kMsgAdjSts: {
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

    void macPdu(int ci, const uint8_t* m, int len) {
        const p25p2::MacHeader hd = p25p2::macHeader(m[0]);
        lastGood = sigSec();
        switch (hd.opcode) {
        case p25p2::kMacPtt:
            if (len >= p25p2::kPttLen) {
                const int alg = m[p25p2::kPttAlgOff];   // the message indicator and key ID next to it are never read
                P25p2Call& c = tel.now[ci];
                const int src = (int)p25p2::be24(m + p25p2::kPttSrcOff), grp = (int)p25p2::be16(m + p25p2::kPttGrpOff);
                if (c.active && !c.lateEntry && c.src == src && c.dst == grp) { lastAct[ci] = sigSec(); break; }
                if (c.active && c.lateEntry) { c.src = src; c.dst = grp; c.group = true; c.lateEntry = false; c.algId = alg; c.cryptoKnown = true; c.encrypted = alg != p25p2::kAlgClear && alg != 0; lastAct[ci] = sigSec(); touchTg(grp, src, 0, c.encrypted, c.emergency, false, true); break; }
                startCall(ci, src, grp, true, true, alg, -1, false);
            }
            break;
        case p25p2::kMacEndPtt:
            if (len >= p25p2::kEndLen) {
                tel.nac = (int)(p25p2::be16(m + p25p2::kEndColorOff) >> 4);
                if (tel.now[ci].active) endCall(ci, true);
            }
            break;
        case p25p2::kMacIdle:
            if (tel.now[ci].active) endCall(ci, true);
            break;
        case p25p2::kMacSignal: case p25p2::kMacActive: case p25p2::kMacHangtime: {
            int pos = 1;
            while (pos < len) {
                const int op = m[pos], ml = p25p2::macMsgLen(op);
                if (op == p25p2::kMsgNull || ml == 0 || pos + ml > len) break;
                if (op == p25p2::kMsgGrpVUser) userMsg(ci, m[pos + 1], (int)p25p2::be16(m + pos + 2), (int)p25p2::be24(m + pos + 4), true);
                else if (op == p25p2::kMsgUuVUser) userMsg(ci, m[pos + 1], (int)p25p2::be24(m + pos + 2), (int)p25p2::be24(m + pos + 5), false);
                else controlMsg(op, p25p2::be64(m + pos + 1));
                pos += ml;
            }
            break;
        }
        default: break;
        }
    }

    // ------------------------------------------------------------ slots and voice
    double lastGood = -10;
    std::unique_ptr<NxdnAmbe> ambe[2];
    std::vector<float> aud8, pcmOut;
    float upPrev = 0;
    std::unique_ptr<AudioOut> audioOut;

    struct Frame { char fr[4][24]; };
    // voice of a call whose encryption is not known yet (joined late, no ESS yet): played once the ESS says clear. The grant on the control channel
    // is no guide: the NSW PSN grants AES calls without the encryption bit in the service options.
    std::vector<Frame> held[2];
    bool vocUsed[2] = {false, false};  // the slot's vocoder has state from a call (a FACCH resets it)

    void voiceFrame(int ci, const char fr[4][24]) {
        uint8_t bits[72];
        p25p2::ambeFlatten(fr, bits);
        lastAct[ci] = sigSec();
        tel.voiceSec[ci] = sigSec();
        if (!tel.now[ci].active) {
            startCall(ci, -1, -1, true, false, p25p2::kAlgClear, -1, true);
        }
        P25p2Call& c = tel.now[ci];
        c.voiceFrames++;
        tel.voiceFrames++;
        const bool clear = c.cryptoKnown && !c.encrypted;
        if (vtap) vtap(ci + 1, bits, clear);
        if (c.encrypted) { tel.encryptedSkipped++; offer(ci, true); return; }   // reported (and listed in the voice monitor), never decoded
        if (!c.cryptoKnown) {
            if (held[ci].size() < 2 * 18) { Frame f; std::copy_n(&fr[0][0], 4 * 24, &f.fr[0][0]); held[ci].push_back(f); }
            return;
        }
        play(ci, fr);
    }

    // the ESS of a slot has decoded: the call's encryption is known now
    void essKnown(int ci, int alg, int key) {
        lastAct[ci] = sigSec();
        if (!tel.now[ci].active) startCall(ci, -1, -1, true, true, alg, -1, true);
        P25p2Call& c = tel.now[ci];
        const bool was = c.cryptoKnown && c.encrypted;
        c.cryptoKnown = true; c.algId = alg; c.keyId = key;
        c.encrypted = alg != p25p2::kAlgClear;
        if (c.encrypted && !was) {
            char b[96];
            snprintf(b, sizeof b, "slot %d: encrypted, algorithm 0x%02X key %d (not decoded)", ci + 1, alg, key);
            event(b);
            touchTg(c.dst, c.src, 0, true, c.emergency, false, false);
        }
        if (c.encrypted) tel.encryptedSkipped += held[ci].size();
        else for (const Frame& f : held[ci]) play(ci, f.fr);
        held[ci].clear();
    }

    struct Ess { uint8_t b[16] = {}, a[28] = {}; int nb = 0; };
    Ess ess[2];
    void essB(int ci, const uint8_t* d) {
        Ess& e = ess[ci];
        if (e.nb >= 4) e.nb = 0;
        p25p2::hexbitsGet(d + p25p2::kEssBPos, 4, e.b + 4 * e.nb);
        e.nb++;
    }
    void essA(int ci, const uint8_t* d) {
        Ess& e = ess[ci];
        p25p2::hexbitsGet(d + p25p2::kEssA1Pos, 16, e.a);
        p25p2::hexbitsGet(d + p25p2::kEssA2Pos, 12, e.a + 16);
        const bool full = e.nb == 4;      // the four ESS-B parts of the four 4V bursts before
        e.nb = 0;
        if (!full) return;
        int alg = 0, key = 0;
        uint8_t mi[9];
        if (!p25p2::essDecode(e.b, e.a, alg, key, mi)) { tel.essBad++; return; }
        tel.essOk++;
        essKnown(ci, alg, key);
    }

    // The voice monitor (voice_monitor.h, the "p25" one: Phase 1 and Phase 2 calls are one list, one played at a time) lists the call and says
    // whether it is played. Without a monitor (silent) the slot choice decides alone.
    struct Hint { int src = -1, dst = -1; bool group = true; double carrierHz = 0; };
    Hint hint[2], hintSet[2];
    bool hintReq = false;
    voice::CallInfo playing;
    voice::Monitor* activeMonitor() { return silent.load() ? nullptr : &voice::monitor("p25"); }
    voice::Verdict offer(int ci, bool encrypted) {
        voice::Monitor* mon = activeMonitor();
        if (!mon) return voice::Verdict::Play;
        const P25p2Call& c = tel.now[ci];
        voice::CallInfo info;
        info.src = c.src > 0 ? (uint32_t)c.src : 0; info.dst = c.dst > 0 ? (uint32_t)c.dst : 0;
        info.group = c.group; info.emergency = c.emergency; info.encrypted = encrypted;
        info.slot = ci + 1; info.carrierHz = hint[ci].carrierHz;
        info.codecMissing = !tel.codec;
        if (tel.colorCode > 0) { char b[24]; snprintf(b, sizeof b, "NAC %03X", tel.colorCode); info.access = b; }
        const voice::Verdict v = mon->offer(info);
        if (v == voice::Verdict::Play) playing = info;
        return v;
    }

    void play(int ci, const char fr[4][24]) {
        P25p2Call& c = tel.now[ci];
        c.clearFrames++;
        const int l = listen.load();
        if (l && l != ci + 1) return;   // the chosen slot only (the trunking follower's call)
        if (!l && !activeMonitor()) {   // no monitor: the first slot heard plays
            if (tel.audioSlot == 0 || !tel.now[tel.audioSlot - 1].active) tel.audioSlot = ci + 1;
            if (tel.audioSlot != ci + 1) return;
        }
        if (offer(ci, false) != voice::Verdict::Play) return;
        tel.audioSlot = ci + 1;
        if (!ambe[ci] && tel.codec) ambe[ci] = std::make_unique<NxdnAmbe>();
        if (!ambe[ci] || !ambe[ci]->ok()) return;
        int16_t pcm[160];
        ambe[ci]->decodeFr(fr, pcm);
        vocUsed[ci] = true;
        for (int16_t s : pcm) aud8.push_back(s / 32768.f);
        tel.voiceDecoded++;
        c.voice = true;
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

    // the descrambler (setSystem): dibit i of the burst in slot s is XORed with seq[s * 180 + i]
    uint8_t seq[p25p2::kScrambleDibits] = {};

    void processBurst() {
        tel.slots++;
        // the ISCH: the sync word (S-ISCH) or the I-ISCH with the logical channel and where in the superframe the burst is (as XMC_SDR)
        const uint64_t isch = p25p2::ischValue(buf);
        int ci = p25p2::slotChannel(slotIdx);
        if (__builtin_popcountll(isch ^ p25p2::kSyncWord) > kSyncMax) {
            int errs = 0;
            const int v = p25p2::ischDecode(isch, &errs);
            if (v < 0) tel.ischBad++;
            else {
                (errs ? tel.ischCorrected : tel.ischExact)++;
                // I-ISCH in slots 0, 1 (location 0), 4, 5 (1) and 8, 9 (2), the channel bit giving the slot of the pair; the sync word in the others.
                // (XMC_SDR sets slot 4 from channel 0 location 1; the rest is read off a recording of the NSW PSN.)
                const int ch = (v >> 5) & 3, loc = (v >> 3) & 3;
                if (loc < 3) slotIdx = 4 * loc + (ch & 1);
                ci = ch & 1;
            }
        }
        tel.slotNow = slotIdx;
        uint8_t d[p25p2::kBurstDibits];
        const uint8_t* sq = seq + (size_t)slotIdx * p25p2::kSlotDibits;
        for (int i = 0; i < p25p2::kBurstDibits; i++) d[i] = (uint8_t)(buf[i] ^ sq[i]);
        const int duid = p25p2::duidGet(buf);
        char fr[4][24];
        switch (duid) {
        case p25p2::kDuid4V:
            tel.v4++;
            essB(ci, d);
            for (int k = 0; k < 4; k++) { p25p2::ambeGet(d, p25p2::kVoiceOff[k], fr); voiceFrame(ci, fr); }
            break;
        case p25p2::kDuid2V:
            tel.v2++;
            essA(ci, d);
            for (int k = 0; k < 2; k++) { p25p2::ambeGet(d, p25p2::kVoiceOff[k], fr); voiceFrame(ci, fr); }
            break;
        case p25p2::kDuidSacch: case p25p2::kDuidFacch: case p25p2::kDuidSacchClear: case p25p2::kDuidFacchClear: {
            const bool facch = duid == p25p2::kDuidFacch || duid == p25p2::kDuidFacchClear;
            (facch ? tel.facch : tel.sacch)++;
            if (facch && vocUsed[ci]) { ambe[ci].reset(); vocUsed[ci] = false; }   // a call boundary: the vocoder starts afresh
            uint8_t pay[p25p2::kPayloadDibits];
            p25p2::payloadGet(duid == p25p2::kDuidSacchClear || duid == p25p2::kDuidFacchClear ? buf : d, pay);
            uint8_t mac[p25p2::kFacchMacBytes];
            int fixed = 0;
            if (p25p2::macDecode(facch, pay, mac, &fixed)) {
                tel.blocksOk++; tel.rsCorrected += (uint64_t)fixed;
                macPdu(ci, mac, facch ? p25p2::kFacchMacBytes : p25p2::kSacchMacBytes);
            } else tel.blocksBad++;
            break;
        }
        case p25p2::kDuidLcch: break;
        default: tel.slotsBad++; tel.duidBad++; break;
        }
    }

    // ------------------------------------------------------------ requests, report
    bool clearReq = false;
    bool sysReq = false;
    int sysWacn = 0, sysId = 0, sysNac = 0;
    void applySystem() {
        p25p2::scrambleSeq(sysWacn, sysId, sysNac, seq);
        tel.wacn = sysWacn; tel.sysId = sysId; tel.colorCode = sysNac;
    }
    static void moveLists(P25p2Telemetry& to, P25p2Telemetry& from) {
        to.adjacent = std::move(from.adjacent); to.talkgroups = std::move(from.talkgroups); to.log = std::move(from.log); to.events = std::move(from.events); to.site = from.site;
        for (int i = 0; i < 16; i++) to.idens[i] = from.idens[i];
        from.adjacent.clear(); from.talkgroups.clear(); from.log.clear(); from.events.clear(); from.site = P25Site();
    }
    void takeRequests() {
        std::lock_guard<std::mutex> lk(mu);
        if (clearReq) { P25p2Telemetry e; moveLists(tel, e); clearReq = false; }
        if (sysReq) { sysReq = false; applySystem(); }
        if (hintReq) { hintReq = false; hint[0] = hintSet[0]; hint[1] = hintSet[1]; }
    }
    uint64_t followTag = 0, followTagSet = 0;
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; followTag = followTagSet; }
        for (int i = 0; i < 2; i++) if (tel.now[i].active) endCall(i, false);
        initFront();
        initFilter();
        power = 0; nPower = 0; nIn = 0; nextReport = 0.25 * rate;
        lastSync = lastGood = -10; snrAvg = 0; lastAct[0] = lastAct[1] = -10;
        grants.clear();
        const uint64_t tseq = tel.seq;
        P25p2Telemetry keep;
        moveLists(keep, tel);
        for (auto& c : keep.log) c.startSec -= tel.timeSec;
        tel = P25p2Telemetry(); tel.seq = tseq;
        moveLists(tel, keep);
        { std::lock_guard<std::mutex> lk(mu); sysReq = false; applySystem(); }
        for (int i = 0; i < 2; i++) { held[i].clear(); ess[i].nb = 0; vocUsed[i] = false; }
        tel.inputRate = rate;
        tel.codec = nxdnAmbeAvailable(&tel.codecPath);
    }

    void report() {
        if (tapReq.exchange(false)) { std::lock_guard<std::mutex> lk(mu); tap = tapSet; vtap = vtapSet; }
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        if (!tel.codec && tel.seq % 40 == 0) tel.codec = nxdnAmbeAvailable(&tel.codecPath);
        for (int i = 0; i < 2; i++) {
            if (tel.now[i].active && sigSec() - lastAct[i] > 2.0) endCall(i, false);
            if (tel.now[i].active) tel.now[i].durSec = sigSec() - tel.now[i].startSec;
        }
        tel.locked = locked;
        tel.cfoHz = cfoStep * kSymRate() / (2 * kPi);
        tel.snrDb = (float)snrAvg;
        tel.listenSlot = listen.load();
        tel.followTag = followTag;
        const int64_t wall = (int64_t)time(nullptr);
        for (auto& t : tel.talkgroups) t.active = wall - t.lastWall < 3;
        tel.state = (tel.now[0].active || tel.now[1].active) ? 2 : locked ? 1 : 0;
        tel.dataValid = locked;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
    static double kSymRate() { return p25p2::kSymbolRate; }
};

P25p2Receiver::P25p2Receiver() : p_(std::make_unique<Impl>()) {}
P25p2Receiver::~P25p2Receiver() = default;

void P25p2Receiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void P25p2Receiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
bool P25p2Receiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= p25p2Tuning().minSampleRate - 1; }
void P25p2Receiver::reset() { p_->resetReq = true; }

void P25p2Receiver::feed(const cf32* x, size_t n) {
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

void P25p2Receiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    P25p2Telemetry e;
    Impl::moveLists(p_->pub, e);
    p_->clearReq = true;
    p_->pub.seq++;
}
bool P25p2Receiver::telemetry(P25p2Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void P25p2Receiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void P25p2Receiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void P25p2Receiver::setMuted(bool m) { p_->muted = m; }
void P25p2Receiver::setSilent(bool s) { p_->silent = s; }
void P25p2Receiver::setSystem(int wacn, int sysId, int nac) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->sysWacn = wacn & 0xFFFFF; p_->sysId = sysId & 0xFFF; p_->sysNac = nac & 0xFFF;
    p_->sysReq = true;
}
void P25p2Receiver::setCallHint(int slot, int src, int dst, bool group, double carrierHz) {
    if (slot < 1 || slot > 2) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Hint& h = p_->hintSet[slot - 1];
    h.src = src; h.dst = dst; h.group = group; h.carrierHz = carrierHz;
    p_->hintReq = true;
}
void P25p2Receiver::startFollow(uint64_t tag) {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->followTagSet = tag; }
    p_->resetReq = true;
}
void P25p2Receiver::setListenSlot(int s) { p_->listen = s < 0 || s > 2 ? 0 : s; }
void P25p2Receiver::setAudioTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tapSet = std::move(cb); p_->tapReq = true; }
void P25p2Receiver::setVoiceTap(std::function<void(int, const uint8_t*, bool)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->vtapSet = std::move(cb); p_->tapReq = true; }

ModeTuning p25p2Tuning() {
    ModeTuning t;
    t.stdMode = 182; t.id = "p25p2"; t.name = "P25 Phase 2";
    t.minMhz = 30; t.maxMhz = 1000; t.defMhz = 851.0125;   // the same bands as Phase 1 (VHF, UHF, 700 and 800 MHz)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.0125;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike
    return t;
}

} // namespace dect2

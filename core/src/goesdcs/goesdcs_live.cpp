// GOES DCS receiver for the engine (see goesdcs_live.h). Written from the public description of the DCP radio set standards; no code from other decoders.
//   Front    : fourth-order CIC decimator, the capture down to about 800 kHz (the 400 kHz uplink band)
//   Scanner  : averaged spectrum, local noise floor, carrier lines that persist
//   Slot     : one followed carrier: mixer, CIC to ~9.6 kHz, carrier PLL, data = quadrature part against the carrier,
//              five bit rate / coding hypotheses with eight bit phases each searching for preamble + frame sync, then bit timing loop and the report
#include "dect2/goesdcs/goesdcs_live.h"
#include "dect2/goesdcs/goesdcs_proto.h"
#include "dect2/goesdcs/goesdcs_tel.h"
#include "dcs_cic.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <mutex>

namespace dect2 {

namespace {

constexpr double kFrontRate = 800e3;
constexpr double kSlotRate = 9600;
constexpr int kMaxSlots = 8;
constexpr int kPhases = 8;

struct Hyp { int bps; bool man; };
const Hyp kHyps[] = {{100, true}, {300, true}, {1200, true}, {1200, false}, {300, false}};
constexpr int kNumHyp = 5;

struct Track {
    double tPrev = 0, tNext = 0;
    uint64_t hist = 0;
    uint32_t nb = 0;
    bool cand = false;                // matched the frame sync: collecting the address
    int pol = 0, ne = -1;
    std::vector<uint8_t> bits;
};

struct Slot {
    int id = 0;
    double freq = 0;                  // nominal offset from the centre, Hz
    cf32 ph{1, 0}, rot{1, 0};
    int renorm = 0;
    dcsdsp::Cic4 cic;
    double rs = kSlotRate;
    // carrier loop
    double theta = 0, integ = 0;
    cf32 mavg{0, 0};
    int acqLeft = 0;
    cf32 acqSum{0, 0}, prev{0, 0};
    bool havePrev = false;
    // the frequency estimate runs on a 16-sample average (first null 600 Hz): a platform on the next channel (1.5 or 3 kHz away) is in the slot's
    // 9.6 kHz band and would pull a plain sample-to-sample estimate
    cf32 box[16] = {}, boxSum{0, 0}, wHist[16] = {};
    int boxN = 0;
    float ref = 0;
    int lostRun = 0;
    // data
    uint64_t ns = 0;                  // baseband samples so far
    std::vector<double> cs;           // prefix sums of the data, ring
    Track tr[kNumHyp][kPhases];
    int state = 0;                    // 0 carrier / preamble search, 2 receiving
    int lockHyp = -1, lockTr = -1, pol = 0;
    int bps = 0;
    bool man = true;
    std::vector<uint8_t> bits;        // after the sync, polarity corrected
    double syncTimeSec = 0;
    float snr = 0;
    double lastSeenSec = 0, bornSec = 0;
    uint64_t unsyncedSamples = 0;
    int pending = 0;                  // sync candidates still collecting their address
    bool addrDone = false;
    uint32_t addrWord = 0;
    int addrFixed = 0;

    double S(double t) const {
        const int64_t i = (int64_t)std::floor(t);
        const double f = t - (double)i;
        const size_t m = cs.size() - 1;
        const double a = cs[(size_t)i & m], b = cs[(size_t)(i + 1) & m];
        return a + (b - a) * f;
    }
};

} // namespace

struct GoesDcsReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0, fw = 0;
    std::atomic<double> centreReq{0}, thrReq{11};
    std::atomic<int> planReq{-1};   // -1 Auto (from the centre), else dcs::Plan
    std::atomic<bool> resetReq{false}, clearReq{false}, logReq{false};
    std::atomic<int64_t> startReq{0};
    std::string logPath, logPathReq;
    // front
    dcsdsp::Cic4 front;
    int D = 1;
    std::vector<cf32> fbuf;
    // scanner
    int N = 0, M = 1, frames = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    std::vector<cf32> fr;
    int fill = 0;
    std::vector<double> acc;
    double binHz = 0;
    struct Cand { double f; int seen; double lastSnr; };
    std::vector<Cand> cands;
    struct Black { double f; double until; };
    std::vector<Black> black;
    std::vector<Slot> slots;
    int nextSlot = 1;
    // results
    std::vector<DcsMessage> msgs;
    std::vector<DcsPlatform> plats;
    uint32_t nextId = 1;
    uint64_t okN = 0, badN = 0, carriersSeen = 0;
    int64_t baseUtc = 0;
    uint64_t nIn = 0, nextPub = 0;
    double timeSec = 0;
    std::vector<uint8_t> wf;
    std::vector<float> spec;
    float noiseDb = -200;
    float bestSnr = 0;
    double bestCfo = 0;
    bool dirty = true;
    DcsTelemetry pub;

    dcs::Plan plan() const { const int p = planReq.load(); return p < 0 ? dcs::planForCentre(centreReq.load()) : (dcs::Plan)p; }

    static int64_t nowUtc() { return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

    void resetState() {
        fw = 0;
        slots.clear(); cands.clear(); black.clear();
        const double r = rateSet > 0 ? rateSet : 1e6;
        D = std::max(1, (int)std::floor(r / kFrontRate));
        front.configure(D);
        fw = r / D;
        double n = std::log2(std::max(1.0, fw / 100.0));
        N = 1 << std::max(8, std::min(14, (int)std::lround(n)));
        fft = std::make_unique<Fft>(N);
        win.resize((size_t)N);
        for (int i = 0; i < N; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * math::kPi * (i + 0.5) / N));
        fr.assign((size_t)N, cf32(0, 0));
        fill = 0; frames = 0;
        acc.assign((size_t)N, 0.0);
        binHz = fw / N;
        M = std::max(1, (int)std::lround(0.08 * fw / N));
        wf.assign((size_t)kDcsWfRows * kDcsWfCols, 0);
        spec.assign(kDcsWfCols, 0.f);
        const int64_t s = startReq.load();
        baseUtc = s ? s : nowUtc();
        nIn = 0; nextPub = 0; timeSec = 0;
        bestSnr = 0;
        dirty = true;
    }

    // ----- scanner -----
    void spectrumDone() {
        std::vector<double> p((size_t)N);   // shifted: index i is (i - N/2) * binHz
        for (int k = 0; k < N; k++) p[(size_t)((k + N / 2) % N)] = acc[(size_t)k] / (double)M;
        std::fill(acc.begin(), acc.end(), 0.0);
        // local noise floor: the 30th percentile of +-64 bins, every 16th bin, interpolated
        const int step = 16, half = 64;
        std::vector<double> fl;
        std::vector<double> w;
        for (int c = 0; c < N + step; c += step) {
            w.clear();
            for (int j = std::max(0, c - half); j <= std::min(N - 1, c + half); j++) w.push_back(p[(size_t)j]);
            if (w.empty()) { fl.push_back(1e-30); continue; }
            const size_t k = w.size() * 3 / 10;
            std::nth_element(w.begin(), w.begin() + (long)k, w.end());
            fl.push_back(std::max(1e-30, w[k]));
        }
        std::vector<double> floorv((size_t)N);
        for (int i = 0; i < N; i++) {
            const int c = i / step;
            const double fr_ = (double)(i - c * step) / step;
            floorv[(size_t)i] = fl[(size_t)c] * (1 - fr_) + fl[(size_t)std::min<int>((int)fl.size() - 1, c + 1)] * fr_;
        }
        std::vector<double> fs = floorv;
        std::nth_element(fs.begin(), fs.begin() + (long)fs.size() / 2, fs.end());
        const double medFloor = fs[fs.size() / 2];
        noiseDb = (float)(10 * std::log10(medFloor / ((double)N * (double)N * 0.375) + 1e-30));
        // waterfall row and spectrum
        std::copy(wf.begin() + kDcsWfCols, wf.end(), wf.begin());
        for (int c = 0; c < kDcsWfCols; c++) {
            const int a = (int)((int64_t)c * N / kDcsWfCols), b = std::max(a + 1, (int)((int64_t)(c + 1) * N / kDcsWfCols));
            double mx = 0;
            for (int j = a; j < b; j++) mx = std::max(mx, p[(size_t)j] / floorv[(size_t)j]);
            const float db = (float)(10 * std::log10(std::max(mx, 1e-3)));
            spec[(size_t)c] = std::max(0.f, db);
            wf[(size_t)(kDcsWfRows - 1) * kDcsWfCols + (size_t)c] = (uint8_t)std::max(0.f, std::min(255.f, db / 40.f * 255.f));
        }
        // carrier lines
        const double thr = std::pow(10.0, thrReq.load() / 10.0);
        std::vector<Cand> found;
        const int guard = 1;   // the radio's own DC spike
        for (int i = 4; i < N - 4; i++) {
            if (std::abs(i - N / 2) <= guard) continue;
            const double v = p[(size_t)i];
            if (v < thr * floorv[(size_t)i]) continue;
            bool top = true;
            for (int j = i - 4; j <= i + 4; j++) if (p[(size_t)j] > v) { top = false; break; }
            if (!top) continue;
            if (p[(size_t)i - 1] == v) continue;
            const double a = std::log(p[(size_t)i - 1] + 1e-30), b = std::log(v), c = std::log(p[(size_t)i + 1] + 1e-30);
            const double den = a - 2 * b + c;
            const double d = std::fabs(den) > 1e-9 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0.0;
            Cand cd;
            cd.f = ((double)(i - N / 2) + d) * binHz;
            cd.lastSnr = 10 * std::log10((p[(size_t)i - 1] + v + p[(size_t)i + 1]) / (3.0 * floorv[(size_t)i]));
            cd.seen = 1;
            found.push_back(cd);
        }
        // the sidebands of the data (a Manchester spectrum peaks about 900 Hz from the carrier) make weaker lines near a strong carrier: not carriers
        {
            std::vector<Cand> keep;
            for (const auto& f : found) {
                bool side = false;
                for (const auto& g : found) if (&g != &f && std::fabs(g.f - f.f) < 1100 && g.lastSnr > f.lastSnr + 4.0) { side = true; break; }
                if (!side) keep.push_back(f);
            }
            found.swap(keep);
        }
        std::vector<Cand> next;
        for (const auto& f : found) {
            Cand c = f;
            for (const auto& o : cands) if (std::fabs(o.f - f.f) < 200) { c.seen = o.seen + 1; break; }
            next.push_back(c);
            for (auto& s : slots) if (std::fabs(s.freq - f.f) < 400) { s.lastSeenSec = timeSec; s.snr = (float)f.lastSnr; }
        }
        cands = next;
        black.erase(std::remove_if(black.begin(), black.end(), [&](const Black& b) { return b.until < timeSec; }), black.end());
        for (const auto& c : cands) {
            if (c.seen < 3) continue;
            bool have = (int)slots.size() >= kMaxSlots;
            for (const auto& s : slots) if (std::fabs(s.freq - c.f) < 400) have = true;
            for (const auto& b : black) if (std::fabs(b.f - c.f) < 400) have = true;
            for (const auto& s : slots) if (std::fabs(s.freq - c.f) < 3600 && s.snr > c.lastSnr + 4.0) have = true;   // a sideband of a carrier already followed
            if (!have) spawn(c.f, (float)c.lastSnr);
        }
    }

    void spawn(double f, float snr) {
        Slot s;
        s.id = nextSlot++;
        s.freq = f;
        s.snr = snr;
        const double ds = std::max(1.0, std::round(fw / kSlotRate));
        s.cic.configure((int)ds);
        s.rs = fw / ds;
        s.rot = std::polar(1.f, (float)(-2 * math::kPi * f / fw));
        s.acqLeft = (int)(0.2 * s.rs);
        s.cs.assign(8192, 0.0);
        s.lastSeenSec = timeSec; s.bornSec = timeSec;
        for (int h = 0; h < kNumHyp; h++) {
            const double P = s.rs / kHyps[h].bps;
            for (int k = 0; k < kPhases; k++) { s.tr[h][k].tPrev = P * k / kPhases; s.tr[h][k].tNext = s.tr[h][k].tPrev + P; }
        }
        carriersSeen++;
        slots.push_back(std::move(s));
        dirty = true;
    }

    // ----- slots -----
    void finish(Slot& s, bool ended) {
        DcsMessage m;
        m.id = nextId;
        m.wallTime = baseUtc + (int64_t)std::floor(s.syncTimeSec);
        m.address = s.addrWord; m.addressOk = true; m.addressFixed = s.addrFixed;
        m.bps = s.bps; m.manchester = s.man;
        const double rel = s.freq + (s.integ * s.rs / (2 * math::kPi));
        const double cen = centreReq.load();
        m.freqAbsolute = cen > 0;
        m.freqHz = cen > 0 ? cen + rel : rel;
        m.channel = cen > 0 ? dcs::channelOf(cen + rel, plan()) : 0;
        m.plan = (int)plan();
        m.snrDb = s.snr;
        std::vector<uint8_t> rest(s.bits.begin() + std::min<size_t>(31, s.bits.size()), s.bits.end());
        const bool ok = dcs::parseBody(rest, s.bps >= 300, m);
        const int chars = (int)m.raw.size();
        const bool good = ok && (m.eot || chars >= 4) && m.parityErrors * 4 <= chars + 1;
        s.state = 0; s.lockHyp = -1; s.bits.clear(); s.addrDone = false; s.bornSec = timeSec;
        if (!good) {
            badN++; dirty = true;
            if (log) { char b[200]; snprintf(b, sizeof b, "GOES DCS: report dropped at %+.0f Hz, %d bps %s, %s, %d characters, %d parity errors%s", rel, s.bps, s.man ? "Manchester" : "NRZ",
                                       dcs::addressHex(m.address).c_str(), chars, m.parityErrors, m.eot ? "" : ", no end code"); log(b); }
            return;
        }
        nextId++;
        m.eot = m.eot || ended;
        okN++;
        addMessage(m);
    }

    void addMessage(const DcsMessage& m) {
        msgs.insert(msgs.begin(), m);
        if (msgs.size() > kDcsMsgMax) msgs.pop_back();
        DcsPlatform* pl = nullptr;
        for (auto& p : plats) if (p.address == m.address) { pl = &p; break; }
        if (!pl) { plats.push_back(DcsPlatform()); pl = &plats.back(); pl->address = m.address; pl->firstTime = m.wallTime; }
        pl->count++; pl->lastTime = m.wallTime; pl->channel = m.channel; pl->bps = m.bps; pl->snrDb = m.snrDb; pl->format = m.format;
        std::string t = m.text;
        for (auto& c : t) if (c == '\n') c = ' ';
        pl->last = t.substr(0, 80);
        std::stable_sort(plats.begin(), plats.end(), [](const DcsPlatform& a, const DcsPlatform& b) { return a.lastTime > b.lastTime; });
        if (plats.size() > kDcsPlatMax) plats.pop_back();
        if (!logPath.empty()) {
            FILE* f = fopen(logPath.c_str(), "ab");
            if (f) {
                fseek(f, 0, SEEK_END);
                if (ftell(f) == 0) fputs("id,unix_time,address,channel,freq_hz,bps,coding,snr_db,format,eot,parity_errors,text\n", f);
                std::string q;
                for (char c : m.text) { if (c == '"') q += "\"\""; else q += (c == '\n' ? ' ' : c); }
                fprintf(f, "%u,%lld,%s,%d,%.0f,%d,%s,%.1f,%s,%d,%d,\"%s\"\n", m.id, (long long)m.wallTime, dcs::addressHex(m.address).c_str(), m.channel, m.freqHz, m.bps,
                        m.manchester ? "manchester" : "nrz", (double)m.snrDb, m.format == 1 ? "pseudo-binary" : "ascii", m.eot ? 1 : 0, m.parityErrors, q.c_str());
                fclose(f);
            }
        }
        if (log) {
            char b[200];
            snprintf(b, sizeof b, "GOES DCS: %s ch %d %d bps %.1f dB: %.60s", dcs::addressHex(m.address).c_str(), m.channel, m.bps, (double)m.snrDb, m.text.c_str());
            log(b);
        }
        dirty = true;
    }

    void bitDecided(Slot& s, int h, int k, bool bit) {
        Track& t = s.tr[h][k];
        t.hist = (t.hist << 1) | (bit ? 1u : 0u);
        t.nb++;
        if (t.cand) {
            if (t.bits.size() < 31 + 64) {   // a candidate that has its address keeps the bits that follow until the others have theirs
                t.bits.push_back((uint8_t)(bit ^ (t.pol != 0)));
                if (t.bits.size() == 31) candDone(s, h, k);
            }
            return;
        }
        if (t.nb < 63) return;
        // candidates are taken only while the first one is young: a later match is a false one
        for (int hh = 0; hh < kNumHyp; hh++)
            for (int kk = 0; kk < kPhases; kk++)
                if (s.tr[hh][kk].cand && s.tr[hh][kk].bits.size() >= 6) return;
        // sync: the last 15 bits; preamble: the 16 bits before
        const uint32_t sw = (uint32_t)(t.hist & 0x7FFF);
        uint32_t want = 0;
        for (int i = 0; i < dcs::kSyncBits; i++) want = (want << 1) | dcs::kSync[i];
        int d = __builtin_popcount(sw ^ want);
        const int dInv = dcs::kSyncBits - d;
        const int pol = dInv < d ? 1 : 0;
        if (pol) d = dInv;
        if (d > 1) return;
        // the 48 preamble bits before it must alternate: a 16-bit check let the 300 / 1200 bps hypotheses lock falsely on a 100 bps platform
        // whose neighbour 1.5 or 3 kHz away beats in the slot (and a wrong address passes the 2-error BCH correction about half the time)
        const uint64_t pre = (t.hist >> 15) & 0xFFFFFFFFFFFFull;
        const uint64_t alt = (pre ^ (pre >> 1)) & 0x7FFFFFFFFFFFull;   // 1 where neighbours differ (47 pairs)
        const int trans = __builtin_popcountll(alt);
        if (d > 1 || d + (47 - trans) > 2) return;
        if (s.pending == 0) s.syncTimeSec = timeSec;
        t.cand = true; t.pol = pol; t.ne = -1; t.bits.clear();
        s.pending++;
    }

    // a candidate has its 31 address bits: when every candidate has, the one whose address passes the BCH check with the fewest corrections takes the carrier
    void candDone(Slot& s, int h, int k) {
        Track& t = s.tr[h][k];
        uint32_t w = 0;
        for (int i = 0; i < 31; i++) w = (w << 1) | t.bits[(size_t)i];
        uint32_t cw; int ne;
        t.ne = dcs::addressDecode(w, cw, ne) ? ne : -1;
        if (--s.pending > 0) return;
        int bh = -1, bk = -1, best = 99;
        for (int hh = 0; hh < kNumHyp; hh++)
            for (int kk = 0; kk < kPhases; kk++) {
                Track& c = s.tr[hh][kk];
                if (c.cand && c.ne >= 0 && c.ne < best) { best = c.ne; bh = hh; bk = kk; }
            }
        if (bh < 0) { clearCands(s); return; }
        Track& wt = s.tr[bh][bk];
        uint32_t ww = 0;
        for (int i = 0; i < 31; i++) ww = (ww << 1) | wt.bits[(size_t)i];
        uint32_t cc; int nn;
        dcs::addressDecode(ww, cc, nn);
        s.state = 2; s.lockHyp = bh; s.lockTr = bk; s.pol = wt.pol;
        s.bps = kHyps[bh].bps; s.man = kHyps[bh].man;
        s.bits = wt.bits;
        s.addrWord = cc; s.addrFixed = nn; s.addrDone = true;
        s.bornSec = timeSec;
        for (auto& row : s.tr) for (auto& c : row) { c.cand = false; c.bits.clear(); }
    }

    void clearCands(Slot& s) {
        s.pending = 0;
        for (auto& row : s.tr) for (auto& c : row) { c.cand = false; c.bits.clear(); }
    }

    void lockedBit(Slot& s, bool bit) {
        s.bits.push_back((uint8_t)(bit ^ (s.pol != 0)));
        if (s.bits.size() >= 31 + 16 && (s.bits.size() - 31) >= 8) {
            // last 8 bits are the EOT code?
            uint8_t c = 0;
            for (int i = 0; i < 8; i++) c |= (uint8_t)(s.bits[s.bits.size() - 8 + (size_t)i] << i);
            if (c == dcs::kEot) {
                const size_t rest = s.bits.size() - 31;
                if (rest % 8 == 0 || (rest >= 9 && (rest - 9) % 8 == 0)) {
                    finish(s, true);
                    resetTracks(s);
                    return;
                }
            }
        }
        if (s.bits.size() > (size_t)(31 + 9 + 8 * (kDcsBytesMax + 2))) { finish(s, false); resetTracks(s); }
    }

    void resetTracks(Slot& s) {
        s.pending = 0;
        for (int h = 0; h < kNumHyp; h++) {
            const double P = s.rs / kHyps[h].bps;
            for (int k = 0; k < kPhases; k++) {
                Track& t = s.tr[h][k];
                t.hist = 0; t.nb = 0; t.cand = false; t.bits.clear();
                t.tPrev = (double)s.ns - P * 1.5 + P * k / kPhases;
                while (t.tPrev < (double)s.ns - P * 2) t.tPrev += P;
                t.tNext = t.tPrev + P;
            }
        }
    }

    // one baseband sample of the slot
    void baseband(Slot& s, cf32 z) {
        // carrier loop
        if (s.acqLeft > 0) {
            {
                const int k = s.boxN & 15;
                s.boxSum += z - s.box[k]; s.box[k] = z;
                const cf32 w = s.boxSum;
                if (s.boxN >= 32) s.acqSum += w * std::conj(s.wHist[k]);   // lag 16
                s.wHist[k] = w;
                s.boxN++;
            }
            s.prev = z; s.havePrev = true;
            if (--s.acqLeft == 0) {
                s.integ = std::arg(s.acqSum) / 16.0;
                s.theta = std::arg(z);
                s.mavg = cf32(std::abs(z), 0);
            }
            s.ns++;
            s.cs[(size_t)(s.ns) & (s.cs.size() - 1)] = s.cs[(size_t)(s.ns - 1) & (s.cs.size() - 1)];
            return;
        }
        const cf32 u = z * std::polar(1.f, (float)-s.theta);
        // decision-directed phase detector: the carrier sits half way between the two +-60 degree states, so turning u back by the state it is nearest to
        // leaves the carrier phase whatever the data (no bias from runs of equal bits), and for the unmodulated carrier the two turns average out
        static const cf32 kBack[2] = {std::polar(1.f, (float)(-math::kPi / 3)), std::polar(1.f, (float)(math::kPi / 3))};
        const cf32 rr = u * kBack[u.imag() >= 0 ? 0 : 1];
        const float a = 0.02f;
        s.mavg += (rr - s.mavg) * a;
        const double err = std::atan2((double)s.mavg.imag(), (double)s.mavg.real());
        const double wn = 2 * math::kPi * 5.0 / s.rs, kp = 2 * 0.9 * wn, ki = wn * wn;
        s.integ += ki * err;
        s.theta += s.integ + kp * err;
        if (s.theta > math::kPi) s.theta -= 2 * math::kPi; else if (s.theta < -math::kPi) s.theta += 2 * math::kPi;
        const float mag = std::abs(s.mavg);
        s.ref = std::max(mag, s.ref * 0.99999f);
        const double dd = (double)u.imag() / (double)(mag + 1e-6f);
        const double dcl = std::max(-4.0, std::min(4.0, dd));
        const size_t m = s.cs.size() - 1;
        s.cs[(size_t)(s.ns + 1) & m] = s.cs[(size_t)s.ns & m] + dcl;
        s.ns++;
        // lost carrier
        if (mag < 0.3f * s.ref) s.lostRun++; else s.lostRun = 0;
        // bit decisions
        const double now = (double)s.ns;
        for (int h = 0; h < kNumHyp; h++) {
            if (s.state == 2 && h != s.lockHyp) continue;
            const double P = s.rs / kHyps[h].bps;
            const double o = std::max(1.0, P / 16);
            for (int k = 0; k < kPhases; k++) {
                if (s.state == 2 && k != s.lockTr) continue;
                Track& t = s.tr[h][k];
                while (now >= t.tNext + o + 1) {
                    const bool man = kHyps[h].man;
                    auto metric = [&](double a0, double b0) {
                        if (man) { const double mid = 0.5 * (a0 + b0); return (s.S(mid) - s.S(a0)) - (s.S(b0) - s.S(mid)); }
                        return s.S(b0) - s.S(a0);
                    };
                    const double mm = metric(t.tPrev, t.tNext);
                    const bool bit = mm > 0;
                    if (s.state == 2 || t.cand) {   // a track that carries a report or an address follows the bit clock
                        const double me = std::fabs(metric(t.tPrev - o, t.tNext - o)), ml = std::fabs(metric(t.tPrev + o, t.tNext + o));
                        const double e = (ml - me) / (ml + me + 1e-9);
                        t.tPrev = t.tNext;
                        t.tNext = t.tPrev + P + 0.03 * e * o;
                    } else { t.tPrev = t.tNext; t.tNext += P; }
                    if (s.state == 2) {
                        lockedBit(s, bit);
                        if (s.state != 2) break;
                    } else {
                        bitDecided(s, h, k, bit);
                        if (s.state == 2) break;
                    }
                }
            }
        }
        if (s.state == 2 && s.lostRun > (int)(0.4 * s.rs)) {
            finish(s, false);
            resetTracks(s);
        }
    }

    void processSlots(const cf32* x, size_t n) {
        for (auto& s : slots) {
            for (size_t i = 0; i < n; i++) {
                cf32 y = x[i] * s.ph;
                s.ph *= s.rot;
                if (++s.renorm >= 2048) { s.renorm = 0; s.ph /= std::abs(s.ph); }
                cf32 z;
                if (s.cic.push(y, z)) baseband(s, z);
            }
        }
        // free slots
        for (size_t i = 0; i < slots.size();) {
            Slot& s = slots[i];
            bool drop = false;
            if (s.state != 2) {
                const bool gone = timeSec - s.lastSeenSec > 2.0;
                const bool stale = timeSec - s.bornSec > 25.0;
                if (gone) drop = true;
                else if (stale) { black.push_back({s.freq, timeSec + 40.0}); drop = true; }
            }
            if (drop) { slots.erase(slots.begin() + (long)i); dirty = true; } else i++;
        }
    }

    void publish() {
        pub.seq++;
        pub.state = okN ? 2 : (!slots.empty() ? 1 : 0);
        pub.cfoHz = bestCfo;
        pub.snrDb = bestSnr;
        pub.dataValid = okN > 0;
        pub.blocksOk = okN; pub.blocksBad = badN;
        pub.timeSec = timeSec; pub.inputRate = rateSet; pub.workRate = fw; pub.binHz = binHz; pub.centreHz = centreReq.load();
        pub.noiseDb = noiseDb;
        pub.plan = (int)plan();
        pub.carriersSeen = carriersSeen;
        pub.messages = msgs;
        pub.platforms = plats;
        pub.carriers.clear();
        float best = -1;
        for (const auto& s : slots) {
            DcsCarrier c;
            c.offsetHz = s.freq + s.integ * s.rs / (2 * math::kPi);
            const double cen = centreReq.load();
            c.channel = cen > 0 ? dcs::channelOf(cen + c.offsetHz, plan()) : 0;
            c.snrDb = s.snr;
            c.state = s.state == 2 ? 2 : (s.ns > 0.5 * s.rs ? 1 : 0);
            c.bps = s.state == 2 ? s.bps : 0;
            c.bytes = s.state == 2 && s.bits.size() > 40 ? (uint32_t)((s.bits.size() - 40) / 8) : 0;
            pub.carriers.push_back(c);
            if (s.snr > best) { best = s.snr; bestSnr = s.snr; bestCfo = c.offsetHz; }
        }
        pub.waterfall = wf;
        pub.spectrum = spec;
        pub.bandLowHz = -fw / 2; pub.bandHighHz = fw / 2;
        pub.logging = !logPath.empty(); pub.logPath = logPath;
        dirty = false;
    }
};

GoesDcsReceiver::GoesDcsReceiver() : p_(std::make_unique<Impl>()) {}
GoesDcsReceiver::~GoesDcsReceiver() = default;

void GoesDcsReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rateSet = inputRateHz;
    p_->resetReq = true;
}
void GoesDcsReceiver::setSignalOffset(double) {}
void GoesDcsReceiver::setCenterHz(double hz) { p_->centreReq = hz; }
void GoesDcsReceiver::setPlan(int plan) { p_->planReq = plan < 0 || plan > 2 ? -1 : plan; }
bool GoesDcsReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rateSet >= goesdcsTuning().minSampleRate - 1;
}
void GoesDcsReceiver::reset() { p_->resetReq = true; }
void GoesDcsReceiver::clearResults() { p_->clearReq = true; }
void GoesDcsReceiver::setThresholdDb(double db) { p_->thrReq = std::max(6.0, std::min(25.0, db)); }
void GoesDcsReceiver::setLogFile(const std::string& path) { std::lock_guard<std::mutex> lk(p_->mu); p_->logPathReq = path; p_->logReq = true; }
void GoesDcsReceiver::setStartUtc(int64_t utcSec) { p_->startReq = utcSec; p_->resetReq = true; }
void GoesDcsReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

bool GoesDcsReceiver::telemetry(DcsTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void GoesDcsReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.logReq.exchange(false)) { std::lock_guard<std::mutex> lk(m.mu); m.logPath = m.logPathReq; m.dirty = true; }
    if (m.resetReq.exchange(false)) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.resetState();
        m.publish();
    }
    if (m.clearReq.exchange(false)) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.msgs.clear(); m.plats.clear(); m.okN = 0; m.badN = 0; m.nextId = 1;
        m.dirty = true;
    }
    if (n == 0 || m.rateSet < goesdcsTuning().minSampleRate - 1 || !m.fft) return;
    const size_t chunk = 65536;
    for (size_t off = 0; off < n; off += chunk) {
        const size_t k = std::min(chunk, n - off);
        m.fbuf.clear();
        for (size_t i = 0; i < k; i++) {
            cf32 v = x[off + i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            cf32 o;
            if (m.front.push(v, o)) m.fbuf.push_back(o);
        }
        m.nIn += k;
        std::lock_guard<std::mutex> lk(m.mu);
        // scanner
        for (size_t i = 0; i < m.fbuf.size(); i++) {
            m.fr[(size_t)m.fill] = m.fbuf[i] * m.win[(size_t)m.fill];
            if (++m.fill == m.N) {
                m.fill = 0;
                std::vector<cf32> t = m.fr;
                m.fft->forward(t.data());
                for (int j = 0; j < m.N; j++) m.acc[(size_t)j] += (double)std::norm(t[(size_t)j]);
                if (++m.frames >= m.M) {
                    m.frames = 0;
                    m.timeSec = ((double)(m.nIn - k) + (double)(i + 1) * m.D) / m.rateSet;
                    m.spectrumDone();
                    m.dirty = true;
                }
            }
        }
        m.timeSec = (double)m.nIn / m.rateSet;
        m.processSlots(m.fbuf.data(), m.fbuf.size());
        if (m.dirty || m.nIn >= m.nextPub) {
            m.nextPub = m.nIn + (uint64_t)(0.25 * m.rateSet);
            m.publish();
        }
    }
}

ModeTuning goesdcsTuning() {
    ModeTuning t;
    t.stdMode = 143; t.id = "goesdcs"; t.name = "GOES DCS";
    t.minMhz = 401.0; t.maxMhz = 403.0; t.defMhz = 401.85;
    t.sampleRate = 1e6;
    t.basebandHz = 0;
    t.bandwidthMhz = 0.5;
    t.minSampleRate = 48e3;
    t.tuneOffsetHz = 0;
    t.carrierAtCentre = true;     // a platform carrier can lie at the centre: the DC removal would take it with the spike
    return t;
}

} // namespace dect2

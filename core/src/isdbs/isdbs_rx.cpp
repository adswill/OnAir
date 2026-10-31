// ISDB-S receiver, see isdbs_rx.h.
#include "dect2/isdbs/isdbs_rx.h"
#include "../dvbs/dvbs_front.h"
#include "dect2/dsp/dsp_compat.h"
#include "isdbs_fec.h"
#include "dect2/fec/fec_proto.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace dect2 {

using namespace isdbs;

namespace {
constexpr double kTwoPi = 2.0 * dect2::math::kPi;
constexpr double kMinRate = 39.0e6;
constexpr size_t kCoarseN = 32768;
constexpr size_t kSearchWin = 2 * (size_t)kFrameSyms + 256;
constexpr int kBack = 16;                 // symbols kept before the frame start for the local re-alignment
constexpr int kExtra = 12;                // symbols of the next frame kept with a frame: W1's first 6 bits end the code's path
constexpr double kSyncThr = 0.30;
constexpr int kMargin = 240;              // trellis steps decoded on each side of the wanted slots

struct SyncHit { int pos = -1; double score = 0; };

// metric of the header's fixed parts for a frame starting at y[0]: the 20 known symbols of W1 (header 12..31) and the 20 of W2 / W3
// (172..191, either sign): |correlation|^2 / (20 * energy), 1 for a clean frame
inline double syncMetric(const cf32* y, cf32* c1 = nullptr, cf32* c2 = nullptr) {
    const auto& a = w1Tail();
    const auto& b = w2Tail();
    float r1 = 0, i1 = 0, r2 = 0, i2 = 0, e = 0;
    for (int i = 0; i < 20; i++) {
        const cf32 u = y[kW1TailPos + i], v = y[kW2TailPos + i];
        r1 += u.real() * a[(size_t)i]; i1 += u.imag() * a[(size_t)i];
        r2 += v.real() * b[(size_t)i]; i2 += v.imag() * b[(size_t)i];
        e += std::norm(u) + std::norm(v);
    }
    if (c1) *c1 = cf32(r1, i1);
    if (c2) *c2 = cf32(r2, i2);
    return e > 1e-9f ? (double)(r1 * r1 + i1 * i1 + r2 * r2 + i2 * i2) / (20.0 * e) : 0.0;
}

// first frame start in y (len symbols, at least 2 frames): the fixed header symbols at p and again one frame later
SyncHit findSync(const cf32* y, size_t len) {
    SyncHit h;
    if (len < (size_t)kFrameSyms + kHeaderSyms + 1) return h;
    const size_t n = len - kFrameSyms - kHeaderSyms;
    for (size_t p = 0; p < n; p++) {
        const double m1 = syncMetric(y + p);
        if (m1 < kSyncThr) continue;
        const double m2 = syncMetric(y + p + kFrameSyms);
        const double s = std::min(m1, m2);
        if (s > h.score && s >= kSyncThr) { h.score = s; h.pos = (int)p; }
    }
    return h;
}

// carrier offset (cycles per symbol) from the 8th power of the symbols: BPSK, QPSK and 8PSK points all go to the same point
double coarseCfo(const cf32* z, size_t n, double& ratio) {
    ratio = 0;
    int lg = 0;
    while (((size_t)2 << lg) <= n && lg < 15) lg++;
    const int N = 1 << lg;
    std::vector<float> re((size_t)N), im((size_t)N);
    for (int i = 0; i < N; i++) {
        const float a = std::abs(z[i]);
        cf32 u = a > 1e-9f ? z[i] / a : cf32(1, 0);
        u *= u; u *= u; u *= u;
        re[(size_t)i] = u.real(); im[(size_t)i] = u.imag();
    }
    fftSplit(re.data(), im.data(), lg, false);
    std::vector<float> mag((size_t)N);
    double sum = 0;
    for (int i = 0; i < N; i++) { mag[(size_t)i] = std::sqrt(re[(size_t)i] * re[(size_t)i] + im[(size_t)i] * im[(size_t)i]); sum += (double)mag[(size_t)i] * mag[(size_t)i]; }
    int kp = 0;
    for (int i = 1; i < N; i++) if (mag[(size_t)i] > mag[(size_t)kp]) kp = i;
    const double mean = (sum - (double)mag[(size_t)kp] * mag[(size_t)kp]) / (N - 1);
    ratio = (double)mag[(size_t)kp] * mag[(size_t)kp] / std::max(1e-30, mean);
    const float a = mag[(size_t)((kp + N - 1) % N)], b = mag[(size_t)kp], c = mag[(size_t)((kp + 1) % N)];
    const double den = (double)a - 2.0 * b + c;
    const double d = den != 0 ? 0.5 * ((double)a - c) / den : 0.0;
    double f8 = ((double)kp + d) / N;
    if (f8 > 0.5) f8 -= 1.0;
    return f8 / 8.0;
}

// the burst of block j, squared and summed: the BPSK data on it (TR-B15 6.5: a receiver must not count on it being fixed) drops out
inline std::complex<double> burstSq(const cf32* F, int j) {
    std::complex<double> a(0, 0);
    for (int i = 0; i < kBurstSyms; i++) { const std::complex<double> y(F[kHeaderSyms + j * kBlockSyms + kBlockData + i]); a += y * y; }
    return a;
}

// The header of a frame (carrier phase removed): W1, the 64 TMCC bits, W2 or W3. Returns the better path metric; `first`: W2 was sent.
float decodeHeader(const cf32* F, uint8_t* tmcc64, bool& first) {
    Branch br[kHeaderBits];
    headerBranches(F, br);
    int8_t forced[kHeaderBits];
    uint8_t bits[kHeaderBits] = {}, best[kHeaderBits] = {};
    float bestM = -1e30f;
    first = false;
    const int n = kHeaderBits - 6;
    for (int w = 0; w < 2; w++) {
        const uint16_t end = w == 0 ? kW2 : kW3;
        for (int i = 0; i < kHeaderBits; i++) forced[i] = -1;
        for (int i = 6; i < 16; i++) forced[i] = (int8_t)((kW1 >> (15 - i)) & 1);
        for (int i = 80; i < 96; i++) forced[i] = (int8_t)((end >> (15 - (i - 80))) & 1);
        unsigned es = 0;
        for (int i = 0; i < 16; i++) es = convNext(es, (end >> (15 - i)) & 1);
        const float m = viterbi(br + 6, n, (int)w1State(), (int)es, forced + 6, bits + 6);
        if (m > bestM) { bestM = m; first = w == 0; memcpy(best, bits, sizeof bits); }
    }
    for (int i = 0; i < 64; i++) tmcc64[i] = best[16 + i];
    return bestM;
}

// how well the header decodes: path metric over the header's energy (about 1 for a clean frame)
double headerQuality(const cf32* F) {
    uint8_t t[64];
    bool first;
    const float m = decodeHeader(F, t, first);
    double e = 0;
    for (int i = 12; i < kHeaderSyms; i++) e += std::abs(F[i].real()) + std::abs(F[i].imag());
    return e > 1e-9 ? m / (2.0 * e) : 0.0;
}
}  // namespace

struct IsdbsReceiver::Impl {
    double rate = 0, offsetHz = 0, centerHz = 0;
    bool configured = false;
    dvbs::DvbsFront front;
    std::vector<cf32> sy;                       // symbols from the front end
    size_t head = 0;                            // start of the frame being waited for (locked) or of the unsearched symbols
    bool locked = false;
    double cfo = 0;                             // cycles per symbol
    int misses = 0;
    float snrLin = 0;
    // super-frame assembly
    bool collecting = false;
    int nextFrame = 0;
    std::vector<cf32> sfData[kFramesPerSuper];  // each frame (carrier removed) and kExtra symbols of the next
    uint8_t tmccBits[kTmccCoded];
    bool firstOf[kFramesPerSuper] = {};
    Tmcc hist[3];                               // TMCC decoded in the last three super-frames, newest first (valid = false when not)
    Tmcc cfg;                                   // the layout in use
    Layout layout;
    std::vector<IsdbsStreamRow> rows;
    int active = -1;
    int selRequest = -1;
    uint64_t rsClean = 0, rsCorrected = 0, rsFailed = 0, packets = 0;
    std::vector<uint8_t> tsOut;
    std::chrono::steady_clock::time_point lastPacket{};
    double tsRateSecs = 0;
    uint64_t tsRatePackets = 0;
    double tsBitrate = 0;
    std::vector<cf32> consts;
    // telemetry
    std::mutex mu;
    IsdbsTelemetry tel;
    uint64_t seq = 0;
    std::function<void(const std::string&)> log;
    std::function<void(const uint8_t*, size_t, double)> onPackets;

    void say(const std::string& s) { if (log) log(s); }

    void resetAll() {
        front.reset();
        front.timer().setLoop(0.01, 0.707);    // wide while searching
        sy.clear(); head = 0; skipSyms = 0; failures = 0; locked = false; cfo = 0; misses = 0; snrLin = 0;
        collecting = false; nextFrame = 0;
        for (auto& h : hist) h = Tmcc();
        cfg = Tmcc(); layout = Layout(); rows.clear(); active = -1;
        rsClean = rsCorrected = rsFailed = packets = 0;
        tsOut.clear();
        tsRateSecs = 0; tsRatePackets = 0; tsBitrate = 0; consts.clear();
        std::lock_guard<std::mutex> l(mu);
        const double ir = tel.inputRate, cf = tel.centerHz;
        tel = IsdbsTelemetry();
        tel.inputRate = ir; tel.centerHz = cf;
        tel.state = configured ? 1 : 0;
        tel.seq = ++seq;
    }

    void publish(int state) {
        std::lock_guard<std::mutex> l(mu);
        tel.state = state;
        tel.cfoHz = cfo * kSymbolRate + offsetHz;
        tel.snrDb = snrLin > 0 ? (float)(10.0 * std::log10(snrLin)) : 0.f;
        tel.locked = locked;
        tel.tmcc = cfg;
        tel.streams = rows;
        tel.selected = active;
        tel.tsBitrate = tsBitrate;
        tel.constellation = consts;
        tel.packets = packets; tel.rsClean = rsClean; tel.rsCorrected = rsCorrected; tel.rsFailed = rsFailed;
        tel.blocksOk = rsClean + rsCorrected; tel.blocksBad = rsFailed;
        tel.dataValid = std::chrono::steady_clock::now() - lastPacket < std::chrono::seconds(2) && lastPacket.time_since_epoch().count() != 0;
        tel.seq = ++seq;
    }

    // ---- searching: the symbols from `head` on
    // residual carrier offset (cycles per symbol) of the frame in F from the squared bursts: they turn by 2 * 2 pi f 207 from one block
    // to the next, which settles f only up to a multiple of 1 / 414; `coherence` is 1 for a clean signal
    static double refineCfo(const cf32* F, double* coherence = nullptr) {
        std::vector<std::complex<double>> c(kBlocks);
        double total = 0;
        for (int j = 0; j < kBlocks; j++) { c[(size_t)j] = burstSq(F, j); total += std::norm(c[(size_t)j]); }
        double bestV = -1, bestF = 0;
        for (int q = -121; q <= 121; q++) {
            const double fr = q * 1e-5;
            const double w = -kTwoPi * 2.0 * fr * kBlockSyms;
            const std::complex<double> st(std::cos(w), std::sin(w));
            std::complex<double> rot(1, 0), acc(0, 0);
            for (int j = 0; j < kBlocks; j++) { acc += c[(size_t)j] * rot; rot *= st; }
            const double v = std::norm(acc);
            if (v > bestV) { bestV = v; bestF = fr; }
        }
        if (coherence) *coherence = total > 1e-9 ? bestV / ((double)kBlocks * total) : 0.0;
        return bestF;
    }

    // the header of the frame at y[0] after removing frequency f (cycles per symbol) and the phase of W1
    static double headerAt(const cf32* y, double f) {
        cf32 h[kHeaderSyms];
        for (int i = 0; i < kHeaderSyms; i++) { const double a = -kTwoPi * f * i; h[i] = y[i] * cf32((float)std::cos(a), (float)std::sin(a)); }
        cf32 c1;
        syncMetric(h, &c1);
        const float m = std::abs(c1);
        if (m < 1e-9f) return 0;
        const cf32 r = std::conj(c1) / m;
        for (auto& v : h) v *= r;
        return headerQuality(h);
    }

    int failures = 0;
    size_t skipSyms = 0;                        // symbols to let go by before the next search
    bool search() {
        if (sy.size() - head < kSearchWin) return false;
        const cf32* w = sy.data() + head;
        double ratio = 0;
        const double c8 = coarseCfo(w, kCoarseN, ratio);
        std::vector<double> cands;
        if (ratio > 10.0) cands.push_back(c8);
        cands.push_back(0.0);
        const bool grid = (failures % 4) == 3;   // the slow search over the whole range, now and then
        if (grid) for (int k = 1; k <= 14; k++) { cands.push_back(k * 0.0048); cands.push_back(-k * 0.0048); }
        SyncHit best; double bestF = 0, bestQ = 0;
        std::vector<cf32> d(kSearchWin), fr(kFrameSyms);
        for (double f : cands) {
            for (size_t i = 0; i < kSearchWin; i++) {
                const double a = -kTwoPi * f * (double)i;
                d[i] = w[i] * cf32((float)std::cos(a), (float)std::sin(a));
            }
            const SyncHit h = findSync(d.data(), kSearchWin);
            if (h.pos < 0) continue;
            for (int i = 0; i < kFrameSyms; i++) fr[(size_t)i] = d[(size_t)(h.pos + i)];
            double coh = 0;
            const double fres = refineCfo(fr.data(), &coh);
            if (coh < 0.2) continue;
            // the squared bursts leave f open by multiples of 1 / 414: the header, which decodes only with the right carrier, decides
            double q = -1e30, fb = 0;
            for (int k = -8; k <= 8; k++) {
                const double ff = fres + k / (2.0 * kBlockSyms);
                const double v = headerAt(&d[(size_t)h.pos], ff) + headerAt(&d[(size_t)h.pos + kFrameSyms], ff);
                if (v > q) { q = v; fb = ff; }
            }
            if (q > bestQ) { bestQ = q; best = h; bestF = f + fb; }
        }
        if (best.pos < 0 || bestQ < 0.5) {
            // nothing in this window: drop most of it, keep enough that a frame start cannot fall between two windows
            failures++;
            head += kSearchWin - (size_t)kFrameSyms - kHeaderSyms;
            if (grid) skipSyms = 24 * (size_t)kFrameSyms;   // that took longer than the signal lasted: let some of it go by
            return true;
        }
        failures = 0;
        locked = true; cfo = bestF; misses = 0;
        front.timer().setLoop(0.002, 0.707);   // the frames carry the timing now: a narrow loop slips less at low Es/N0 (a slip costs a super-frame)
        head += (size_t)best.pos;
        collecting = false; nextFrame = 0;
        say("ISDB-S frame sync found, carrier " + std::to_string((long)(cfo * kSymbolRate + offsetHz)) + " Hz from the centre");
        return true;
    }

    // ---- one frame at sy[head ...], already aligned
    bool frame() {
        const size_t need = (size_t)kFrameSyms + kExtra + 8;
        if (sy.size() - head < need) return false;
        std::vector<cf32> F;
        auto derot = [&](size_t from, size_t n, std::vector<cf32>& out) {
            out.resize(n);
            for (size_t i = 0; i < n; i++) {
                const double a = -kTwoPi * cfo * (double)i;
                out[i] = sy[from + i] * cf32((float)std::cos(a), (float)std::sin(a));
            }
        };
        // local alignment: the header may have moved a few symbols (a slip of the symbol clock)
        int shift = 0;
        {
            std::vector<cf32> t;
            double m0 = 0, bestM = 0;
            for (int d = -3; d <= 3; d++) {
                if ((long)head + d < 0) continue;
                derot(head + (size_t)d, kHeaderSyms, t);
                const double m = syncMetric(t.data());
                if (d == 0) m0 = m;
                if (m > bestM) { bestM = m; shift = d; }
            }
            if (bestM < m0 + 0.1) { shift = 0; bestM = m0; }
            if (bestM < 0.15) {
                head += (size_t)kFrameSyms;
                collecting = false;
                if (++misses >= 3) {
                    locked = false; cfg.valid = false;
                    front.timer().setLoop(0.01, 0.707);
                    { std::lock_guard<std::mutex> l(mu); tel.syncLosses++; }
                    say("ISDB-S frame sync lost");
                    publish(1);
                }
                return true;
            }
            misses = 0;
        }
        head = (size_t)((long)head + shift);
        derot(head, (size_t)kFrameSyms + kExtra, F);
        // ---- carrier phase: W1 (known), then W2 / W3 and the 192 bursts squared (their sign is not known), each one moved by whole half
        // turns next to the phase before it
        const int nP = kBlocks + 2;
        std::vector<double> pos((size_t)nP), ph((size_t)nP);
        std::vector<std::complex<double>> z((size_t)nP);
        cf32 c1, c2;
        syncMetric(F.data(), &c1, &c2);
        pos[0] = kW1TailPos + 9.5; ph[0] = std::arg(std::complex<double>(c1));
        pos[1] = kW2TailPos + 9.5; z[1] = std::complex<double>(c2) * std::complex<double>(c2) / 20.0;
        for (int j = 0; j < kBlocks; j++) { pos[(size_t)j + 2] = kHeaderSyms + j * kBlockSyms + kBlockData + 1.5; z[(size_t)j + 2] = burstSq(F.data(), j); }
        for (int k = 1; k < nP; k++) {
            std::complex<double> a(0, 0);
            for (int q = std::max(1, k - 3); q <= std::min(nP - 1, k + 3); q++) a += z[(size_t)q];
            double th = 0.5 * std::arg(a);
            while (th - ph[(size_t)k - 1] > dect2::math::kPi / 2) th -= dect2::math::kPi;
            while (th - ph[(size_t)k - 1] < -dect2::math::kPi / 2) th += dect2::math::kPi;
            ph[(size_t)k] = th;
        }
        // smoothing: a straight line through the neighbours of each point
        std::vector<double> sm((size_t)nP);
        for (int k = 0; k < nP; k++) {
            const int lo = std::max(0, k - 4), hi = std::min(nP - 1, k + 4);
            double sx = 0, sy2 = 0, sxx = 0, sxy = 0; int n = 0;
            for (int q = lo; q <= hi; q++) { const double x = pos[(size_t)q] - pos[(size_t)k]; sx += x; sy2 += ph[(size_t)q]; sxx += x * x; sxy += x * ph[(size_t)q]; n++; }
            const double den = n * sxx - sx * sx;
            sm[(size_t)k] = den > 1e-9 ? (sy2 - sx * (n * sxy - sx * sy2) / den) / n : ph[(size_t)k];
        }
        {
            int k = 0;
            double slp = (sm[1] - sm[0]) / (pos[1] - pos[0]);
            for (int i = 0; i < kFrameSyms + kExtra; i++) {
                while (k + 2 < nP && i > pos[(size_t)k + 1]) { k++; slp = (sm[(size_t)k + 1] - sm[(size_t)k]) / (pos[(size_t)k + 1] - pos[(size_t)k]); }
                const double a = sm[(size_t)k] + slp * (i - pos[(size_t)k]);
                F[(size_t)i] *= cf32((float)std::cos(-a), (float)std::sin(-a));
            }
        }
        const double slope = (sm[(size_t)nP - 1] - sm[0]) / (pos[(size_t)nP - 1] - pos[0]) / kTwoPi;   // cycles per symbol
        cfo += 0.7 * slope;
        // Es/N0 from the burst symbols (constant amplitude, whatever they carry): second and fourth moments
        {
            double m2 = 0, m4 = 0;
            for (int j = 0; j < kBlocks; j++) for (int i = 0; i < kBurstSyms; i++) {
                const double e = std::norm(F[(size_t)(kHeaderSyms + j * kBlockSyms + kBlockData + i)]);
                m2 += e; m4 += e * e;
            }
            m2 /= kBlocks * kBurstSyms; m4 /= kBlocks * kBurstSyms;
            const double sq = 2 * m2 * m2 - m4;
            if (sq > 0) {
                const double S = std::sqrt(sq), N = m2 - S;
                const float v = (float)(N > 1e-9 ? S / N : 1e4);
                snrLin = snrLin > 0 ? 0.8f * snrLin + 0.2f * v : v;
            }
        }
        // the header: TMCC bits and whether this is the first frame of a super-frame
        uint8_t t64[64];
        bool first = false;
        decodeHeader(F.data(), t64, first);
        { std::lock_guard<std::mutex> l(mu); tel.frames++; }
        if (first) { collecting = true; nextFrame = 0; }
        if (collecting) {
            const int fi = nextFrame;
            sfData[fi] = F;
            memcpy(tmccBits + fi * 64, t64, 64);
            firstOf[fi] = first;
            nextFrame = fi + 1;
            if (nextFrame == kFramesPerSuper) { finishSuper(); collecting = false; }
        }
        head += (size_t)kFrameSyms;
        if (!collecting && !cfg.valid) publish(2);
        return true;
    }

    void applyConfig(const Tmcc& t) {
        cfg = t;
        layout.build(t);
        rows.clear();
        int bestN = 0, bestRel = -1;
        for (int r = 0; r < kMaxStreams; r++) {
            const int n = t.slotsOf(r);
            if (!n) continue;
            IsdbsStreamRow row;
            row.rel = r; row.tsId = t.tsId[(size_t)r]; row.slots = n; row.mod = t.modOf(r);
            bool mixed = false;
            double br = 0;
            for (int s = 0; s < kSlots; s++) if (t.slotTs[(size_t)s] == r && t.slotMod(s) != kUnused) { if (t.slotMod(s) != row.mod) mixed = true; br += slotTsBitrate(t.slotMod(s)); }
            row.mixed = mixed; row.bitrate = br;
            rows.push_back(row);
            if (t.packetsOf(r) > bestN) { bestN = t.packetsOf(r); bestRel = r; }
        }
        active = selRequest >= 0 && t.packetsOf(selRequest) > 0 ? selRequest : bestRel;
        std::string d;
        for (const auto& r : rows) d += (d.empty() ? "" : "; ") + std::to_string(r.slots) + " slots " + modName(r.mod) + " (TS id " + std::to_string(r.tsId) + ")";
        say("ISDB-S TMCC: " + d);
        tsRateSecs = 0; tsRatePackets = 0;
    }

    bool wanted(int s) const { return cfg.slotTs[(size_t)s] == active && layout.ordinal[(size_t)s] >= 0; }

    // the bytes (interleaved order) of the packet-carrying slots of stream `active` in frame f
    void decodeFrame(int f, std::vector<uint8_t>* slotBytes) {
        const std::vector<cf32>& F = sfData[f];
        const Layout& L = layout;
        const int total = L.totalSteps + 6;   // and the next frame's first 6 header bits
        std::vector<Branch> br((size_t)total);
        headerBranches(F.data(), br.data());
        std::vector<std::pair<int, int>> want;
        for (int s = 0; s < kSlots; s++) {
            if (!wanted(s)) continue;
            const auto& G = L.groups[(size_t)L.group[(size_t)s]];
            const int per = G.mod == kTc8psk23 ? kSlotSyms : kSlotBytes * 8;
            const int a = G.step0 + L.ordinal[(size_t)s] * per;
            want.push_back({a, a + per});
        }
        if (want.empty()) return;
        for (size_t g = 0; g < L.groups.size(); g++) {
            const auto& G = L.groups[g];
            bool need = false;
            for (const auto& w : want) if (w.first < G.step0 + G.steps + kMargin && w.second > G.step0 - kMargin) need = true;
            if (need) groupBranches(L, (int)g, F.data(), &br[(size_t)G.step0]);
        }
        {   // the next frame's W1: its first 6 bits, BPSK at F[39936 ...]
            Branch nb[6];
            headerBranches(F.data() + kFrameSyms, nb, 6);   // only kExtra = 12 symbols of the next frame are kept
            for (int i = 0; i < 6; i++) br[(size_t)L.totalSteps + (size_t)i] = nb[i];
        }
        std::vector<int8_t> forced((size_t)total, -1);
        for (int i = 0; i < 6; i++) forced[(size_t)L.totalSteps + (size_t)i] = (int8_t)((kW1 >> (15 - i)) & 1);
        unsigned hs = 0;   // the state after the header: the last 6 bits of W2 / W3
        { const uint16_t end = firstOf[f] ? kW2 : kW3; for (int i = 0; i < 16; i++) hs = convNext(hs, (end >> (15 - i)) & 1); }
        std::sort(want.begin(), want.end());
        std::vector<std::pair<int, int>> win;
        for (const auto& w : want) {
            const int a = std::max(kHeaderBits, w.first - kMargin), b = std::min(total, w.second + kMargin);
            if (!win.empty() && a <= win.back().second) win.back().second = std::max(win.back().second, b);
            else win.push_back({a, b});
        }
        std::vector<uint8_t> bits((size_t)total), states((size_t)total + 1);
        for (const auto& w : win) {
            const int n = w.second - w.first;
            viterbi(&br[(size_t)w.first], n, w.first == kHeaderBits ? (int)hs : -1, w.second == total ? (int)w1State() : -1, &forced[(size_t)w.first], &bits[(size_t)w.first], &states[(size_t)w.first]);
        }
        std::vector<uint8_t> wb(kSlotSyms);
        for (int s = 0; s < kSlots; s++) {
            if (!wanted(s)) continue;
            const int g = L.group[(size_t)s];
            const auto& G = L.groups[(size_t)g];
            uint8_t* out = &slotBytes[f][(size_t)s * kSlotBytes];
            if (G.mod == kTc8psk23) {
                const int a = L.ordinal[(size_t)s] * kSlotSyms;   // group-relative step = symbol
                const int st = G.step0 + a;
                tc8Uncoded(L, g, F.data(), &states[(size_t)st], &bits[(size_t)st], a, a + kSlotSyms, wb.data());
                for (int k = 0; k < kSlotBytes; k++) {
                    unsigned v = 0;
                    for (int q = 0; q < 4; q++) v = (v << 2) | ((unsigned)wb[(size_t)(4 * k + q)] << 1) | bits[(size_t)(st + 4 * k + q)];
                    out[k] = (uint8_t)v;
                }
            } else {
                const int a = G.step0 + L.ordinal[(size_t)s] * kSlotBytes * 8;
                for (int k = 0; k < kSlotBytes; k++) { unsigned v = 0; for (int q = 0; q < 8; q++) v = (v << 1) | bits[(size_t)(a + 8 * k + q)]; out[k] = (uint8_t)v; }
            }
        }
    }

    void finishSuper() {
        // TMCC of this super-frame
        Tmcc t;
        bool have = false;
        {
            uint8_t c[kTmccCoded], info[kTmccBits];
            const auto& pr = tmccPrbs();
            for (int i = 0; i < kTmccCoded; i++) c[i] = tmccBits[i] ^ pr[(size_t)i];
            have = tmccDecode(c, info) >= 0 && tmccUnpack(info, t);
            std::lock_guard<std::mutex> l(mu);
            if (have) tel.tmccGood++; else tel.tmccBad++;
        }
        // it describes the super-frame after next (TR-B15 6.2.1), so the one for this super-frame came two super-frames ago. Just after
        // the lock that one is not there: the latest known stands in (wrong only while a change is under way)
        hist[2] = hist[1]; hist[1] = hist[0]; hist[0] = have ? t : Tmcc();
        Tmcc use = hist[2].valid ? hist[2] : hist[1].valid ? hist[1] : hist[0];
        if (!use.valid) { publish(2); return; }
        if (!cfg.valid || use != cfg) applyConfig(use);
        if (!layout.ok || active < 0) { publish(3); return; }
        // the selected stream: inner code, de-interleaving, energy dispersal, Reed-Solomon
        std::vector<uint8_t> inter[kFramesPerSuper];
        for (int f = 0; f < kFramesPerSuper; f++) inter[f].assign((size_t)kSlots * kSlotBytes, 0);
        for (int f = 0; f < kFramesPerSuper; f++) decodeFrame(f, inter);
        tsOut.clear();
        for (int f = 0; f < kFramesPerSuper; f++)
            for (int s = 0; s < kSlots; s++) {
                if (!wanted(s)) continue;
                uint8_t blk[204];
                blk[0] = 0x47;
                for (int j = 0; j < kSlotBytes; j++) {
                    int F, k;
                    interleavePos(f, j, F, k);
                    blk[j + 1] = inter[F][(size_t)s * kSlotBytes + (size_t)k] ^ prbsByte(f, s, j + 1);
                }
                const int r = fecp::rs204Decode(blk);
                packets++;
                if (r < 0) rsFailed++; else if (r == 0) rsClean++; else rsCorrected++;
                blk[0] = 0x47;
                if (r < 0) blk[1] |= 0x80;   // transport_error_indicator
                tsOut.insert(tsOut.end(), blk, blk + 188);
            }
        // a few data symbols for the constellation
        consts.clear();
        for (int s = 0; s < kSlots; s++) if (wanted(s)) {
            for (int i = 0; i < kSlotSyms && consts.size() < 400; i += 2) consts.push_back(sfData[7][(size_t)dataPos(s * kSlotSyms + i)]);
            break;
        }
        const double secs = kFramesPerSuper * kFrameSeconds;
        { std::lock_guard<std::mutex> l(mu); tel.superFrames++; }
        const size_t np = tsOut.size() / 188;
        if (np) {
            lastPacket = std::chrono::steady_clock::now();
            if (onPackets) onPackets(tsOut.data(), np, secs);
        }
        tsRateSecs += secs; tsRatePackets += np;
        if (tsRateSecs >= 1.0) { tsBitrate = (double)tsRatePackets * 188 * 8 / tsRateSecs; tsRateSecs = 0; tsRatePackets = 0; }
        publish(np ? 4 : 3);
    }

    void run() {
        for (;;) {
            if (!locked && skipSyms) {
                const size_t k = std::min(skipSyms, sy.size() - head);
                head += k; skipSyms -= k;
                if (skipSyms) break;
            }
            bool progressed = locked ? frame() : search();
            if (!progressed) break;
            // free what has been used
            if (head > (size_t)kFrameSyms * 2 + 4096) {
                const size_t cut = head - kBack;
                sy.erase(sy.begin(), sy.begin() + (long)cut);
                head -= cut;
            }
        }
    }
};

IsdbsReceiver::IsdbsReceiver() : p_(new Impl) {}
IsdbsReceiver::~IsdbsReceiver() = default;

void IsdbsReceiver::configure(double inputRateHz) {
    Impl& m = *p_;
    m.rate = inputRateHz;
    m.configured = ready();
    if (m.configured) {
        dvbs::DvbsFront::Config c;
        c.fs = inputRateHz; c.symbolRate = kSymbolRate; c.rollOff = kRollOff; c.centerHz = m.offsetHz;
        m.front.configure(c);
    }
    { std::lock_guard<std::mutex> l(m.mu); m.tel.inputRate = inputRateHz; }
    m.resetAll();
}

bool IsdbsReceiver::ready() const { return p_->rate >= kMinRate; }

void IsdbsReceiver::reset() { if (p_->configured) p_->resetAll(); }

void IsdbsReceiver::setSignalOffset(double hz) {
    p_->offsetHz = hz;
    if (p_->configured) p_->front.setCenter(hz);
}

void IsdbsReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> l(p_->mu); p_->centerHz = hz; p_->tel.centerHz = hz; }

void IsdbsReceiver::selectStream(int rel) { p_->selRequest = rel; }

void IsdbsReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->log = std::move(cb); }
void IsdbsReceiver::setPacketCallback(std::function<void(const uint8_t*, size_t, double)> cb) { p_->onPackets = std::move(cb); }

void IsdbsReceiver::feed(const std::complex<float>* x, size_t n) {
    Impl& m = *p_;
    if (!m.configured || !n) return;
    m.front.process(x, n, m.sy);
    m.run();
    // a stream change the interface asked for
    if (m.cfg.valid && m.selRequest != m.active && m.selRequest >= 0 && m.cfg.packetsOf(m.selRequest) > 0) { Tmcc t = m.cfg; m.applyConfig(t); }
}

bool IsdbsReceiver::telemetry(IsdbsTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->tel.seq == lastSeq) return false;
    out = p_->tel;
    return true;
}

ModeTuning isdbsTuning() {
    ModeTuning t;
    t.stdMode = 156;                       // family 154, engine standard 156
    t.id = "isdbs";
    t.name = "ISDB-S (BS)";
    t.minMhz = 950; t.maxMhz = 2150; t.defMhz = 1049.48;   // L-band IF after an LNB; BS-1 at 11727.48 MHz with the usual 10.678 GHz oscillator
    t.sampleRate = 48e6;
    t.bandwidthMhz = 34.5;
    t.minSampleRate = kMinRate;
    t.carrierAtCentre = true;
    return t;
}

std::string isdbsSummary(const IsdbsTelemetry& t) {
    char b[200];
    if (t.state == 0) return "ISDB-S: no input";
    if (t.state == 1) return "ISDB-S: searching for a BS transponder";
    if (t.state == 2) { snprintf(b, sizeof b, "ISDB-S: frame sync, waiting for the TMCC (Es/N0 %.1f dB)", t.snrDb); return b; }
    snprintf(b, sizeof b, "ISDB-S: %zu streams, TS %d, %llu packets, RS fail %llu, Es/N0 %.1f dB", t.streams.size(), t.selected, (unsigned long long)t.packets, (unsigned long long)t.rsFailed, t.snrDb);
    return b;
}

}  // namespace dect2

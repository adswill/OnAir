// CDR receiver (see cdr_rx.h). GY/T 268.1-2013 physical layer and GY/T 268.2-2013 multiplex; the DRA+ sound is not decoded.
//
// Chain: resampling to 816 ksps; beacon search with a lag-Nb autocorrelation in two bands (inside +-100 kHz for the all-digital modes,
// 100 .. 250 kHz for the modes beside an analogue FM programme, whose carrier would swamp a full-band correlation); the spectrum mode and
// the whole carrier offset from the sync signal's sequence in the frequency domain, the beacon timing from its phase slope; per sub-frame:
// beacon tracking (timing, carrier), OFDM symbols, scattered pilots interpolated in time and frequency, the system information
// (Viterbi, CRC-6) of every sub-frame; per logical frame (after undoing the sub-frame allocation): the service description channel
// (de-interleaving, Viterbi, descrambling, control multiplex frame) and the service data (de-interleaving of 5.7, LDPC, descrambling,
// service multiplex frame).
#include "dect2/cdr_rx.h"
#include "dect2/cdr_defs.h"
#include "dect2/cdr_ldpc.h"
#include "dect2/cdr_mux.h"
#include "dect2/exact_resampler.h"
#include "dect2/channel_find.h"
#include "dect2/fftutil.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

using namespace cdr;

namespace {
// whole carriers of offset tried at the beacon (about +-19 kHz): 50 ppm in band II, plus the error of a channel found off centre from its
// spectrum (the analogue FM programme of the hybrid modes shows its middle only to a few kHz)
constexpr int kMaxShift = 24;

using cd = std::complex<double>;
constexpr int kSearchFft = 2048, kSearchHop = 1024, kSearchDelay = 512;

// ---------------------------------------------------------------- beacon search

// Overlap-save band split: inner |f| < 104 kHz, outer 96 .. 254 kHz (zero phase; the output lags the input by kSearchDelay samples)
class SearchBands {
public:
    SearchBands() : fft_(kSearchFft), in_((size_t)kSearchFft), out_((size_t)kSearchFft), hist_((size_t)kSearchHop, cf32(0, 0)), a_((size_t)kSearchFft), b_((size_t)kSearchFft) {
        auto taper = [](double f, double edge, double w) {   // 1 below edge - w/2, 0 above edge + w/2
            if (f <= edge - w / 2) return 1.0;
            if (f >= edge + w / 2) return 0.0;
            return 0.5 + 0.5 * std::cos(M_PI * (f - (edge - w / 2)) / w);
        };
        for (int k = 0; k < kSearchFft; k++) {
            const double f = std::fabs((k < kSearchFft / 2 ? k : k - kSearchFft) * kFs / kSearchFft);
            in_[(size_t)k] = (float)(taper(f, 104e3, 8e3) / kSearchFft);
            out_[(size_t)k] = (float)((1.0 - taper(f, 96e3, 8e3)) * taper(f, 254e3, 8e3) / kSearchFft);
        }
    }
    void process(const cf32* x, cf32* inner, cf32* outer) {
        std::copy(hist_.begin(), hist_.end(), a_.begin());
        std::copy(x, x + kSearchHop, a_.begin() + kSearchHop);
        std::copy(x, x + kSearchHop, hist_.begin());
        fft_.forward(a_.data());
        for (int k = 0; k < kSearchFft; k++) { b_[(size_t)k] = a_[(size_t)k] * out_[(size_t)k]; a_[(size_t)k] *= in_[(size_t)k]; }
        fft_.inverse(a_.data());
        fft_.inverse(b_.data());
        std::copy(a_.begin() + kSearchDelay, a_.begin() + kSearchDelay + kSearchHop, inner);
        std::copy(b_.begin() + kSearchDelay, b_.begin() + kSearchDelay + kSearchHop, outer);
    }
private:
    Fft fft_;
    std::vector<float> in_, out_;
    std::vector<cf32> hist_, a_, b_;
};

struct Candidate { int band = 0, lag = 0; int64_t dStart = 0, dEnd = 0; cd sumP; double meanM = 0; };

// Normalised autocorrelation at lag `lag` over a window of `lag`: the beacon's two equal halves give a plateau as long as its prefix
class AutoCorr {
public:
    AutoCorr(int band, int lag) : band_(band), lag_(lag), ring_((size_t)(2 * lag), cf32(0, 0)) {}
    int band() const { return band_; }
    // r: filtered sample with absolute index idx (consecutive)
    bool push(int64_t idx, cf32 r, Candidate& out) {
        const size_t s = (size_t)(2 * lag_);
        const cf32 old = ring_[(size_t)(idx % (int64_t)s)], mid = ring_[(size_t)((idx - lag_) % (int64_t)s + (int64_t)s) % s];
        p_ += cd(mid * std::conj(r)) - cd(old * std::conj(mid));
        e1_ += (double)std::norm(mid) - (double)std::norm(old);
        e2_ += (double)std::norm(r) - (double)std::norm(mid);
        ring_[(size_t)(idx % (int64_t)s)] = r;
        if (++count_ < 2 * lag_ + 8) return false;
        if (++sinceFix_ > 200000) {                       // keep the running sums exact
            sinceFix_ = 0;
            p_ = 0; e1_ = 0; e2_ = 0;
            for (int k = 0; k < lag_; k++) {
                const cf32 a = ring_[(size_t)((idx - 2 * lag_ + 1 + k) % (int64_t)s + (int64_t)s) % s], b = ring_[(size_t)((idx - lag_ + 1 + k) % (int64_t)s + (int64_t)s) % s];
                p_ += cd(a * std::conj(b)); e1_ += std::norm(a); e2_ += std::norm(b);
            }
        }
        const int64_t d = idx - 2 * lag_ + 1;
        const double m = std::norm(p_) / (e1_ * e2_ + 1e-30);
        const bool above = m >= 0.04 && e1_ > 0 && e2_ > 0;
        if (!run_) {
            if (!above) return false;
            run_ = true; tooLong_ = false; start_ = d; ms_.clear(); ps_.clear();
        }
        if (above) last_ = d;
        if (!tooLong_) {
            ms_.push_back((float)m);
            ps_.push_back(p_);
            if ((int)ms_.size() > 4 * lag_ + 1500) { tooLong_ = true; ms_.clear(); ps_.clear(); }   // a carrier or a steady tone: not a beacon
        }
        if (d - last_ <= 32) return false;
        run_ = false;
        if (tooLong_) return false;
        // the plateau: where the smoothed metric stays within 90 % of its peak
        const int n = (int)(last_ - start_ + 1);
        if (n < 40) return false;
        std::vector<float> sm((size_t)n);
        double acc = 0;
        const int w = 16;
        for (int i = 0; i < n + w; i++) {
            if (i < n) acc += ms_[(size_t)i];
            if (i - 2 * w - 1 >= 0) acc -= ms_[(size_t)(i - 2 * w - 1)];
            const int c = i - w;
            if (c >= 0 && c < n) sm[(size_t)c] = (float)(acc / (std::min(i, n - 1) - std::max(0, i - 2 * w) + 1));
        }
        int peak = 0;
        for (int i = 1; i < n; i++) if (sm[(size_t)i] > sm[(size_t)peak]) peak = i;
        if (sm[(size_t)peak] < 0.08f) return false;
        const float thr = 0.9f * sm[(size_t)peak];
        int a = peak, b = peak;
        while (a > 0 && sm[(size_t)(a - 1)] >= thr) a--;
        while (b + 1 < n && sm[(size_t)(b + 1)] >= thr) b++;
        if (b - a + 1 < 100 || b - a + 1 > 1200) return false;
        cd sumP = 0;
        double sumM = 0;
        for (int i = a; i <= b; i++) { sumP += ps_[(size_t)i]; sumM += ms_[(size_t)i]; }
        out.band = band_; out.lag = lag_; out.dStart = start_ + a; out.dEnd = start_ + b; out.sumP = sumP; out.meanM = sumM / (b - a + 1);
        return true;
    }
private:
    int band_, lag_;
    std::vector<cf32> ring_;
    cd p_;
    double e1_ = 0, e2_ = 0;
    int64_t count_ = 0, sinceFix_ = 0;
    bool run_ = false, tooLong_ = false;
    int64_t start_ = 0, last_ = 0;
    std::vector<float> ms_;
    std::vector<cd> ps_;
};

// differential correlation of the sync carriers with Pb: magnitude 0..1 and the phase step per carrier
struct SyncFit { double metric = 0, phase = 0, coherent = 0; int shift = 0; };
// Y2 (optional): the second copy of the sync signal, which adds coherently
SyncFit syncCorrelate(const std::vector<cf32>& Y, const std::vector<cf32>* Y2, int nb, const Layout& L, int shift) {
    cd d = 0;
    double nrm = 0;
    for (int w = 0; w < (Y2 ? 2 : 1); w++) {
        const std::vector<cf32>& y = w ? *Y2 : Y;
        for (size_t n = 0; n + 1 < L.syncCarrier.size(); n++) {
            if (L.syncCarrier[n + 1] != L.syncCarrier[n] + 1) continue;
            const cf32 y1 = y[(size_t)(((L.syncCarrier[n] + shift) % nb + nb) % nb)], y2 = y[(size_t)(((L.syncCarrier[n + 1] + shift) % nb + nb) % nb)];
            d += cd(y2 * std::conj(y1) * std::conj(L.beaconSeq[n + 1] * std::conj(L.beaconSeq[n])));
            nrm += std::abs(y1) * std::abs(y2);
        }
    }
    SyncFit f;
    f.metric = nrm > 0 ? std::abs(d) / nrm : 0;
    f.coherent = std::abs(d);
    f.phase = std::arg(d);
    f.shift = shift;
    return f;
}

// logical sub-frame as demodulated: equalised cells and their noise variance
struct SubData {
    bool valid = false;
    int64_t superTag = 0;
    SysInfo si;
    std::vector<cf32> z;
    std::vector<float> nv;
};

} // namespace

struct CdrReceiver::Impl {
    std::mutex mu;                         // guards the members up to pub
    std::function<void(const std::string&)> log;
    double rate = 0;
    CdrTelemetry pub;
    std::atomic<bool> resetReq{true};
    // receiver thread
    CdrTelemetry tel;
    double curRate = 0;
    int64_t nIn = 0;
    double nextReport = 0;
    double power = 0;
    int64_t nPower = 0;
    ExactResampler rs;
    std::vector<cf32> rsOut;
    // 816 ksps samples: buf[i] has the absolute index buf0 + i
    std::vector<cf32> buf;
    int64_t buf0 = 0;
    // search
    std::unique_ptr<SearchBands> bands;
    std::vector<AutoCorr> corr;
    std::vector<cf32> searchIn;             // samples waiting for a full block
    int64_t searchNext = 0;                 // absolute index of the next sample the search takes
    std::vector<Candidate> pending;
    // lock
    bool locked = false;
    int tm = 0, sm = 0;
    std::shared_ptr<const Layout> lay;
    double bs = 0;                          // start of the next beacon (absolute index, fractional)
    double cfo = 0;
    bool mirror = false;                    // the input is read conjugated (a mirrored spectrum)
    ChannelCentre cc;                       // moves a signal far from the middle of the sample band there
    std::vector<cf32> cin;
    int badBeacons = 0, badSi = 0;
    bool siSeen = false;                    // the system information has decoded since the lock
    int64_t nSub = 0;                       // sub-frames processed since the lock
    double bsAtLock = 0;
    int64_t searchFloor = 0;                // the search does not go back before the beacon of the last lock
    std::map<int, std::unique_ptr<Fft>> ffts;
    // logical frames
    std::array<std::array<SubData, 4>, 4> store;
    std::map<uint16_t, CdrServiceInfo> svc;
    std::vector<uint16_t> svcOrder;
    float snrAvg = 0;
    bool haveSmct = false;
    Smct smct;

    Fft& fft(int n) {
        auto& f = ffts[n];
        if (!f) f = std::make_unique<Fft>(n);
        return *f;
    }

    void say(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }

    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; }
        const uint64_t s = tel.seq;
        tel = CdrTelemetry();
        tel.seq = s;
        tel.inputRate = curRate;
        nIn = 0; nextReport = 0; power = 0; nPower = 0;
        rs = ExactResampler();
        if (curRate > 0) rs.configure(curRate, kFs);
        cc.configure(curRate, 100e3, 70e3, 6.0);
        buf.clear(); buf0 = 0; searchFloor = 0; mirror = false;
        startSearch(0);
        locked = false; lay.reset(); tm = sm = 0;
        for (auto& r : store) for (auto& s2 : r) s2 = SubData();
        svc.clear(); svcOrder.clear(); haveSmct = false; snrAvg = 0;
    }

    void startSearch(int64_t from) {
        bands = std::make_unique<SearchBands>();
        corr.clear();
        for (int b = 0; b < 2; b++) for (int lag : {512, 1024}) corr.emplace_back(b, lag);
        searchIn.clear();
        searchNext = from;
        pending.clear();
    }

    cf32 sample(int64_t idx) const {
        const int64_t i = idx - buf0;
        if (i < 0 || i >= (int64_t)buf.size()) return cf32(0, 0);
        return mirror ? std::conj(buf[(size_t)i]) : buf[(size_t)i];
    }
    int64_t bufEnd() const { return buf0 + (int64_t)buf.size(); }

    // n samples from absolute index `start`, with the carrier offset taken out (phase referred to the absolute index)
    void grab(int64_t start, int n, double f, cf32* out) const {
        const double w = -2 * M_PI * f / kFs;
        const double ph0 = std::fmod(w * (double)start, 2 * M_PI);
        cd rot(std::cos(ph0), std::sin(ph0));
        const cd step(std::cos(w), std::sin(w));
        for (int i = 0; i < n; i++) {
            out[i] = sample(start + i) * cf32((float)rot.real(), (float)rot.imag());
            rot *= step;
            if ((i & 255) == 255) rot /= std::abs(rot);
        }
    }

    // ------------------------------------------------------------ search and acquisition

    void runSearch() {
        while (searchNext < bufEnd()) {
            const int64_t take = std::min<int64_t>(bufEnd() - searchNext, kSearchHop - (int64_t)searchIn.size());
            for (int64_t i = 0; i < take; i++) searchIn.push_back(sample(searchNext + i));
            searchNext += take;
            if ((int)searchIn.size() < kSearchHop) break;
            cf32 inner[kSearchHop], outer[kSearchHop];
            bands->process(searchIn.data(), inner, outer);
            const int64_t first = searchNext - kSearchHop - kSearchDelay;      // absolute index of inner[0]
            searchIn.clear();
            for (int i = 0; i < kSearchHop; i++)
                for (auto& c : corr) {
                    Candidate cand;
                    if (first + i >= 0 && c.push(first + i, c.band() == 0 ? inner[i] : outer[i], cand)) pending.push_back(cand);
                }
        }
        // The candidates of one beacon (both bands, both lags, stray plateaus nearby) are judged together once all their samples are
        // here: the most coherent fit wins.
        while (!pending.empty()) {
            int64_t first = pending[0].dStart;
            for (const auto& c : pending) first = std::min(first, c.dStart);
            std::vector<Candidate> group;
            int64_t need = 0;
            for (const auto& c : pending)
                if (c.dStart - first < 8192) { group.push_back(c); need = std::max(need, c.dEnd + 2 * c.lag + 8 * 2288 + 4096); }
            if (bufEnd() < need || searchNext < first + 8192 + 4096) return;   // more of the group may still come
            pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Candidate& c) { return c.dStart - first < 8192; }), pending.end());
            Acq best;
            for (const auto& c : group) {
                const Acq a = evaluate(c);
                if (a.ok && a.metric > best.metric) best = a;
            }
            if (!best.ok) {
                // a beacon that fits nothing: perhaps the spectrum is mirrored (I and Q swapped by the radio or the file format)
                mirror = !mirror;
                for (const auto& c : group) {
                    const Acq a = evaluate(c);
                    if (a.ok && a.metric > best.metric) best = a;
                }
                if (!best.ok) mirror = !mirror;
            }
            if (best.ok) { lockOn(best); pending.clear(); return; }
        }
    }

    double pilotCoherence(const Layout& L, double s1, double f) {
        const cdr::TxParams& tp = *L.tp;
        cd d = 0;
        double nrm = 0;
        std::vector<cf32> x((size_t)tp.ns);
        const int bo = std::max(2, tp.tcp / 4);
        for (int a = 1; a <= 6; a++) {
            const int64_t st = (int64_t)std::llround(s1) + (int64_t)a * tp.ts + tp.tcp - bo;
            grab(st, tp.ns, f, x.data());
            fft(tp.ns).forward(x.data());
            int prevC = -1;
            for (int c = 0; c < L.cols; c++) {
                if (L.kind[(size_t)(a * L.cols + c)] != kElemPilot) continue;
                if (prevC >= 0 && c - prevC == 12 && L.carrier[(size_t)c] - L.carrier[(size_t)prevC] == 12) {
                    const cf32 y1 = x[(size_t)((L.carrier[(size_t)prevC] + tp.ns) % tp.ns)], y2 = x[(size_t)((L.carrier[(size_t)c] + tp.ns) % tp.ns)];
                    const cf32 p1 = L.pilot[(size_t)(a * L.cols + prevC)], p2 = L.pilot[(size_t)(a * L.cols + c)];
                    d += cd(y2 * std::conj(y1) * std::conj(p2 * std::conj(p1)));
                    nrm += std::abs(y1) * std::abs(y2) * std::abs(p1) * std::abs(p2);
                }
                prevC = c;
            }
        }
        return nrm > 0 ? std::abs(d) / nrm : 0;
    }

    struct Acq { bool ok = false; double metric = 0, s1 = 0, f = 0; int tm = 0, sm = 0; int64_t dEnd = 0; };

    Acq evaluate(const Candidate& c) {
        Acq r;
        const int nb = c.lag;
        const double frac = -std::arg(c.sumP) * kFs / (2 * M_PI * nb);
        const int64_t w0 = (c.dStart + c.dEnd) / 2;
        std::vector<cf32> Y((size_t)nb), Y2((size_t)nb);
        grab(w0, nb, frac, Y.data());
        fft(nb).forward(Y.data());
        grab(w0 + nb, nb, frac, Y2.data());
        fft(nb).forward(Y2.data());
        // every spectrum mode and whole-carrier shift: the one that gathers the most coherent energy on its sync carriers
        // (a mode whose carriers only catch the edge of the signal can look coherent on a few carriers, so the coherence alone does not choose)
        SyncFit best;
        int bestSm = 0;
        const int tmProbe = nb == 512 ? 2 : 1;
        // the inner band can only hold the all-digital modes; the outer band sees the hybrid modes and the edge of spectrum mode 2
        static const std::vector<int> innerModes = {1, 2}, outerModes = {2, 9, 10, 22, 23};
        for (int sm2 : c.band == 0 ? innerModes : outerModes) {
            const auto L = layoutFor(tmProbe, sm2);
            for (int q = -kMaxShift; q <= kMaxShift; q++) {
                const SyncFit f = syncCorrelate(Y, &Y2, nb, *L, q);
                // an analogue FM programme puts strong lines on the carriers of the all-digital modes, but they do not follow Pb:
                // only fits that are coherent compete on energy
                if (f.metric >= 0.55 && f.coherent > best.coherent) { best = f; bestSm = sm2; }
            }
        }
        if (bestSm == 0) return r;
        const double tau = -best.phase * nb / (2 * M_PI);              // the sync signal's period starts tau samples after w0 (mod nb)
        const double s1c = (double)c.dEnd + 2.0 * nb;
        const double s1 = (double)w0 + tau + nb * std::round((s1c - (double)w0 - tau) / nb);
        const double f = frac + best.shift * txParams(tmProbe)->dfb;
        int t = tmProbe;
        if (nb == 1024) {
            const double m1 = pilotCoherence(*layoutFor(1, bestSm), s1, f), m3 = pilotCoherence(*layoutFor(3, bestSm), s1, f);
            t = m3 > m1 ? 3 : 1;
        }
        r.ok = true; r.metric = best.metric; r.s1 = s1; r.f = f; r.tm = t; r.sm = bestSm; r.dEnd = c.dEnd;
        return r;
    }

    void lockOn(const Acq& r) {
        lay = layoutFor(r.tm, r.sm);
        tm = r.tm; sm = r.sm;
        cfo = r.f;
        bs = r.s1 - lay->tp->tb;
        while (bs + lay->tp->tbcp / 2 < (double)buf0) bs += kSubframeLen;   // the first beacon began before the samples we have: take the next one
        bsAtLock = bs;
        searchFloor = r.dEnd + 1;
        locked = true;
        badBeacons = 0; badSi = 0; nSub = 0; siSeen = false;
        tel.tm = tm; tel.sm = sm; tel.ni = lay->ni;
        tel.innerKhz = lay->spec->innerKhz; tel.outerKhz = lay->spec->outerKhz;
        tel.state = std::max(tel.state, 1);
        char b[160];
        snprintf(b, sizeof b, "CDR: beacon found, transmission mode %d, spectrum mode %d (%s), carrier offset %+.0f Hz", tm, sm, lay->spec->text, cfo);
        say(b);
    }

    void unlock(const char* why) {
        locked = false;
        lay.reset();
        tel.state = 0;
        tel.siValid = false;
        for (auto& r : store) for (auto& s2 : r) s2 = SubData();
        const int64_t from = std::max({buf0, (int64_t)bs - 4096, searchFloor});   // never the same beacon again
        startSearch(from);
        say(std::string("CDR: lost the signal (") + why + "), searching again");
    }

    // ------------------------------------------------------------ one sub-frame

    void processSubframe() {
        const Layout& L = *lay;
        const cdr::TxParams& tp = *L.tp;
        const int nb = tp.nb, ns = tp.ns, sn = L.sn, cols = L.cols;
        const int64_t b0 = (int64_t)std::llround(bs);
        // beacon: timing from the phase slope of the sync carriers, residual carrier offset from its two halves
        std::vector<cf32> y1((size_t)nb), y2((size_t)nb);
        const int64_t w0 = b0 + tp.tbcp / 2;
        grab(w0, nb, cfo, y1.data());
        grab(w0 + nb, nb, cfo, y2.data());
        fft(nb).forward(y1.data());
        fft(nb).forward(y2.data());
        const SyncFit fit = syncCorrelate(y1, &y2, nb, L, 0);
        cd half = 0;
        for (int k : L.syncCarrier) half += cd(y2[(size_t)((k + nb) % nb)] * std::conj(y1[(size_t)((k + nb) % nb)]));
        tel.syncMetric = (float)fit.metric;
        double terr = 0;
        if (fit.metric > 0.15) {
            const double tau = -fit.phase * nb / (2 * M_PI);
            terr = std::remainder((double)w0 + tau - ((double)b0 + tp.tbcp), (double)nb);
            const double df = std::arg(half) * kFs / (2 * M_PI * nb);
            cfo += 0.5 * df;
            badBeacons = 0;
        } else badBeacons++;
        const double sub0 = (double)b0 + (std::fabs(terr) < 64 ? terr : 0.0);     // this sub-frame's beacon start
        // OFDM symbols
        const int bo = std::max(2, tp.tcp / 4);
        std::vector<cf32> Y((size_t)(sn * cols)), x((size_t)ns);
        const int64_t s0 = (int64_t)std::llround(sub0);
        for (int a = 0; a < sn; a++) {
            const int64_t st = s0 + tp.tb + (int64_t)a * tp.ts + tp.tcp - bo;
            grab(st, ns, cfo, x.data());
            fft(ns).forward(x.data());
            for (int c = 0; c < cols; c++) {
                const int k = L.carrier[(size_t)c];
                const double ph = 2 * M_PI * (double)k * bo / ns;
                Y[(size_t)(a * cols + c)] = x[(size_t)((k + ns) % ns)] * cf32((float)std::cos(ph), (float)std::sin(ph));
            }
        }
        // channel: pilots, then linear interpolation in time (per column) and in frequency (per half-sub-band)
        std::vector<cf32> H((size_t)(sn * cols), cf32(0, 0));
        std::vector<uint8_t> have((size_t)(sn * cols), 0);
        std::vector<char> pilotCol((size_t)cols, 0);
        double dsum = 0;
        int dn = 0;
        for (int c = 0; c < cols; c++) {
            int prev = -1;
            for (int a = 0; a < sn; a++) {
                const size_t e = (size_t)(a * cols + c);
                if (L.kind[e] != kElemPilot) continue;
                pilotCol[(size_t)c] = 1;
                H[e] = Y[e] / L.pilot[e];
                have[e] = 1;
                if (prev < 0) for (int r = 0; r < a; r++) H[(size_t)(r * cols + c)] = H[e];
                else for (int r = prev + 1; r < a; r++) {
                    const float t = (float)(r - prev) / (float)(a - prev);
                    H[(size_t)(r * cols + c)] = H[(size_t)(prev * cols + c)] * (1 - t) + H[e] * t;
                }
                if (prev >= 3) {
                    // noise from the second difference of the pilots of one column (spacing 3 symbols)
                    const size_t pe = (size_t)(prev * cols + c), ppe = (size_t)((prev - 3) * cols + c);
                    if (have[ppe] && a - prev == 3) { dsum += std::norm(H[pe] - (H[ppe] + H[e]) * 0.5f) * std::norm(L.pilot[pe]); dn++; }
                }
                prev = a;
            }
            if (prev >= 0) for (int r = prev + 1; r < sn; r++) H[(size_t)(r * cols + c)] = H[(size_t)(prev * cols + c)];
        }
        const int halfLen = L.nv / 2;
        for (int a = 0; a < sn; a++) {
            for (int h0 = 0; h0 < cols; h0 += halfLen) {
                int prev = -1;
                for (int c = h0; c < h0 + halfLen; c++) {
                    if (!pilotCol[(size_t)c]) continue;
                    if (prev < 0) for (int k = h0; k < c; k++) H[(size_t)(a * cols + k)] = H[(size_t)(a * cols + c)];
                    else for (int k = prev + 1; k < c; k++) {
                        const float t = (float)(k - prev) / (float)(c - prev);
                        H[(size_t)(a * cols + k)] = H[(size_t)(a * cols + prev)] * (1 - t) + H[(size_t)(a * cols + c)] * t;
                    }
                    prev = c;
                }
                if (prev >= 0) for (int k = prev + 1; k < h0 + halfLen; k++) H[(size_t)(a * cols + k)] = H[(size_t)(a * cols + prev)];
            }
        }
        double hp = 0;
        for (const auto& h : H) hp += std::norm(h);
        hp /= (double)H.size();
        const double nvar = std::max(1e-12, dn > 0 ? dsum / dn / 1.5 : hp * 0.01);
        const float snr = (float)(10 * std::log10(hp / nvar + 1e-12));
        snrAvg = tel.subframes == 0 ? snr : 0.8f * snrAvg + 0.2f * snr;
        tel.snrDb = snrAvg;
        // equalise
        SubData sd;
        sd.z.resize(Y.size());
        sd.nv.resize(Y.size());
        for (size_t e = 0; e < Y.size(); e++) {
            const float g = std::norm(H[e]) + 1e-20f;
            sd.z[e] = Y[e] * std::conj(H[e]) / g;
            sd.nv[e] = (float)(nvar / g);
        }
        // channel plot: the middle symbol
        tel.chanDb.clear(); tel.chanKhz.clear();
        for (int c = 0; c < cols; c += 2) {
            tel.chanDb.push_back((float)(10 * std::log10(std::norm(H[(size_t)((sn / 2) * cols + c)]) + 1e-12)));
            tel.chanKhz.push_back((float)(L.carrier[(size_t)c] * tp.df / 1000.0));
        }
        // system information: every copy (two or three per half, every half) soft-combined
        float llr[kSiCoded] = {};
        std::vector<cf32> siPts;
        for (size_t e = 0; e < Y.size(); e++) {
            if (L.kind[e] != kElemSi) continue;
            float l2[2];
            demap(sd.z[e], sd.nv[e], kQpsk, l2, (float)std::sqrt(2.0));
            const int s = L.siSym[e];
            llr[2 * s] += l2[0]; llr[2 * s + 1] += l2[1];
            if (siPts.size() < 300) siPts.push_back(sd.z[e] * (float)M_SQRT1_2);
        }
        tel.siConst = siPts;
        float u[kSiCoded];
        const std::vector<int>& R = interleaver(kSiCoded);
        for (int n = 0; n < kSiCoded; n++) u[R[(size_t)n]] = llr[n];
        uint8_t bits[kSiBits];
        convDecode(u, kSiBits, bits);
        SysInfo si;
        tel.subframes++;
        if (siFromBits(bits, si) && si.spec == sm && si.alloc >= 1 && si.alloc <= 3) {
            tel.siOk++;
            badSi = 0;
            siSeen = true;
            tel.siValid = true;
            tel.state = std::max(tel.state, 2);
            tel.frame = si.frame; tel.subframe = si.subframe; tel.alloc = si.alloc; tel.sdiMod = si.sdiMod; tel.msdMod = si.msdMod;
            tel.hier = si.hier; tel.rate = si.rateHi; tel.rateLo = si.rateLo; tel.uniform = si.uniform; tel.multiFreq = si.multiFreq;
            tel.nextFreqCode = si.nextFreq; tel.nominalKhz = si.nominal * 50;
            int p, q;
            physToLogical(si.alloc, si.frame, si.subframe, p, q);
            sd.valid = true;
            sd.si = si;
            sd.superTag = nSub - (si.frame * 4 + si.subframe);
            store[(size_t)p][(size_t)q] = std::move(sd);
            bool full = true;
            for (int k = 0; k < 4; k++) full = full && store[(size_t)p][(size_t)k].valid && store[(size_t)p][(size_t)k].superTag == store[(size_t)p][(size_t)q].superTag;
            if (full) {
                decodeLogical(p);
                for (int k = 0; k < 4; k++) store[(size_t)p][(size_t)k].valid = false;
            }
        } else {
            tel.siBad++;
            badSi++;
        }
        // next beacon
        bs = sub0 + kSubframeLen;
        nSub++;
        if (nSub > 4) tel.timingDriftPpm = ((bs - bsAtLock) / ((double)nSub * kSubframeLen) - 1.0) * 1e6;
    }

    // ------------------------------------------------------------ one logical frame

    void decodeLogical(int p) {
        const Layout& L = *lay;
        const SysInfo si = store[(size_t)p][3].si;
        const int perSub = L.sn * L.cols;
        auto cell = [&](int pos, cf32& z, float& nv) {
            const SubData& s = store[(size_t)p][(size_t)(pos / perSub)];
            z = s.z[(size_t)(pos % perSub)];
            nv = s.nv[(size_t)(pos % perSub)];
        };
        tel.notes.clear();
        if (si.multiFreq) tel.notes.push_back("multi-frequency cooperation is signalled: the receiver stays on this frequency");
        // service description channel
        if (si.sdiMod <= 2) {
            const int mb = cdr::modBits(si.sdiMod);
            const int T = L.sdiBits(si.sdiMod);
            std::vector<float> v(L.sdisPos.size() * (size_t)mb), uu(v.size());
            std::vector<cf32> pts;
            for (size_t i = 0; i < L.sdisPos.size(); i++) {
                cf32 z; float nv;
                cell(L.sdisPos[i], z, nv);
                demap(z, nv, si.sdiMod, &v[i * (size_t)mb]);
                if (pts.size() < 400 && i % 3 == 0) pts.push_back(z);
            }
            tel.sdcConst = pts;
            const std::vector<int>& R = interleaver((int)v.size());
            for (size_t n = 0; n < v.size(); n++) uu[(size_t)R[n]] = v[n];
            std::vector<uint8_t> bits((size_t)T);
            convDecode(uu.data(), T, bits.data());
            scrambleBits(bits.data(), T);
            std::vector<uint8_t> bytes((size_t)((T + 7) / 8), 0xFF);
            for (int i = 0; i < T; i++) if (!bits[(size_t)i]) bytes[(size_t)(i / 8)] &= (uint8_t)~(0x80u >> (i % 8));
            ControlFrame cf;
            if (parseControlFrame(bytes.data(), bytes.size(), cf)) {
                tel.sdcOk++;
                tel.state = 3;
                tel.tablesOk += (uint64_t)cf.tablesOk; tel.tablesBad += (uint64_t)cf.tablesBad;
                tel.otherTables = cf.otherTables;
                if (cf.haveNit) {
                    const std::string name = nameText(cf.nit.name);
                    if (name != tel.network) say("CDR: network \"" + name + "\"");
                    tel.network = name;
                    tel.country = cf.nit.country;
                    tel.networkId = cf.nit.networkId;
                    tel.freqsMhz.clear();
                    for (uint32_t f : cf.nit.freqs) tel.freqsMhz.push_back(f * 10.0 / 1e6);
                    tel.nitUpdate = cf.nit.update;
                }
                if (cf.haveSmct) applySmct(cf.smct);
            } else tel.sdcBad++;
        } else tel.notes.push_back("the service description channel uses a reserved constellation code");
        // service data
        if (si.hier != 0) { tel.notes.push_back("hierarchical modulation of the service data is not decoded"); publishServices(); return; }
        if (!si.uniform) { tel.notes.push_back("unequal protection of the service data (code rates in the service description) is not decoded"); publishServices(); return; }
        if (si.msdMod > 2) { tel.notes.push_back("the service data uses a reserved constellation code"); publishServices(); return; }
        const int mb = cdr::modBits(si.msdMod);
        const CdrLdpc& code = cdrLdpc(si.rateHi);
        const int cw = L.codewordsFor(si.msdMod);
        std::vector<float> llr(L.msdsPos.size() * (size_t)mb);
        std::vector<cf32> pts;
        for (size_t m = 0; m < L.msdsPos.size(); m++) {
            cf32 z; float nv;
            cell(L.msdsPos[m], z, nv);
            demap(z, nv, si.msdMod, &llr[m * (size_t)mb]);
            if (pts.size() < 600 && m % 61 == 0) pts.push_back(z);
        }
        for (auto& v : llr) v = std::max(-60.f, std::min(60.f, v));
        tel.mscConst = pts;
        std::vector<uint8_t> info((size_t)cw * (size_t)code.k());
        int iters = 0, okWords = 0;
        for (int c = 0; c < cw; c++) {
            const CdrLdpc::Result r = code.decode(&llr[(size_t)c * kLdpcBits], &info[(size_t)c * (size_t)code.k()], 40);
            iters += r.iterations;
            if (r.ok) { tel.blocksOk++; okWords++; } else tel.blocksBad++;
        }
        tel.codewords = cw;
        tel.ldpcIterations = cw ? (float)iters / cw : 0;
        tel.logicalFrames++;
        scrambleBits(info.data(), (int)info.size());
        std::vector<uint8_t> bytes(info.size() / 8, 0);
        for (size_t i = 0; i < bytes.size() * 8; i++) if (info[i]) bytes[i / 8] |= (uint8_t)(0x80u >> (i % 8));
        tel.capacityBytes = (int)bytes.size();
        ServiceFrame sf;
        if (parseServiceFrame(bytes.data(), bytes.size(), sf)) {
            tel.muxOk++;
            tel.smctUpdate = sf.h.smctUpdate;
            applyServiceFrame(sf);
        } else tel.muxBad++;
        if (okWords < cw && tel.muxBad > 0 && tel.muxOk == 0) tel.status = "service data: the LDPC code words do not decode";
        publishServices();
    }

    void applySmct(const Smct& t) {
        smct = t;
        haveSmct = true;
        for (const auto& f : t.frames)
            for (size_t i = 0; i < f.services.size(); i++) {
                const uint16_t id = f.services[i];
                if (!svc.count(id)) {
                    svcOrder.push_back(id);
                    char b[80];
                    snprintf(b, sizeof b, "CDR: service %04X in multiplex frame %d", id, f.smfId);
                    say(b);
                }
                CdrServiceInfo& s = svc[id];
                s.id = id; s.smfId = f.smfId; s.subIndex = (int)i;
            }
    }

    void applyServiceFrame(const ServiceFrame& sf) {
        if (!haveSmct) return;      // the sub-frames are matched to services through the configuration table
        const SmctEntry* e = nullptr;
        for (const auto& f : smct.frames) if (f.smfId == sf.h.smfId) e = &f;
        if (!e) return;
        for (size_t i = 0; i < sf.subs.size() && i < e->services.size(); i++) {
            CdrServiceInfo& s = svc[e->services[i]];
            const ParsedSubFrame& ps = sf.subs[i];
            if (!ps.headerOk) { s.subBad++; continue; }
            s.subOk++;
            s.seen = true;
            s.audio = ps.sf.hasAudio;
            s.data = ps.sf.hasData;
            s.kbps = ps.len * 8 / 0.64 / 1000.0;
            s.streams.clear();
            for (const auto& st : ps.sf.streams) {
                CdrAudioInfo a;
                a.algo = st.algo;
                a.bitrate = st.rate100 >= 0 ? st.rate100 * 100 : 0;
                a.sampleRate = sampleRateHz(st.sampleRateCode);
                a.channelsCode = st.channelsCode;
                a.language = st.language;
                s.streams.push_back(a);
            }
            s.audioUnits = (int)ps.sf.audio.size();
            s.audioBytes = 0;
            for (const auto& u : ps.sf.audio) s.audioBytes += (int)u.data.size();
            s.dataBytes = 0;
            for (const auto& u : ps.sf.data) {
                s.dataBytes += (int)u.data.size();
                if (std::find(s.dataTypes.begin(), s.dataTypes.end(), u.type) == s.dataTypes.end()) s.dataTypes.push_back(u.type);
                if (u.type == 160) {
                    const std::string t = printableText(u.data);
                    if (!t.empty() && t != s.text) { s.text = t; say("CDR text: " + t); }
                }
            }
        }
        tel.dataValid = true;
    }

    void publishServices() {
        tel.services.clear();
        for (uint16_t id : svcOrder) tel.services.push_back(svc[id]);
    }

    void report() {
        tel.seq++;
        tel.timeSec = curRate > 0 ? (double)nIn / curRate : 0;
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        tel.cfoHz = cfo + (mirror ? -1 : 1) * cc.offsetHz();
        if (!locked) tel.state = 0;
        if (tel.status.empty() || tel.state != 0) tel.status = locked ? cdrStateText(tel.state) : "searching for a CDR beacon";
        power = 0; nPower = 0;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }

    void work() {
        if (!locked) runSearch();
        while (locked) {
            const int64_t need = (int64_t)std::llround(bs) + kSubframeLen + 2048;
            if (bufEnd() < need) break;
            if ((int64_t)std::llround(bs) + lay->tp->tbcp / 2 < buf0) { unlock("timing"); break; }
            processSubframe();
            if (badBeacons >= 6 || badSi >= (siSeen ? 16 : 8)) { unlock(badBeacons >= 6 ? "no beacon" : "no system information"); break; }
        }
        // keep what the next step still needs
        int64_t keep = locked ? (int64_t)std::llround(bs) - 4096 : std::min(searchNext - kSearchHop, pending.empty() ? searchNext : pending.front().dStart) - 4096;
        for (const auto& c : pending) keep = std::min(keep, c.dStart - 4096);
        const int64_t drop = std::min<int64_t>(keep - buf0, (int64_t)buf.size());
        if (drop > 65536) { buf.erase(buf.begin(), buf.begin() + (long)drop); buf0 += drop; }
    }
};

CdrReceiver::CdrReceiver() : p_(std::make_unique<Impl>()) {}
CdrReceiver::~CdrReceiver() = default;

// configure() and reset() may come from another thread than feed(): they only leave a request that feed() carries out
void CdrReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->resetReq = true;
}
bool CdrReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= cdrTuning().minSampleRate - 1;
}
void CdrReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    p_->pub = CdrTelemetry();
    p_->pub.seq = s;
}

void CdrReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.curRate < cdrTuning().minSampleRate - 1) return;
    size_t done = 0;
    while (done < n) {
        // in pieces, so that a report falls every quarter second of signal
        const size_t chunk = std::min(n - done, (size_t)std::max<int64_t>(1, (int64_t)(m.nextReport - (double)m.nIn) + 1));
        // a signal away from the middle of the sample band (beyond the +-100 kHz of the beacon search) is moved there first
        m.cin.assign(x + done, x + done + chunk);
        m.cc.process(m.cin.data(), chunk, m.locked && m.tel.siValid);
        if (m.cc.takeChanged()) {
            m.buf0 = m.bufEnd(); m.buf.clear(); m.searchFloor = m.buf0;
            m.locked = false; m.lay.reset(); m.tel.state = 0;
            m.startSearch(m.buf0);
        }
        const cf32* xs = m.cin.data();
        for (size_t i = 0; i < chunk; i++) m.power += (double)std::norm(xs[i]);
        m.nPower += (int64_t)chunk;
        m.nIn += (int64_t)chunk;
        m.rsOut.clear();
        m.rs.process(xs, chunk, m.rsOut);
        m.buf.insert(m.buf.end(), m.rsOut.begin(), m.rsOut.end());
        m.work();
        done += chunk;
        while ((double)m.nIn >= m.nextReport) {
            m.nextReport += 0.25 * m.curRate;
            m.report();
        }
    }
}

bool CdrReceiver::telemetry(CdrTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void CdrReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning cdrTuning() {
    ModeTuning t;
    t.stdMode = 24; t.id = "cdr"; t.name = "CDR";
    t.minMhz = 87; t.maxMhz = 108; t.defMhz = 106.1;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.5;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 0;                  // no offset: the digital spectrum sits either side of the station's centre
    return t;
}

} // namespace dect2

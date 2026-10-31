// The SSB scanner: Welch spectrum of the captured span -> narrow occupied bands -> sideband decision -> per candidate a baseband channel (CIC3 to
// ~48 kHz, mix the suppressed carrier to 0, FIR, decimate to ~8 kHz) -> VoiceAnalyzer. Tracks keep a station across windows and follow its drift.
#include "dsp_util.h"
#include "dect2/ssbconv/ssbconv.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

namespace dect2::ssbconv {
using detail::fft;
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kCarrierToEdge = 160.0;   // the suppressed carrier sits about this far outside the edge found at the threshold of the 250 Hz smoothed spectrum (the voice band starts 300 Hz from it)

// ---- stage 1: mix by mixHz, CIC3 decimate by D (exact integer arithmetic, wrap-around), output scaled to unit gain. Returns the first valid output only.
void stage1(const cf* x, size_t n, double rate, double mixHz, int D, std::vector<cf>& out) {
    out.clear();
    if (D < 1) D = 1;
    out.reserve(n / (size_t)D + 4);
    const double w = -2.0 * kPi * mixHz / rate;
    cf rot(1, 0); const cf step((float)std::cos(w), (float)std::sin(w));
    uint64_t i1r = 0, i2r = 0, i3r = 0, i1i = 0, i2i = 0, i3i = 0;
    uint64_t z1r = 0, z2r = 0, z3r = 0, z1i = 0, z2i = 0, z3i = 0;
    const double q = 1048576.0;
    const double norm = 1.0 / (q * (double)D * (double)D * (double)D);
    int cnt = 0, skip = 3;
    for (size_t i = 0; i < n; i++) {
        const cf m = x[i] * rot;
        rot *= step;
        if ((i & 255) == 255) rot /= std::abs(rot);
        auto qz = [&](float v) { double d = (double)v * q; d = d > 1.0e9 ? 1.0e9 : d < -1.0e9 ? -1.0e9 : d; return (uint64_t)(int64_t)std::llround(d); };
        i1r += qz(m.real()); i2r += i1r; i3r += i2r;
        i1i += qz(m.imag()); i2i += i1i; i3i += i2i;
        if (++cnt == D) {
            cnt = 0;
            uint64_t a = i3r - z1r; z1r = i3r; uint64_t b = a - z2r; z2r = a; uint64_t c = b - z3r; z3r = b;
            uint64_t ai = i3i - z1i; z1i = i3i; uint64_t bi = ai - z2i; z2i = ai; uint64_t ci = bi - z3i; z3i = bi;
            if (skip > 0) { skip--; continue; }
            out.push_back(cf((float)((double)(int64_t)c * norm), (float)((double)(int64_t)ci * norm)));
        }
    }
}

// ---- stage 2: mix by -offHz (the carrier to 0), complex FIR low-pass (3.2 kHz pass, 4.8 kHz stop for ~8 kHz output), decimate by Q
std::vector<float> lpTaps(double rate1) {
    const double pass = 3200, stop = 4800;
    const double fc = 0.5 * (pass + stop) / rate1, tr = (stop - pass) / rate1;
    int n = (int)std::ceil(4.0 / tr) | 1;
    std::vector<float> h((size_t)n);
    double s = 0;
    for (int i = 0; i < n; i++) {
        const double m = i - (n - 1) / 2.0;
        const double sinc = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
        const double wd = 0.42 - 0.5 * std::cos(2 * kPi * i / (n - 1)) + 0.08 * std::cos(4 * kPi * i / (n - 1));
        h[(size_t)i] = (float)(sinc * wd); s += sinc * wd;
    }
    for (float& v : h) v = (float)(v / s);
    return h;
}

void stage2(const std::vector<cf>& x1, double rate1, double offHz, int Q, std::vector<cf>& out) {
    out.clear();
    const std::vector<float> h = lpTaps(rate1);
    const size_t N = h.size();
    if (x1.size() < N + (size_t)Q * 4) return;
    std::vector<cf> m(x1.size());
    const double w = -2.0 * kPi * offHz / rate1;
    cf rot(1, 0); const cf step((float)std::cos(w), (float)std::sin(w));
    for (size_t i = 0; i < x1.size(); i++) { m[i] = x1[i] * rot; rot *= step; if ((i & 255) == 255) rot /= std::abs(rot); }
    out.reserve(x1.size() / (size_t)Q + 2);
    for (size_t p = 0; p + N <= m.size(); p += (size_t)Q) {
        float ar = 0, ai = 0;
        for (size_t k = 0; k < N; k++) { ar += h[k] * m[p + k].real(); ai += h[k] * m[p + k].imag(); }
        out.push_back(cf(ar, ai));
    }
}

// Keeps only the wanted sideband of a baseband window (the carrier at 0, the voice at +150 .. +3150 Hz) and drops what is on the other side of the
// carrier: an SSB neighbour 3.3 - 5 kHz below the dial has its edge at -1.5 .. -0.3 kHz in this channel. 'Valid' output only (taps - 1 samples shorter).
std::vector<cf> sidebandFilter(const cf* x, size_t n, double rate) {
    constexpr size_t T = 121;
    static thread_local double cachedRate = 0;
    static thread_local std::vector<cf> h;
    if (rate != cachedRate) {
        h.assign(T, cf(0, 0));
        const double fc = 1500.0 / rate, shift = 1650.0 / rate;
        double sum = 0;
        std::vector<double> lp(T);
        for (size_t k = 0; k < T; k++) {
            const double m = (double)k - (double)(T - 1) / 2;
            const double sinc = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
            const double wd = 0.42 - 0.5 * std::cos(2 * kPi * (double)k / (double)(T - 1)) + 0.08 * std::cos(4 * kPi * (double)k / (double)(T - 1));
            lp[k] = sinc * wd; sum += lp[k];
        }
        for (size_t k = 0; k < T; k++) {
            const double m = (double)k - (double)(T - 1) / 2, a = -2 * kPi * shift * m;   // correlation form below: the response is the mirror image of this kernel's
            h[k] = cf((float)(lp[k] / sum * std::cos(a)), (float)(lp[k] / sum * std::sin(a)));
        }
        cachedRate = rate;
    }
    std::vector<cf> y;
    if (n < T + 8) return y;
    y.resize(n - T + 1);
    for (size_t i = 0; i + T <= n; i++) {
        float ar = 0, ai = 0;
        for (size_t k = 0; k < T; k++) { ar += h[k].real() * x[i + k].real() - h[k].imag() * x[i + k].imag(); ai += h[k].real() * x[i + k].imag() + h[k].imag() * x[i + k].real(); }
        y[i] = cf(ar, ai);
    }
    return y;
}

double medianOf(std::vector<double> v) { if (v.empty()) return 0; std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end()); return v[v.size() / 2]; }
}  // namespace

struct Scanner::Impl {
    ScannerOptions o;
    std::vector<cf> win;
    size_t fill = 0, winN = 0;
    double rate = 0, centre = 0;
    struct Track {
        uint32_t id = 0;
        double dial = 0, bw = 0, low = 0, high = 0;
        Side side = Side::Unknown;
        double vote = 0;
        VoiceAnalyzer va;
        double firstSeen = 0, lastSeen = 0, sinceT = 0;
        bool confirmed = false, vetoed = false;
        std::deque<float> sc;
        int lowRun = 0;
        double lastVeto = -1e9;
        float snr = 0, level = 0, pitch = 0, tilt = 0;
        int seen = 0;
    };
    std::vector<Track> tracks;
    uint32_t nextId = 1;
    std::vector<Band> bands;
    double noiseDb = -100, busy = 0;

    struct Cand { double low, high, bw, snr, level, tilt, vote, dialU, dialL; double peakDb; bool present; };
    std::vector<double> held;   // decayed max-hold of the smoothed spectrum: the shape of a station over a few seconds (speech has gaps in every window)

    void analyse(double tw, std::vector<Detection>& out);
    std::vector<Cand> findCandidates(double& noiseChanDb);
};

Scanner::Scanner(const ScannerOptions& o) : p_(new Impl) { p_->o = o; }
Scanner::~Scanner() = default;
void Scanner::setOptions(const ScannerOptions& o) { p_->o = o; p_->fill = 0; }
const ScannerOptions& Scanner::options() const { return p_->o; }
void Scanner::reset() { p_->tracks.clear(); p_->fill = 0; p_->bands.clear(); }
std::vector<Scanner::Band> Scanner::lastBands() const { return p_->bands; }
double Scanner::noiseFloorDb() const { return p_->noiseDb; }
size_t Scanner::trackCount() const { return p_->tracks.size(); }
double Scanner::busy() const { return p_->busy; }

std::vector<Detection> Scanner::feed(const cf* x, size_t n, double rateHz, double centreHz, double tEnd) {
    Impl& s = *p_;
    std::vector<Detection> out;
    if (rateHz < 16000 || n == 0) return out;
    if (rateHz != s.rate || centreHz != s.centre) { s.rate = rateHz; s.centre = centreHz; s.fill = 0; }
    s.winN = (size_t)std::llround(s.o.windowSec * rateHz);
    if (s.win.size() != s.winN) { s.win.assign(s.winN, cf(0, 0)); s.fill = 0; }
    size_t pos = 0;
    while (pos < n) {
        const size_t take = std::min(n - pos, s.winN - s.fill);
        std::copy(x + pos, x + pos + take, s.win.begin() + (long)s.fill);
        s.fill += take; pos += take;
        if (s.fill == s.winN) {
            const auto t0 = std::chrono::steady_clock::now();
            s.analyse(tEnd - (double)(n - pos) / rateHz, out);
            const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            s.busy = dt / s.o.windowSec;
            s.fill = 0;
        }
    }
    return out;
}

std::vector<Scanner::Impl::Cand> Scanner::Impl::findCandidates(double& noiseChanDb) {
    std::vector<Cand> res;
    // spectrum size
    size_t N = 1024;
    while ((double)N < rate / 45.0 && N < (size_t)o.maxFft) N <<= 1;
    while (N > winN && N > 256) N >>= 1;
    const size_t nb = std::max<size_t>(1, std::min<size_t>((size_t)o.maxFftBlocks, winN / N));
    const double binHz = rate / (double)N;
    std::vector<double> P(N, 0.0);
    std::vector<cf> buf(N);
    std::vector<float> wnd(N);
    double w2 = 0;
    for (size_t i = 0; i < N; i++) { wnd[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)N)); w2 += (double)wnd[i] * wnd[i]; }
    for (size_t b = 0; b < nb; b++) {
        const size_t s0 = nb == 1 ? 0 : b * (winN - N) / (nb - 1);
        for (size_t i = 0; i < N; i++) buf[i] = win[s0 + i] * wnd[i];
        fft(buf);
        for (size_t i = 0; i < N; i++) P[i] += std::norm(buf[i]);
    }
    for (double& v : P) v /= ((double)nb * (double)N * w2);
    // ascending frequency order: index j <-> f = (j - N/2) * binHz
    std::vector<double> Q(N);
    for (size_t j = 0; j < N; j++) Q[j] = P[(j + N / 2) % N];
    // the DC spike of a direct-conversion radio: the bins around 0 Hz take the level of their neighbours
    {
        const size_t h = std::max<size_t>(1, (size_t)std::ceil(o.dcGuardHz / binHz));
        if (N / 2 > h + 3) {
            const double a = Q[N / 2 - h - 2], b = Q[N / 2 + h + 2];
            for (size_t j = N / 2 - h; j <= N / 2 + h; j++) Q[j] = 0.5 * (a + b);
        }
    }
    // smoothing over ~250 Hz (the noise of the averaged spectrum is then about 0.5 dB, so a threshold a few dB above the floor is safe)
    const size_t sm = std::max<size_t>(1, (size_t)std::lround(250.0 / binHz)) | 1;
    std::vector<double> pre(N + 1, 0.0), S(N, 0.0);
    for (size_t j = 0; j < N; j++) pre[j + 1] = pre[j] + Q[j];
    for (size_t j = 0; j < N; j++) {
        const size_t a = j >= sm / 2 ? j - sm / 2 : 0, e = std::min(N, j + sm / 2 + 1);
        S[j] = (pre[e] - pre[a]) / (double)(e - a);
    }
    const double floorLin = medianOf(S);
    noiseDb = 10 * std::log10(floorLin + 1e-30);
    noiseChanDb = 10 * std::log10(floorLin * 6400.0 / binHz + 1e-30);
    const double decay = std::pow(10.0, -0.06);   // 0.6 dB per window
    if (held.size() != N) held = S;
    else for (size_t k = 0; k < N; k++) held[k] = std::max(S[k], held[k] * decay);
    const std::vector<double> Scur = S;
    S = held;   // the candidates are looked for in the held spectrum; whether the station talks right now is told by the current one
    const double thr = floorLin * std::pow(10.0, o.minSnrDb / 10.0);
    const size_t gap = std::max<size_t>(1, (size_t)(400.0 / binHz));
    auto freqOf = [&](double j) { return (j - (double)(N / 2)) * binHz; };
    size_t j = 0;
    while (j < N) {
        if (S[j] < thr) { j++; continue; }
        size_t a = j, e = j, miss = 0;
        for (size_t k = j; k < N; k++) {
            if (S[k] >= thr) { e = k; miss = 0; } else if (++miss > gap) break;
        }
        j = e + 1;
        // refine the edges: from the peak outwards down to peak - 30 dB (or the threshold)
        size_t pk = a; for (size_t k = a; k <= e; k++) if (S[k] > S[pk]) pk = k;
        const double thrE = std::max(thr, S[pk] * 3.2e-3);
        size_t lo = pk, hi = pk;
        for (size_t k = pk, ms = 0; k > a && ms <= gap; k--) { if (S[k - 1] >= thrE) { lo = k - 1; ms = 0; } else ms++; }
        for (size_t k = pk, ms = 0; k < e && ms <= gap; k++) { if (S[k + 1] >= thrE) { hi = k + 1; ms = 0; } else ms++; }
        // sub-bin edge
        auto cross = [&](size_t in, size_t outIdx) { const double a1 = S[in], b1 = S[outIdx]; const double t = a1 > b1 ? std::log(a1 / thrE) / std::log(a1 / std::max(b1, 1e-30)) : 0; return (double)in + ((double)outIdx - (double)in) * std::min(1.0, std::max(0.0, t)); };
        const double lowIdx = lo > 0 ? cross(lo, lo - 1) : (double)lo;
        const double highIdx = hi + 1 < N ? cross(hi, hi + 1) : (double)hi;
        Cand c{};
        c.low = freqOf(lowIdx); c.high = freqOf(highIdx); c.bw = c.high - c.low;
        if (c.bw < o.minBwHz * 0.8 || c.bw > o.maxBwHz * 1.15) continue;
        if (c.low < -rate / 2 + o.edgeGuardHz || c.high > rate / 2 - o.edgeGuardHz) continue;
        // strength: power in the band over the noise in the same width
        double ps = 0; size_t cntb = 0;
        for (size_t k = (size_t)std::max(0.0, std::floor(lowIdx)); k <= (size_t)std::min((double)N - 1, std::ceil(highIdx)); k++) { ps += Q[k]; cntb++; }
        const double pn = floorLin * (double)cntb;
        c.snr = 10 * std::log10(std::max(1e-3, (ps - pn) / pn));
        double pcur = 0;
        for (size_t k = (size_t)std::max(0.0, std::floor(lowIdx)); k <= (size_t)std::min((double)N - 1, std::ceil(highIdx)); k++) pcur += Scur[k];
        c.present = pcur / pn >= std::pow(10.0, 0.15);
        c.level = 10 * std::log10(ps + 1e-30);
        c.peakDb = 10 * std::log10(S[pk] / floorLin);
        // tilt over thirds (raw spectrum): the end that has more energy is the carrier side
        const double third = c.bw / 3;
        double pl = 0, ph = 0;
        for (size_t k = 0; k < N; k++) {
            const double f = freqOf((double)k);
            if (f >= c.low && f < c.low + third) pl += S[k];
            if (f > c.high - third && f <= c.high) ph += S[k];
        }
        pl = std::max(pl - floorLin * third / binHz, 1e-30); ph = std::max(ph - floorLin * third / binHz, 1e-30);
        c.tilt = 10 * std::log10(pl / ph);
        double vote = std::max(-1.0, std::min(1.0, c.tilt / 8.0));
        if (o.useConvention) { const Side cs = conventionSide(centre + 0.5 * (c.low + c.high)); vote += cs == Side::Usb ? 0.25 : cs == Side::Lsb ? -0.25 : 0; }
        c.vote = vote;
        c.dialU = centre + c.low - kCarrierToEdge;
        c.dialL = centre + c.high + kCarrierToEdge;
        res.push_back(c);
    }
    return res;
}

void Scanner::Impl::analyse(double tw, std::vector<Detection>& out) {
    double noiseChan = -100;
    std::vector<Cand> cands = findCandidates(noiseChan);
    bands.clear();
    // 1. match candidates to tracks
    std::vector<int> assign(cands.size(), -1);
    std::vector<char> used(tracks.size(), 0);
    for (size_t ci = 0; ci < cands.size(); ci++) {
        double best = o.freqTolHz + 1; int bi = -1;
        for (size_t ti = 0; ti < tracks.size(); ti++) {
            if (used[ti]) continue;
            const Track& t = tracks[ti];
            double d = 1e18;
            if (t.side == Side::Usb) d = std::fabs(cands[ci].dialU - t.dial);
            else if (t.side == Side::Lsb) d = std::fabs(cands[ci].dialL - t.dial);
            else d = std::min(std::fabs(cands[ci].dialU - t.dial), std::fabs(cands[ci].dialL - t.dial));
            if (d < best) { best = d; bi = (int)ti; }
        }
        if (bi < 0) {   // the band of a track already (the edges moved while the held spectrum filled out): the same station
            for (size_t ti = 0; ti < tracks.size(); ti++) {
                if (used[ti]) continue;
                const Track& t = tracks[ti];
                const double lo = std::max(t.low, cands[ci].low), hi = std::min(t.high, cands[ci].high);
                if (hi - lo > 0.6 * std::min(t.high - t.low, cands[ci].bw)) { bi = (int)ti; break; }
            }
        }
        if (bi >= 0) { assign[ci] = bi; used[(size_t)bi] = 1; }
    }
    // 2. new tracks, strongest candidate first; a piece of a band that a track already covers (a weak station's spectrum splits at a formant gap) is not a station
    std::vector<size_t> order;
    for (size_t ci = 0; ci < cands.size(); ci++) if (assign[ci] < 0) order.push_back(ci);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cands[a].snr > cands[b].snr; });
    for (size_t ci : order) {
        if (tracks.size() >= o.maxTracks) break;
        const Cand& c = cands[ci];
        bool piece = false;
        for (const Track& t : tracks) {
            if (t.seen == 0 && t.low == 0 && t.high == 0) continue;
            const double lo = std::max(t.low - 150, c.low), hi = std::min(t.high + 150, c.high);
            if (hi - lo > 0.25 * c.bw) { piece = true; break; }
        }
        if (piece) continue;
        Track t; t.id = nextId++; t.firstSeen = t.lastSeen = t.sinceT = tw; t.low = c.low; t.high = c.high;
        tracks.push_back(std::move(t));
        assign[ci] = (int)tracks.size() - 1;
    }
    // 3. update the tracks that have a candidate
    const int D = std::max(1, (int)std::lround(rate / 48000.0));
    const double rate1 = rate / D;
    const int Q = std::max(1, (int)std::lround(rate1 / 8000.0));
    const double rate2 = rate1 / Q;
    int extracted = 0;
    for (size_t ci = 0; ci < cands.size(); ci++) {
        const Cand& c = cands[ci];
        if (assign[ci] < 0) continue;
        Track& t = tracks[(size_t)assign[ci]];
        const bool first = t.seen == 0;
        if (tw - t.lastSeen > 10.0 && t.confirmed) t.sinceT = tw - o.windowSec;
        t.lastSeen = tw; t.seen++;
        // a candidate that is only a piece of the band the track already has (a gap in the speech spectrum) does not move the track
        const bool fragment = !first && t.high > t.low && c.bw < 0.75 * (t.high - t.low) && c.low >= t.low - 200 && c.high <= t.high + 200;
        if (!fragment) {
            t.vote = first ? c.vote : 0.85 * t.vote + 0.15 * c.vote;
            const Side side = t.vote >= 0 ? Side::Usb : Side::Lsb;
            const double dialNew = side == Side::Usb ? c.dialU : c.dialL;
            if (first || side != t.side || std::fabs(dialNew - t.dial) > o.freqTolHz) t.dial = dialNew; else t.dial = 0.6 * t.dial + 0.4 * dialNew;
            t.side = side; t.low = c.low; t.high = c.high; t.bw = c.bw; t.tilt = (float)c.tilt;
        }
        t.snr = (float)c.snr; t.level = (float)c.level;
        if (t.vetoed) continue;
        if (D > 1000 || extracted >= 16) continue;
        extracted++;
        // extract: stage 1 at the band centre (also what the digital veto sees), stage 2 puts the carrier at 0
        const double bandCentre = 0.5 * (c.low + c.high);
        std::vector<cf> x1, x2;
        stage1(win.data(), win.size(), rate, bandCentre, D, x1);
        stage2(x1, rate1, (t.dial - centre) - bandCentre, Q, x2);
        if (x2.size() < 400) continue;
        if (t.side == Side::Lsb) for (cf& v : x2) v = std::conj(v);   // the voice band is below the carrier: mirror it so that the analysis always sees 0 .. +3 kHz
        // drop the filter start
        const size_t drop = std::min<size_t>(x2.size() / 4, 40);
        std::vector<cf> xf = sidebandFilter(x2.data() + drop, x2.size() - drop, rate2);
        const WindowVoice wv = xf.empty() ? t.va.process(x2.data() + drop, x2.size() - drop, rate2, noiseChan)
                                          : t.va.process(xf.data(), xf.size(), rate2, noiseChan, x2.data() + drop, x2.size() - drop);
        const VoiceFeatures vf = t.va.features();
        t.pitch = vf.pitchHz;
        t.sc.push_back(vf.score);
        if (t.sc.size() > 12) t.sc.pop_front();
        const int good = (int)std::count_if(t.sc.begin(), t.sc.end(), [&](float v) { return v >= o.confirmScore; });
        if (!t.confirmed && good >= o.confirmWindows) t.confirmed = true;
        if (t.confirmed) { if (vf.score < 0.15f) t.lowRun++; else t.lowRun = 0; if (t.lowRun >= 20) { t.confirmed = false; t.sc.clear(); t.lowRun = 0; } }
        // second opinion for the unconfirmed
        if (!t.confirmed && o.digitalVeto && tw - t.lastVeto > 10.0 && t.va.frames() >= 150) {
            t.lastVeto = tw;
            std::vector<cf> xs; stage1(win.data(), win.size(), rate, bandCentre, D, xs);
            if (o.digitalVeto(xs.data(), xs.size(), rate1, bandCentre)) { t.vetoed = true; t.confirmed = false; continue; }
        }
        if (t.confirmed) {
            Detection d;
            d.trackId = t.id; d.time = tw; d.dialHz = t.dial; d.lowHz = centre + c.low; d.highHz = centre + c.high; d.bwHz = c.bw;
            d.side = t.side; d.sideConf = (float)std::min(1.0, std::fabs(t.vote));
            d.confirmed = true; d.voiceNow = wv.active; d.speech = vf.score; d.snrDb = (float)c.snr; d.levelDb = (float)c.level;
            d.pitchHz = t.va.pitchNow(); d.tiltDb = t.va.tiltNow(); d.sinceT = t.sinceT;
            d.audioRate = rate2;
            d.audio.resize(x2.size() - drop);
            for (size_t i = 0; i < d.audio.size(); i++) d.audio[i] = x2[drop + i].real();
            out.push_back(std::move(d));
        }
    }
    for (size_t ci = 0; ci < cands.size(); ci++) {
        if (assign[ci] < 0) continue;
        const Track& t = tracks[(size_t)assign[ci]];
        bands.push_back({centre + cands[ci].low, centre + cands[ci].high, t.confirmed, t.side});
    }
    // 4. forget
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const Track& t) {
        const double age = tw - t.lastSeen;
        if (t.confirmed) return age > o.dropSec;
        return age > (t.vetoed ? 120.0 : 8.0);
    }), tracks.end());
}

}  // namespace dect2::ssbconv

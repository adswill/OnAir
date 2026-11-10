#include "dect2/iono/iono_dsp.h"
#include "dect2/iono/iono_dechirp.h"
#include "dect2/results/results_io.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/iono/iono_scale.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <map>
#include <unordered_map>

namespace dect2 {
namespace iono {

namespace {

using cd = std::complex<double>;

constexpr int kM = 256;                 // dechirped FFT (decimated samples): kM x kDelayBin of delay, 20 kHz of sweep per block
constexpr double kKey = 0.01;           // start-time cells of the detector, s
constexpr int kPeaks = 8;               // strongest bins per detector FFT
constexpr double kThrDb = 11;           // detector bin over the mean noise
constexpr double kWinLo = -0.012, kWinHi = 0.040;   // hits within this of a sweep's start (delay spread and timing error) belong to it

struct Acc { int count = 0, frames = 0; uint64_t lastFrame = ~0ull; double w = 0, wtz = 0, last = -1e300, snr = 0; };

struct SounderAcc {
    IonoSounder info;
    int rateIdx = 0;
    std::vector<double> ion;            // kRows x kCols power sums
    std::vector<uint32_t> cnt;          // kCols blocks per column
    double armedFor = -1e300;           // the predicted start a sweep was opened for
    std::vector<double> recent;         // kRows x kCols: each column as the newest sweep through it left it (mean power), the scaling input
    std::vector<uint8_t> recentHave;
    std::vector<IonoTrace> traces;      // of the latest scaling
    std::vector<IonoHistPt> history;
};

struct Sweep {
    int rateIdx = 0;
    double r = 0;
    double t0 = 0;                      // relative to the origin
    int sounder = -1;                   // index in sounders
    bool predicted = false, confirmed = false, done = false;
    int hits = 0; double snrSum = 0;
    double fLo = 1e300, fHi = -1e300;
    uint64_t gEntry = 0, gExit = 0;     // the samples the model chirp is inside the usable span
    int D = 1; double fd = 1;
    cd z = 1.0, w = 1.0, dw = 1.0, acc = 0.0;
    int dc = 0;
    uint64_t blockG = 0;
    std::vector<cf32> blk;
    std::map<int, std::vector<double>> temp;   // predicted and not yet confirmed: column -> kRows sums and the block count
    std::map<int, std::vector<double>> own;    // everything of this sweep: column -> kRows sums and the block count (the scaling)
};

} // namespace

struct Processor::Impl {
    double fs = 0, fc = 0, origin = 0;
    bool locked = true;
    int selReq = -1;
    Observer obs;
    uint64_t g = 0;                     // samples fed
    int N = 0;
    std::unique_ptr<Fft> fft, fftM;
    std::vector<float> win, winM;
    std::vector<cf32> buf, tmp, tmpM;
    std::vector<float> pw, scratch;
    double frameDur = 0;
    std::unordered_map<int64_t, Acc> acc[kNumRates];
    double minSpan[kNumRates] = {};
    int minHits[kNumRates] = {};
    std::vector<Sweep> sweeps;
    std::vector<SounderAcc> sounders;
    std::vector<IonoEvent> timeline;
    std::vector<uint8_t> covered;
    uint64_t frames = 0, hits = 0;
    double pAcc = 0; uint64_t pN = 0; double powerDb = -200;
    bool retuned = false;

    double tAbs(double rel) const { return origin + rel; }

    void setup() {
        int n = 256;
        while (n < 65536 && n * 2 <= fs * 3.2e-3 * 1.414) n *= 2;
        N = n;
        fft = std::make_unique<Fft>(N);
        win.resize((size_t)N);
        for (int i = 0; i < N; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / N));
        buf.clear(); buf.reserve((size_t)N);
        tmp.resize((size_t)N); pw.resize((size_t)N);
        fftM = std::make_unique<Fft>(kM);
        winM.resize(kM);
        for (int i = 0; i < kM; i++) winM[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / kM));
        tmpM.resize(kM);
        frameDur = N / fs;
        // a wrong candidate rate r' sees a sweep of rate r drift through the start-time cells at |1 - r / r'| s per s; a sweep needs hits
        // for longer than the slowest such drift takes to cross the +-2 cells looked at together
        for (int i = 0; i < kNumRates; i++) {
            double dwell = 0;
            for (int j = 0; j < kNumRates; j++) if (j != i) dwell = std::max(dwell, 5 * kKey / std::fabs(1 - kRates[j] / kRates[i]));
            minSpan[i] = std::max(0.1, 2.6 * dwell);   // counted in detector frames with hits (two echoes of a frame can fall in two cells)
            minHits[i] = std::max(12, (int)(0.5 * minSpan[i] / frameDur));
        }
        covered.assign(kCols, 0);
        g = 0; frames = 0; hits = 0;
        for (auto& a : acc) a.clear();   // the sounders, their history and the timeline stay (clear())
        sweeps.clear();
    }

    void clear() {
        for (auto& a : acc) a.clear();
        sweeps.clear(); sounders.clear(); timeline.clear();
        covered.assign(kCols, 0);
    }

    void markCoverage() {
        const double lo = std::max(0.0, fc - kEdge * fs), hi = std::min(kBandHz, fc + kEdge * fs);
        for (int c = 0; c < kCols; c++) if ((c + 0.5) * kColHz >= lo && (c + 0.5) * kColHz <= hi) covered[(size_t)c] = 1;
    }

    // ---- sounders

    void sounderSweep(int si, double t0abs, double snrDb) {
        IonoSounder& s = sounders[(size_t)si].info;
        const double d = t0abs - s.lastT0;
        if (s.sweeps == 0 || std::fabs(d) >= 0.5) {
            if (s.sweeps > 0 && s.periodSec <= 0 && d >= 2) s.periodSec = locked ? std::round(d) : d;
            s.sweeps++;
            s.lastT0 = t0abs;
        }
        if (s.periodSec > 0) s.offsetSec = std::fmod(std::fmod(t0abs, s.periodSec) + s.periodSec, s.periodSec);
        s.snrDb = snrDb;
        s.lastSeen = tAbs(g / fs);
    }

    int matchSounder(int ri, double t0abs) {
        for (size_t k = 0; k < sounders.size(); k++) {
            const auto& s = sounders[k];
            if (s.rateIdx != ri) continue;
            const double d = t0abs - s.info.lastT0;
            if (std::fabs(d) < 0.5) return (int)k;
            if (s.info.periodSec > 0) { const double m = std::round(d / s.info.periodSec); if (m >= 1 && std::fabs(d - m * s.info.periodSec) < 0.5) return (int)k; }
        }
        for (size_t k = 0; k < sounders.size(); k++)
            if (sounders[k].rateIdx == ri && sounders[k].info.periodSec <= 0 && t0abs - sounders[k].info.lastT0 >= 2) return (int)k;
        SounderAcc s;
        s.rateIdx = ri;
        s.info.id = sounders.empty() ? 1 : sounders.back().info.id + 1;
        s.info.rateHz = kRates[ri];
        s.info.fLoHz = 1e300; s.info.fHiHz = -1e300;
        s.ion.assign((size_t)kRows * kCols, 0.0);
        s.cnt.assign(kCols, 0);
        s.recent.assign((size_t)kRows * kCols, 0.0);
        s.recentHave.assign(kCols, 0);
        sounders.push_back(std::move(s));
        return (int)sounders.size() - 1;
    }

    // ---- sweeps

    void openSweep(int ri, double t0, int si, bool predicted, int hits0, double snr0) {
        Sweep s;
        s.rateIdx = ri; s.r = kRates[ri]; s.t0 = t0; s.sounder = si; s.predicted = predicted; s.confirmed = !predicted;
        s.hits = hits0; s.snrSum = snr0;
        const double fEnt = std::max(0.0, fc - kEdge * fs), fExit = std::min(kBandHz, fc + kEdge * fs);
        if (fExit <= fEnt) return;
        s.gEntry = (uint64_t)std::max((double)g, std::ceil((t0 + fEnt / s.r) * fs));
        s.gExit = (uint64_t)std::max(0.0, std::floor((t0 + fExit / s.r) * fs));
        s.D = std::max(1, (int)std::lround(fs / (kM * s.r * kDelayBin)));
        s.fd = fs / s.D;
        s.dw = std::polar(1.0, -2 * M_PI * s.r / (fs * fs));
        s.blk.reserve(kM);
        sweeps.push_back(std::move(s));
    }

    void addColumn(SounderAcc& so, int col, const double* rows, double n) {
        for (int j = 0; j < kRows; j++) so.ion[(size_t)j * kCols + (size_t)col] += rows[j];
        so.cnt[(size_t)col] += (uint32_t)n;
    }

    void block(Sweep& s) {
        const double T = (double)kM * s.D / fs;
        const double tmid = s.blockG / fs + T / 2;
        const double fmid = s.r * (tmid - s.t0);
        const int col = (int)std::floor(fmid / kColHz);
        if (col < 0 || col >= kCols) return;
        for (int i = 0; i < kM; i++) tmpM[(size_t)i] = s.blk[(size_t)i] * winM[(size_t)i];
        fftM->forward(tmpM.data());
        double rows[kRows];
        for (int j = 0; j < kRows; j++) {
            const double f = -s.r * rowDelay(j);
            const long kk = std::lround(f * kM / s.fd);
            rows[j] = std::norm(tmpM[(size_t)(((kk % kM) + kM) % kM)]);
        }
        s.fLo = std::min(s.fLo, fmid); s.fHi = std::max(s.fHi, fmid);
        {
            auto& v = s.own[col];
            if (v.empty()) v.assign(kRows + 1, 0.0);
            for (int j = 0; j < kRows; j++) v[(size_t)j] += rows[j];
            v[kRows] += 1;
        }
        if (s.confirmed && s.sounder >= 0) addColumn(sounders[(size_t)s.sounder], col, rows, 1);
        else {
            auto& v = s.temp[col];
            if (v.empty()) v.assign(kRows + 1, 0.0);
            for (int j = 0; j < kRows; j++) v[(size_t)j] += rows[j];
            v[kRows] += 1;
        }
    }

    void confirm(Sweep& s) {
        s.confirmed = true;
        if (s.sounder < 0) return;
        for (auto& kv : s.temp) addColumn(sounders[(size_t)s.sounder], kv.first, kv.second.data(), kv.second[kRows]);
        s.temp.clear();
        sounderSweep(s.sounder, tAbs(s.t0), 10 * std::log10(std::max(1e-9, s.snrSum / std::max(1, s.hits))));
    }

    void runSweep(Sweep& s, const cf32* x, size_t m, uint64_t g0) {
        size_t k = 0;
        if (g0 + m <= s.gEntry) return;
        if (g0 < s.gEntry && s.blk.empty() && s.dc == 0) k = (size_t)(s.gEntry - g0);
        while (k < m) {
            const uint64_t gi = g0 + k;
            if (s.dc == 0) {
                if (s.blk.empty()) {
                    if (gi + (uint64_t)kM * s.D > s.gExit) { s.done = true; return; }
                    s.blockG = gi;
                }
                const double fb = s.r * (gi / fs - s.t0) - fc;
                s.w = std::polar(1.0, -2 * M_PI * fb / fs);
                s.z /= std::abs(s.z);
            }
            const size_t len = std::min(m - k, (size_t)(s.D - s.dc));   // up to the end of this decimated sample
            iono::dechirpRun(x + k, len, s.z, s.w, s.dw, std::arg(s.dw), s.acc);
            k += len; s.dc += (int)len;
            if (s.dc == s.D) {
                s.blk.push_back(cf32((float)(s.acc.real() / s.D), (float)(s.acc.imag() / s.D)));
                s.acc = 0; s.dc = 0;
                if ((int)s.blk.size() == kM) { block(s); s.blk.clear(); }
            }
        }
    }

    void endSweep(Sweep& s) {
        const int need = std::max(6, minHits[s.rateIdx] / 3);
        if (!s.confirmed && s.hits >= need) confirm(s);
        if (s.sounder < 0) return;
        IonoSounder& so = sounders[(size_t)s.sounder].info;
        if (!s.confirmed) { so.missed++; return; }
        IonoEvent e;
        e.t0 = tAbs(s.t0); e.sounder = so.id; e.rateHz = s.r; e.predicted = s.predicted;
        e.fLoHz = s.fHi >= s.fLo ? s.fLo : 0; e.fHiHz = s.fHi >= s.fLo ? s.fHi : 0;
        e.snrDb = 10 * std::log10(std::max(1e-9, s.snrSum / std::max(1, s.hits)));
        if (s.fHi >= s.fLo) { so.fLoHz = std::min(so.fLoHz, s.fLo); so.fHiHz = std::max(so.fHiHz, s.fHi); }
        so.snrDb = e.snrDb;
        timeline.push_back(e);
        if (timeline.size() > 300) timeline.erase(timeline.begin());
        scaleSweep(sounders[(size_t)s.sounder], s);
    }

    // the newest columns of the sounder's ionogram, in dB over each column's median (as fill hands the accumulated one out)
    static void columnsDb(const std::vector<double>& pw, const std::vector<uint8_t>& have, std::vector<float>& db) {
        db.assign((size_t)kRows * kCols, NAN);
        std::vector<double> v(kRows), srt;
        for (int c = 0; c < kCols; c++) {
            if (!have[(size_t)c]) continue;
            for (int j = 0; j < kRows; j++) v[(size_t)j] = pw[(size_t)j * kCols + (size_t)c];
            srt = v;
            std::nth_element(srt.begin(), srt.begin() + kRows / 2, srt.end());
            const double med = std::max(1e-30, srt[kRows / 2]);
            for (int j = 0; j < kRows; j++) db[(size_t)j * kCols + (size_t)c] = (float)(10 * std::log10(std::max(1e-30, v[(size_t)j]) / med));
        }
    }

    void scaleSweep(SounderAcc& so, const Sweep& s) {
        if (s.own.empty()) return;
        for (const auto& kv : s.own) {
            const int c = kv.first;
            if (c < 0 || c >= kCols || kv.second[kRows] <= 0) continue;
            for (int j = 0; j < kRows; j++) so.recent[(size_t)j * kCols + (size_t)c] = kv.second[(size_t)j] / kv.second[kRows];
            so.recentHave[(size_t)c] = 1;
        }
        std::vector<float> db;
        columnsDb(so.recent, so.recentHave, db);
        ScaleOptions o;
        o.locked = locked; o.obs = obs;
        std::vector<IonoTrace> tr;
        IonoScaling sc = scaleIonogram(db, so.info, o, &tr);
        sc.t = tAbs(s.t0);
        if (sc.geometry == 0) return;   // no echo traces: the last values stay
        so.info.scale = sc;
        so.traces = std::move(tr);
        IonoHistPt h;
        h.t = sc.t; h.geometry = sc.geometry;
        h.foE = (float)sc.foE; h.foF1 = (float)sc.foF1; h.foF2 = sc.foF2Above ? NAN : (float)sc.foF2; h.hE = (float)sc.hE; h.hF = (float)sc.hF;
        h.m3000 = (float)sc.m3000; h.muf3000 = (float)sc.muf3000; h.pathMuf = sc.pathMufAbove ? NAN : (float)sc.pathMuf; h.distKm = (float)sc.distKm;
        if (sc.foEAbove) h.foE = NAN;
        so.history.push_back(h);
        if (so.history.size() > 2000) so.history.erase(so.history.begin());
    }

    // ---- detector

    void promote(int ri, double tz, int count, double snrSum) {
        double t0abs = tAbs(tz);
        if (locked) { const double rr = std::round(t0abs); t0abs = std::fabs(t0abs - rr) < 0.05 ? rr : t0abs - 0.002; }
        else t0abs -= 0.002;
        const double t0 = t0abs - origin;
        for (const auto& s : sweeps) if (s.rateIdx == ri && std::fabs(s.t0 - t0) < 0.5) return;
        const int si = matchSounder(ri, t0abs);
        sounderSweep(si, t0abs, 10 * std::log10(std::max(1e-9, snrSum / std::max(1, count))));
        openSweep(ri, t0, si, false, count, snrSum);
    }

    void evaluate(double now) {
        for (int i = 0; i < kNumRates; i++) {
            auto& m = acc[i];
            for (auto it = m.begin(); it != m.end();) { if (it->second.last < now - 1.5) it = m.erase(it); else ++it; }
            int64_t bestKey = 0; Acc best; bool found = false;
            for (const auto& kv : m) {
                if (kv.second.last < now - 0.1) continue;
                Acc c;
                for (int64_t d = -2; d <= 2; d++) {
                    auto f = m.find(kv.first + d);
                    if (f == m.end()) continue;
                    const Acc& a = f->second;
                    c.count += a.count; c.frames += a.frames; c.w += a.w; c.wtz += a.wtz; c.snr += a.snr;
                }
                if (c.count >= minHits[i] && c.frames * frameDur >= minSpan[i] && (!found || c.count > best.count)) { found = true; best = c; bestKey = kv.first; }
            }
            if (found) {
                for (int64_t d = -3; d <= 3; d++) m.erase(bestKey + d);
                promote(i, best.wtz / best.w, best.count, best.snr);
            }
        }
        // the next sweep of each sounder whose period is known: dechirped from the moment it enters the span
        const double fEnt = std::max(0.0, fc - kEdge * fs), fExit = std::min(kBandHz, fc + kEdge * fs);
        for (size_t k = 0; k < sounders.size(); k++) {
            SounderAcc& so = sounders[k];
            if (so.info.periodSec <= 0) { so.info.nextT0 = 0; continue; }
            const double r = kRates[so.rateIdx], P = so.info.periodSec;
            double next = so.info.lastT0 + P;
            while (fExit > fEnt && next - origin + fExit / r < now) next += P;
            so.info.nextT0 = next;
            if (fExit <= fEnt || so.armedFor == next) continue;
            const double t0 = next - origin;
            if (now >= t0 + fEnt / r - 0.2 && now < t0 + fExit / r) {
                bool open = false;
                for (const auto& s : sweeps) if (s.rateIdx == so.rateIdx && std::fabs(s.t0 - t0) < 0.5) open = true;
                so.armedFor = next;
                if (!open) openSweep(so.rateIdx, t0, (int)k, true, 0, 0);
            }
        }
    }

    void frame(uint64_t gStart) {
        const double tmid = (gStart + N / 2.0) / fs;
        for (int i = 0; i < N; i++) tmp[(size_t)i] = buf[(size_t)i] * win[(size_t)i];
        fft->forward(tmp.data());
        for (int j = 0; j < N; j++) pw[(size_t)j] = std::norm(tmp[(size_t)((j + N / 2) % N)]);   // bin j = baseband (j - N/2) fs / N
        const int half = (int)(kEdge * N), lo = N / 2 - half, hi = N / 2 + half;
        scratch.clear();
        for (int j = lo; j < hi; j += 4) scratch.push_back(pw[(size_t)j]);
        std::nth_element(scratch.begin(), scratch.begin() + scratch.size() / 2, scratch.end());
        const double mean = std::max(1e-30, (double)scratch[scratch.size() / 2] / std::log(2.0));
        const double thr = mean * std::pow(10.0, kThrDb / 10);
        // the strongest local maxima over the threshold (not the radio's centre spike)
        std::pair<float, int> pk[kPeaks];
        int np = 0;
        for (int j = std::max(1, lo); j < std::min(N - 1, hi); j++) {
            const float v = pw[(size_t)j];
            if (v < thr || v < pw[(size_t)j - 1] || v < pw[(size_t)j + 1] || std::abs(j - N / 2) <= 2) continue;
            if (np < kPeaks) { pk[np++] = {v, j}; continue; }
            int mi = 0; for (int q = 1; q < kPeaks; q++) if (pk[q].first < pk[mi].first) mi = q;
            if (v > pk[mi].first) pk[mi] = {v, j};
        }
        for (int q = 0; q < np; q++) {
            const int j = pk[q].second;
            const double a = pw[(size_t)j - 1], b = pw[(size_t)j], c = pw[(size_t)j + 1];
            const double den = a - 2 * b + c;
            const double dl = den != 0 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
            const double f = fc + (j + dl - N / 2) * fs / N;
            const double snr = b / mean;
            bool taken = false;
            for (auto& s : sweeps) {
                const double tz = tmid - f / s.r;
                if (tz >= s.t0 + kWinLo && tz <= s.t0 + kWinHi) {
                    s.hits++; s.snrSum += snr; taken = true;
                    if (s.sounder >= 0) sounders[(size_t)s.sounder].info.lastSeen = tAbs(tmid);
                    if (!s.confirmed && s.hits >= std::max(6, minHits[s.rateIdx] / 3)) confirm(s);
                    break;
                }
            }
            if (taken || !(f >= 0 && f <= kBandHz)) continue;   // (written so a NaN frequency is skipped too: it reached the int64 cast below, UBSan)
            hits++;
            for (int i = 0; i < kNumRates; i++) {
                const double tz = tmid - f / kRates[i];
                Acc& ac = acc[i][(int64_t)std::floor(tz / kKey)];
                ac.count++; ac.w += b; ac.wtz += b * tz; ac.snr += snr;
                ac.last = tmid;
                if (ac.lastFrame != frames) { ac.lastFrame = frames; ac.frames++; }
            }
        }
        frames++;
        if (frames % 16 == 0) { evaluate(tmid); markCoverage(); }
    }

    void feed(const cf32* x, size_t n) {
        if (fs <= 0) return;
        if (retuned) {   // the model chirps and the start-time cells were for the old frequency
            retuned = false;
            for (auto& s : sweeps) endSweep(s);
            sweeps.clear();
            for (auto& a : acc) a.clear();
        }
        size_t off = 0;
        while (off < n) {
            const size_t m = std::min(n - off, (size_t)N - buf.size());
            const uint64_t g0 = g;
            for (auto& s : sweeps) runSweep(s, x + off, m, g0);
            for (size_t k = 0; k < m; k++) {
                cf32 v = x[off + k];
                if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
                pAcc += std::norm(v); pN++;
                buf.push_back(v);
            }
            g += m; off += m;
            if ((int)buf.size() == N) {
                frame(g - (uint64_t)N);
                buf.clear();
                if (pN >= (uint64_t)fs / 4) { powerDb = 10 * std::log10(std::max(1e-20, pAcc / pN)); pAcc = 0; pN = 0; }
            }
            for (size_t k = 0; k < sweeps.size();) {
                if (sweeps[k].done) { endSweep(sweeps[k]); sweeps.erase(sweeps.begin() + (long)k); } else k++;
            }
        }
    }
};

Processor::Processor() : p_(new Impl) {}
Processor::~Processor() = default;

void Processor::configure(double fs, double centerHz, double originSec) {
    p_->fs = fs; p_->fc = centerHz; p_->origin = originSec;
    if (fs > 0) p_->setup();
}
void Processor::setCenterHz(double hz) { if (hz != p_->fc) { p_->fc = hz; p_->retuned = true; } }
void Processor::setLocked(bool on) { p_->locked = on; }
void Processor::setSelected(int i) { p_->selReq = i; }
void Processor::setObserver(bool have, double lat, double lon) { p_->obs.have = have; p_->obs.lat = lat; p_->obs.lon = lon; }
void Processor::clear() { p_->clear(); }
std::string Processor::saveResults() const {
    std::vector<std::pair<IonoSounder, std::vector<IonoHistPt>>> so;
    for (const auto& a : p_->sounders) so.emplace_back(a.info, a.history);
    return results::pack(so, p_->timeline);
}
bool Processor::loadResults(const std::string& blob) {
    std::vector<std::pair<IonoSounder, std::vector<IonoHistPt>>> so;
    std::vector<IonoEvent> tl;
    if (!results::unpack(blob, so, tl)) return false;
    p_->sounders.clear();
    for (auto& [info, hist] : so) {
        SounderAcc s;
        int ri = 0;
        for (int i = 1; i < kNumRates; i++) if (std::fabs(kRates[i] - info.rateHz) < std::fabs(kRates[ri] - info.rateHz)) ri = i;
        s.rateIdx = ri;
        s.info = info;
        s.ion.assign((size_t)kRows * kCols, 0.0);
        s.cnt.assign(kCols, 0);
        s.recent.assign((size_t)kRows * kCols, 0.0);
        s.recentHave.assign(kCols, 0);
        s.history = std::move(hist);
        p_->sounders.push_back(std::move(s));
    }
    p_->timeline = std::move(tl);
    return true;
}
void Processor::feed(const cf32* x, size_t n) { p_->feed(x, n); }
double Processor::now() const { return p_->fs > 0 ? p_->origin + p_->g / p_->fs : 0; }
int Processor::detectorFft() const { return p_->N; }

void Processor::fill(IonoTelemetry& t) const {
    const Impl& m = *p_;
    t.state = m.fs > 0 ? 1 : 0;
    t.rate = m.fs; t.centerHz = m.fc; t.nowSec = now(); t.locked = m.locked;
    t.spanLoHz = std::max(0.0, m.fc - kEdge * m.fs); t.spanHiHz = std::min(kBandHz, m.fc + kEdge * m.fs);
    t.covered = m.covered;
    t.sounders.clear();
    for (size_t k = 0; k < m.sounders.size(); k++) {
        IonoSounder i = m.sounders[k].info;
        if (i.fHiHz < i.fLoHz) i.fLoHz = i.fHiHz = 0;
        i.active = false;
        for (const auto& w : m.sweeps) if (w.sounder == (int)k && w.confirmed) i.active = true;
        t.sounders.push_back(i);
    }
    t.timeline = m.timeline;
    int sel = m.selReq;
    if (sel < 0 || sel >= (int)m.sounders.size()) {
        sel = -1;
        double best = -1e300;
        for (size_t k = 0; k < m.sounders.size(); k++) if (m.sounders[k].info.lastSeen > best) { best = m.sounders[k].info.lastSeen; sel = (int)k; }
    }
    t.sel = sel;
    t.ionoDb.clear(); t.swept.clear(); t.traces.clear(); t.history.clear();
    t.haveObserver = m.obs.have; t.obsLat = m.obs.lat; t.obsLon = m.obs.lon;
    if (sel >= 0) {
        const SounderAcc& so = m.sounders[(size_t)sel];
        t.traces = so.traces;
        t.history = so.history;
        t.ionoDb.assign((size_t)kRows * kCols, NAN);
        t.swept.assign(kCols, 0);
        std::vector<double> v(kRows);
        for (int c = 0; c < kCols; c++) {
            const uint32_t n = so.cnt[(size_t)c];
            if (!n) continue;
            t.swept[(size_t)c] = 1;
            for (int j = 0; j < kRows; j++) v[(size_t)j] = so.ion[(size_t)j * kCols + (size_t)c] / n;
            std::vector<double> s(v);
            std::nth_element(s.begin(), s.begin() + kRows / 2, s.end());
            const double med = std::max(1e-30, s[kRows / 2]);
            for (int j = 0; j < kRows; j++) t.ionoDb[(size_t)j * kCols + (size_t)c] = (float)(10 * std::log10(std::max(1e-30, v[(size_t)j]) / med));
        }
    }
    t.activeSweeps = 0;
    for (const auto& w : m.sweeps) if (w.confirmed) t.activeSweeps++;
    t.frames = m.frames; t.hits = m.hits; t.fftSize = m.N; t.powerDb = m.powerDb;
}

std::string ionogramCsv(const IonoTelemetry& t) {
    std::string out = "frequency_mhz,delay_ms,group_range_km,db\n";
    if (t.ionoDb.size() != (size_t)kRows * kCols) return out;
    char b[96];
    for (int c = 0; c < kCols; c++) {
        if (t.swept.size() == (size_t)kCols && !t.swept[(size_t)c]) continue;
        for (int j = 0; j < kRows; j++) {
            const float v = t.ionoDb[(size_t)j * kCols + (size_t)c];
            if (std::isnan(v)) continue;
            snprintf(b, sizeof b, "%.3f,%.3f,%.1f,%.2f\n", (c + 0.5) * kColHz / 1e6, rowDelay(j) * 1e3, groupRangeKm(rowDelay(j)), v);
            out += b;
        }
    }
    return out;
}

} // namespace iono
} // namespace dect2

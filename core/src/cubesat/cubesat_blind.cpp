// Blind analysis of the Smart CubeSat decoder: the narrow-carrier scan of a wide capture and the modulation / baud classifier of one carrier.
// Nothing here knows a satellite: the scan finds carriers (centre, width, SNR), the classifier looks at 1.5 s of one carrier at 96 kHz and says
// which of CW / AFSK / BPSK / FSK it is and at which baud rate, as a ranked list the demodulation chains are then tried in.
#include "cubesat_blind.h"
#include <cstdio>
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace cubesat {

namespace {

using dect2::math::kPi;

size_t pow2ceil(size_t v) { size_t p = 1; while (p < v) p <<= 1; return p; }

double medianOf(std::vector<float> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end());
    return v[v.size() / 2];
}

// power spectrum (|X|^2 of a Hann-windowed block, zero padded or cut to n)
std::vector<float> powerSpectrum(const std::vector<float>& x, size_t n, size_t from = 0) {
    Fft fft((int)n);
    std::vector<cf32> v(n, cf32(0.f, 0.f));
    double mean = 0;
    const size_t m = std::min(n, x.size() - std::min(x.size(), from));
    for (size_t i = 0; i < m; i++) mean += x[from + i];
    if (m) mean /= (double)m;
    for (size_t i = 0; i < m; i++) v[i] = cf32((float)((x[from + i] - mean) * (0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)m))), 0.f);
    fft.forward(v.data());
    std::vector<float> p(n / 2);
    for (size_t k = 0; k < n / 2; k++) p[k] = std::norm(v[k]);
    return p;
}

// the line of a cyclic feature at `b` Hz in a power spectrum with bins of binHz: peak within +-3 % over the median of the neighbourhood (dB)
double lineDb(const std::vector<float>& p, double binHz, double b, double* atHz = nullptr) {
    const size_t lo = (size_t)std::max(1.0, std::floor(0.97 * b / binHz)), hi = (size_t)std::min((double)p.size() - 2, std::ceil(1.03 * b / binHz));
    if (hi <= lo + 1) return -100;
    size_t bk = lo;
    for (size_t k = lo; k <= hi; k++) if (p[k] > p[bk]) bk = k;
    std::vector<float> ref;
    const size_t r0 = (size_t)std::max(1.0, 0.55 * b / binHz), r1 = (size_t)std::min((double)p.size() - 1, 1.5 * b / binHz);
    for (size_t k = r0; k < r1; k++) if (k < lo || k > hi) ref.push_back(p[k]);
    const double med = medianOf(ref) + 1e-30;
    if (atHz) {
        const double a = std::log(p[bk - 1] + 1e-30), c0 = std::log(p[bk] + 1e-30), cc = std::log(p[bk + 1] + 1e-30);
        const double den = a - 2 * c0 + cc;
        const double off = den < 0 ? std::max(-0.5, std::min(0.5, 0.5 * (a - cc) / den)) : 0.0;
        *atHz = ((double)bk + off) * binHz;
    }
    return 10 * std::log10((p[bk] + 1e-30) / med);
}

const int kBauds[] = {300, 600, 1200, 2400, 4800, 9600, 14400, 19200};

} // namespace

// ================================================================= CarrierScan
void CarrierScan::configure(double rate, double binHz) {
    rate_ = rate;
    const double want = binHz > 0 ? binHz : 250.0;
    n_ = std::max<size_t>(1024, std::min<size_t>(131072, pow2ceil((size_t)std::ceil(rate / want))));
    fft_ = std::make_shared<Fft>((int)n_);
    win_.resize(n_);
    for (size_t i = 0; i < n_; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)n_));
    acc_.assign(n_, 0.0);
    buf_.clear(); fill_ = 0; nAvg_ = 0;
    // one estimate per 0.25 s of signal, at most 16 blocks of it (a wide capture is sampled, not summed in full)
    target_ = (int)std::max(1.0, std::min(16.0, std::floor(0.25 * rate / (double)n_)));
    hop_ = (size_t)std::max<double>((double)n_, 0.25 * rate / target_);
    skip_ = 0;
    cands_.clear();
    nextId_ = 1;
    noiseDb_ = -200;
    updates_ = 0;
}

void CarrierScan::reset() { configure(rate_, rate_ / (double)n_); }

void CarrierScan::feed(const cf32* x, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (skip_ > 0) { const size_t s = std::min(skip_, n - i); skip_ -= s; i += s; continue; }
        const size_t take = std::min(n_ - buf_.size(), n - i);
        buf_.insert(buf_.end(), x + i, x + i + take);
        i += take;
        if (buf_.size() < n_) continue;
        std::vector<cf32> v(n_);
        for (size_t k = 0; k < n_; k++) v[k] = buf_[k] * win_[k];
        fft_->forward(v.data());
        for (size_t k = 0; k < n_; k++) acc_[k] += std::norm(v[k]);
        nAvg_++;
        buf_.clear();
        skip_ = hop_ - n_;
        if (nAvg_ >= target_) { update(); nAvg_ = 0; std::fill(acc_.begin(), acc_.end(), 0.0); }
    }
}

void CarrierScan::update() {
    const size_t n = n_;
    const double binHz = rate_ / (double)n;
    // DC-centred power in dB (normalised: a full-scale tone per bin = 0 dB at bin power n^2 / 4 .. only differences matter)
    std::vector<float> db(n), lin(n);
    for (size_t k = 0; k < n; k++) {
        const size_t j = (k + n / 2) % n;                    // bin k of the centred spectrum is FFT bin j
        lin[k] = (float)(acc_[j] / (double)nAvg_);
        db[k] = 10.f * std::log10(lin[k] + 1e-20f);
    }
    // noise floor: the 30th percentile of every segment, the lowest of the segment and its neighbours, linear between segment centres
    const size_t nseg = std::max<size_t>(8, std::min<size_t>(64, n / 256));
    const size_t sl = n / nseg;
    std::vector<float> p30(nseg);
    for (size_t s = 0; s < nseg; s++) {
        std::vector<float> seg(db.begin() + (long)(s * sl), db.begin() + (long)((s + 1) * sl));
        std::nth_element(seg.begin(), seg.begin() + (long)(seg.size() * 3 / 10), seg.end());
        p30[s] = seg[seg.size() * 3 / 10];
    }
    std::vector<float> fl(nseg);
    for (size_t s = 0; s < nseg; s++) fl[s] = std::min(p30[s], std::min(p30[s > 0 ? s - 1 : s], p30[s + 1 < nseg ? s + 1 : s]));
    std::vector<float> floorDb(n);
    for (size_t k = 0; k < n; k++) {
        const double pos = ((double)k + 0.5) / (double)sl - 0.5;
        const long a = std::max(0L, std::min((long)nseg - 1, (long)std::floor(pos))), b = std::min((long)nseg - 1, a + 1);
        const double f = std::max(0.0, std::min(1.0, pos - (double)a));
        floorDb[k] = (float)(fl[(size_t)a] * (1 - f) + fl[(size_t)b] * f);
    }
    {
        std::vector<float> m(fl);
        noiseDb_ = medianOf(m);
    }
    // groups of bins above the floor
    const double binThr = std::max(3.5, minSnrDb * 0.55);
    std::vector<ScanPeak> found;
    size_t k = 2;
    const size_t kLo = 2, kHi = n - 2;
    const size_t dcBins = (size_t)std::ceil(1500.0 / binHz);
    while (k < kHi) {
        if (db[k] - floorDb[k] < binThr) { k++; continue; }
        size_t first = k, last = k, gap = 0;
        size_t j = k;
        while (j + 1 < kHi && gap <= 2) {
            j++;
            if (db[j] - floorDb[j] >= binThr * 0.8) { last = j; gap = 0; } else gap++;
        }
        k = last + 1;
        const size_t nb = last - first + 1;
        double sum = 0, fsum = 0, pk = -1e9, wsum = 0;
        size_t pkk = first;
        for (size_t q = first; q <= last; q++) {
            const double fl2 = std::pow(10.0, floorDb[q] / 10.0);
            const double pw = std::max(0.0, (double)lin[q] - fl2);
            sum += pw; wsum += pw; fsum += pw * (double)q;
            if (db[q] > pk) { pk = db[q]; pkk = q; }
        }
        if (first < kLo || last >= kHi) continue;
        const double centreBin = wsum > 0 ? fsum / wsum : (double)pkk;
        const double fl0 = std::pow(10.0, floorDb[pkk] / 10.0);
        const double snr = 10 * std::log10(std::max(1e-9, sum / ((double)nb * fl0)));
        const bool nearDc = (double)first <= (double)n / 2 + (double)dcBins && (double)last + 1 >= (double)n / 2 - (double)dcBins && nb <= 2 * dcBins;
        if (nb < 3 && pk - floorDb[pkk] < 9.0) continue;
        if (nearDc) continue;                                  // the radio's own spike
        ScanPeak sp;
        sp.hz = (centreBin - (double)n / 2.0) * binHz;
        sp.widthHz = (double)nb * binHz;
        sp.snrDb = snr;
        sp.peakDb = pk - noiseDb_;
        // flatness inside the group: a wide, flat group is a LoRa-like chirp signal
        double mn = 1e9, mx = -1e9;
        for (size_t q = first; q <= last; q++) { mn = std::min(mn, (double)db[q]); mx = std::max(mx, (double)db[q]); }
        sp.flatDb = mx - mn;
        found.push_back(sp);
    }
    // groups that touch each other's skirts are one carrier (a wide FSK spectrum may dip in the middle)
    std::sort(found.begin(), found.end(), [](const ScanPeak& a, const ScanPeak& b) { return a.hz < b.hz; });
    std::vector<ScanPeak> merged;
    for (const ScanPeak& s : found) {
        if (!merged.empty()) {
            ScanPeak& m = merged.back();
            const double mEdge = m.hz + m.widthHz / 2, sEdge = s.hz - s.widthHz / 2;
            if (sEdge - mEdge < std::max(1500.0, 0.25 * (m.widthHz + s.widthHz))) {
                const double lo = std::min(m.hz - m.widthHz / 2, s.hz - s.widthHz / 2), hi = std::max(mEdge, s.hz + s.widthHz / 2);
                const double wm = std::pow(10.0, m.snrDb / 10.0) * m.widthHz, ws = std::pow(10.0, s.snrDb / 10.0) * s.widthHz;
                m.hz = (m.hz * wm + s.hz * ws) / (wm + ws);
                m.widthHz = hi - lo;
                m.snrDb = 10 * std::log10((wm + ws) / m.widthHz);
                m.peakDb = std::max(m.peakDb, s.peakDb);
                m.flatDb = std::max(m.flatDb, s.flatDb);
                continue;
            }
        }
        merged.push_back(s);
    }
    // persistence: a carrier is reported once it has been seen in a few estimates in a row
    for (Cand& c : cands_) c.seenNow = false;
    for (const ScanPeak& s : merged) {
        if (s.snrDb < minSnrDb - 3.0) continue;               // the lower bound; the owner decides per track (priors are accepted lower)
        Cand* best = nullptr;
        double bd = 1e18;
        for (Cand& c : cands_) {
            const double d = std::fabs(c.p.hz - s.hz);
            if (!c.seenNow && d < std::max(1500.0, 0.6 * std::max(c.p.widthHz, s.widthHz)) && d < bd) { bd = d; best = &c; }
        }
        if (best) {
            best->p.hz += 0.5 * (s.hz - best->p.hz);
            best->p.widthHz += 0.3 * (s.widthHz - best->p.widthHz);
            best->p.snrDb += 0.3 * (s.snrDb - best->p.snrDb);
            best->p.peakDb = s.peakDb; best->p.flatDb = s.flatDb;
            best->hits++; best->misses = 0; best->seenNow = true;
        } else {
            Cand c; c.id = nextId_++; c.p = s; c.hits = 1; c.seenNow = true;
            cands_.push_back(c);
        }
    }
    for (Cand& c : cands_) if (!c.seenNow) c.misses++;
    cands_.erase(std::remove_if(cands_.begin(), cands_.end(), [](const Cand& c) { return c.misses > 24 || (c.hits < 3 && c.misses > 3); }), cands_.end());
    updates_++;
}

std::vector<ScanCarrier> CarrierScan::carriers() const {
    std::vector<ScanCarrier> v;
    for (const Cand& c : cands_) {
        ScanCarrier s; s.id = c.id; s.hz = c.p.hz; s.widthHz = c.p.widthHz; s.snrDb = c.p.snrDb; s.peakDb = c.p.peakDb; s.flatDb = c.p.flatDb;
        s.hits = c.hits; s.active = c.seenNow; s.ageSec = c.hits * 0.25;
        v.push_back(s);
    }
    return v;
}

// ================================================================= classifier
ProbeResult classifyCarrier(const std::vector<cf32>& y, int forceMod, int forceBaud) {
    ProbeResult r;
    const size_t n = y.size();
    if (n < 24000) { r.note = "too short"; return r; }
    // ---- spectrum, width, tonality
    const size_t N = 2048;
    std::vector<double> psd(N, 0.0);
    {
        Fft fft((int)N);
        std::vector<cf32> v(N);
        int blocks = 0;
        for (size_t s = 0; s + N <= n; s += N / 2) {
            for (size_t i = 0; i < N; i++) v[i] = y[s + i] * (float)(0.5 - 0.5 * std::cos(2 * kPi * (double)i / N));
            fft.forward(v.data());
            for (size_t i = 0; i < N; i++) psd[(i + N / 2) % N] += std::norm(v[i]);
            blocks++;
        }
        for (double& p : psd) p /= std::max(1, blocks);
    }
    const double binHz = kChanRate / (double)N;
    std::vector<float> pf(psd.begin(), psd.end());
    const double floorLin = medianOf(pf) * 1.0;
    // the outer eighth of the band holds the channel filter's skirt: the floor from the lowest 30 %
    std::vector<float> sorted(pf);
    std::sort(sorted.begin(), sorted.end());
    const double fl = sorted[sorted.size() * 3 / 10];
    (void)floorLin;
    size_t pk = 0;
    for (size_t i = 0; i < N; i++) if (psd[i] > psd[pk]) pk = i;
    double sig = 0;
    for (size_t i = 0; i < N; i++) sig += std::max(0.0, psd[i] - fl);
    // the narrowest span around the peak that holds 95 % of the signal power above the floor
    size_t lo = pk, hi = pk;
    double acc = std::max(0.0, psd[pk] - fl);
    while (acc < 0.95 * sig && (lo > 1 || hi + 2 < N)) {
        const double a = lo > 1 ? std::max(0.0, psd[lo - 1] - fl) : -1, b = hi + 2 < N ? std::max(0.0, psd[hi + 1] - fl) : -1;
        if (a >= b) { lo--; acc += std::max(0.0, a); } else { hi++; acc += std::max(0.0, b); }
    }
    r.bwHz = (double)(hi - lo + 1) * binHz;
    double tone = 0;
    for (size_t i = pk > 2 ? pk - 2 : 0; i <= std::min(N - 1, pk + 2); i++) tone += std::max(0.0, psd[i] - fl);
    r.tonality = sig > 0 ? tone / sig : 0;
    r.offsetHz = ((double)pk - (double)N / 2.0) * binHz;
    // ---- envelope statistics
    std::vector<float> env(n);
    double em = 0;
    for (size_t i = 0; i < n; i++) { env[i] = std::abs(y[i]); em += env[i]; }
    em /= (double)n;
    double ev = 0;
    for (size_t i = 0; i < n; i++) ev += (env[i] - em) * (env[i] - em);
    r.envCv = em > 0 ? std::sqrt(ev / (double)n) / em : 0;

    auto add = [&](int mod, int baud, double score) { r.hyps.push_back({mod, baud, score}); };
    if (forceMod != ModUnknown) {
        add(forceMod, forceBaud, 100);
        r.note = "forced";
        return r;
    }
    // ---- CW / unmodulated carrier
    if (r.tonality > 0.8 && r.bwHz < 600) {
        if (r.envCv > 0.45) { add(ModCw, 0, 30 * r.envCv); r.note = "keyed carrier"; }
        else { r.carrierOnly = true; r.note = "unmodulated carrier"; }
        return r;
    }
    // ---- FM discriminator, used by AFSK and FSK
    std::vector<float> disc(n, 0.f);
    {
        cf32 prev = y[0];
        const float k = (float)(kChanRate / (2 * kPi));
        for (size_t i = 1; i < n; i++) {
            const cf32 p = y[i] * std::conj(prev);
            prev = y[i];
            disc[i] = std::max(-20000.f, std::min(20000.f, std::atan2(p.imag(), p.real()) * k));
        }
    }
    // ---- AFSK 1200: tones at 1200 and 2200 Hz in the discriminator audio
    {
        const size_t M = 32768;
        const std::vector<float> ps = powerSpectrum(disc, M, 2000);
        const double bh = kChanRate / (double)M;
        auto bandMax = [&](double a, double b) { float m = 0; for (size_t k = (size_t)(a / bh); k <= (size_t)(b / bh) && k < ps.size(); k++) m = std::max(m, ps[k]); return (double)m; };
        std::vector<float> ref;
        for (size_t k = (size_t)(400 / bh); k < (size_t)(3200 / bh) && k < ps.size(); k++) ref.push_back(ps[k]);
        const double med = medianOf(ref) + 1e-30;
        const double m12 = 10 * std::log10(bandMax(1100, 1300) / med), m22 = 10 * std::log10(bandMax(2100, 2300) / med);
        r.afskDb = std::min(m12, m22);
    }
    // ---- BPSK: a line at twice the carrier in the square of the signal (and none for a constant-envelope signal)
    {
        std::vector<float> sq(n);
        // the square is complex: take the power spectrum of re and im together by running both through a complex FFT
        const size_t M = 32768;
        Fft fft((int)M);
        std::vector<cf32> v(M, cf32(0.f, 0.f));
        const size_t m = std::min(M, n);
        for (size_t i = 0; i < m; i++) v[i] = (y[i] * y[i]) * (float)(0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)m));
        fft.forward(v.data());
        std::vector<float> ps(M);
        for (size_t k = 0; k < M; k++) ps[k] = std::norm(v[k]);
        std::vector<float> s2(ps);
        const double med = medianOf(s2) + 1e-30;
        size_t bk = 0;
        for (size_t k = 0; k < M; k++) if (ps[k] > ps[bk]) bk = k;
        r.bpskDb = 10 * std::log10(ps[bk] / med);
        // MSK / GMSK: two lines R apart; the second strongest peak away from the first
        float b2 = 0; size_t k2 = 0;
        for (size_t k = 0; k < M; k++) {
            const size_t d = k > bk ? k - bk : bk - k;
            const size_t dd = std::min(d, M - d);
            if (dd < (size_t)(250.0 / (kChanRate / (double)M))) continue;
            if (ps[k] > b2) { b2 = ps[k]; k2 = k; }
        }
        r.mskDb = 10 * std::log10(b2 / med);
        const size_t dk = k2 > bk ? k2 - bk : bk - k2;
        r.mskSepHz = (double)std::min(dk, M - dk) * kChanRate / (double)M;
        r.bpskLineHz = ((bk >= M / 2 ? (double)bk - (double)M : (double)bk) * kChanRate / (double)M) / 2.0;
    }
    // ---- baud rates from cyclic lines
    auto rank = [&](const std::vector<float>& x, const char* what, std::vector<std::pair<int, double>>& out, double* est) {
        const size_t M = 131072;
        const std::vector<float> ps = powerSpectrum(x, M, 0);
        const double bh = kChanRate / (double)M;
        (void)what;
        double bestScore = -100, bestHz = 0;
        for (int b : kBauds) {
            double at = 0;
            const double s = lineDb(ps, bh, b, &at);
            out.push_back({b, s});
            if (s > bestScore) { bestScore = s; bestHz = at; }
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (est) *est = bestHz;
    };
    std::vector<std::pair<int, double>> fskR, bpskR;
    {
        std::vector<float> a(n);
        double mean = 0;
        for (size_t i = 0; i < n; i++) mean += disc[i];
        mean /= (double)n;
        for (size_t i = 0; i < n; i++) a[i] = std::fabs(disc[i] - (float)mean);
        // a short moving average takes the click noise out
        for (size_t i = n; i-- > 3;) a[i] = 0.25f * (a[i] + a[i - 1] + a[i - 2] + a[i - 3]);
        rank(a, "fsk", fskR, &r.estBaudFsk);
    }
    {
        std::vector<float> a(n);
        for (size_t i = 0; i < n; i++) a[i] = env[i] * env[i];
        for (size_t i = n; i-- > 3;) a[i] = 0.25f * (a[i] + a[i - 1] + a[i - 2] + a[i - 3]);
        rank(a, "bpsk", bpskR, &r.estBaudBpsk);
    }
    r.fskDb = fskR.empty() ? -100 : fskR[0].second;
    r.bpskCycDb = bpskR.empty() ? -100 : bpskR[0].second;
    // ---- the ranked hypotheses
    const bool wideFlat = false;
    (void)wideFlat;
    if (r.afskDb > 9.0 && r.bwHz < 16000) add(ModAfsk, 1200, 60 + r.afskDb);
    const bool bpsk = r.bpskDb > 14.0 && r.bpskDb > r.mskDb + 6.0;
    if (bpsk) {
        for (size_t i = 0; i < std::min<size_t>(2, bpskR.size()); i++) if (bpskR[i].second > 4.0) add(ModBpsk, bpskR[i].first, 50 + r.bpskDb * 0.5 + bpskR[i].second - (double)i * 8);
    }
    // MSK-like two-line signature: the separation of the lines is the baud rate
    int mskBaud = 0;
    if (r.mskDb > 10.0 && r.mskDb > r.bpskDb - 8.0 && r.mskSepHz > 600.0 && r.mskSepHz < 25000.0) {
        double bestd = 1e9;
        for (int b : kBauds) { const double d = std::fabs(std::log((double)b / r.mskSepHz)); if (d < bestd) { bestd = d; mskBaud = b; } }
        if (bestd > 0.08) mskBaud = 0;
    }
    if (mskBaud) add(ModFsk, mskBaud, 55 + r.mskDb * 0.4);
    for (size_t i = 0; i < std::min<size_t>(3, fskR.size()); i++) if (fskR[i].second > 3.5 && !(bpsk && r.bpskDb > 25.0)) {
        bool dup = false;
        for (const Hyp& h : r.hyps) if (h.mod == ModFsk && h.baud == fskR[i].first) dup = true;
        if (!dup) add(ModFsk, fskR[i].first, 20 + fskR[i].second - (double)i * 6);
    }
    std::sort(r.hyps.begin(), r.hyps.end(), [](const Hyp& a, const Hyp& b) { return a.score > b.score; });
    if (r.hyps.empty()) r.note = "no recognisable keying or symbol clock";
    else r.note = "ranked";
    return r;
}

std::string modulationText(int mod, int baud) {
    char b[96];
    switch (mod) {
    case ModCw: return "CW / OOK";
    case ModAfsk: return "AFSK 1200 baud";
    case ModFsk: snprintf(b, sizeof b, "FSK / GMSK %d baud", baud); return baud > 0 ? b : "FSK / GMSK";
    case ModBpsk: snprintf(b, sizeof b, "BPSK %d baud", baud); return baud > 0 ? b : "BPSK";
    case ModLora: return "LoRa";
    default: return "unknown";
    }
}

} // namespace cubesat
} // namespace dect2

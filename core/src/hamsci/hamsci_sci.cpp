// The science receivers: SuperSID (VLF station levels and sudden changes), the riometer (cosmic noise absorption against a sidereal quiet-day curve) and the
// pulsar folder (filter bank, incoherent dedispersion, folding, S/N against the folded noise).
// Reuses the conventions of core/src/astro (a power-in-band measurement, sidereal bookkeeping in the hydrogen line code) but is self-contained so it can be tested alone.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_int.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace dect2 {
namespace hamsci {
using namespace detail;

// ================================================================================================================================ SuperSID
namespace {
// Frequencies as commonly listed for SuperSID monitors. From memory and the public SuperSID station lists: check before relying on one.
const SidStation kSid[] = {
    {"NAA", 24.0, "Cutler, Maine, USA", 44.65, -67.28},
    {"NLK", 24.8, "Jim Creek, Washington, USA", 48.20, -121.92},
    {"NPM", 21.4, "Lualualei, Hawaii, USA", 21.42, -158.15},
    {"NWC", 19.8, "Exmouth, Australia", -21.82, 114.17},
    {"DHO38", 23.4, "Rhauderfehn, Germany", 53.08, 7.62},
    {"GQD", 22.1, "Anthorn, United Kingdom", 54.91, -3.28},
    {"GBZ", 19.58, "Anthorn, United Kingdom", 54.91, -3.28},
    {"HWU", 20.9, "Rosnay, France", 46.71, 1.24},
    {"ICV", 20.27, "Tavolara, Italy", 40.92, 9.73},
    {"JXN", 16.4, "Novik, Norway", 66.97, 13.87},
    {"TBB", 26.7, "Bafa, Turkey", 37.41, 27.32},
};
}  // namespace

const SidStation* sidStations(int& count) { count = 11; return kSid; }

SidMonitor::SidMonitor() { reset(); }

void SidMonitor::reset() {
    st_ = SidState();
    int n = 0;
    const SidStation* t = sidStations(n);
    st_.stations.resize((size_t)n);
    recent_.assign((size_t)n, {});
    minuteAcc_.assign((size_t)n, {});
    for (int i = 0; i < n; i++) { st_.stations[(size_t)i].call = t[i].call; st_.stations[(size_t)i].kHz = t[i].kHz; }
    acc_.clear(); accN_ = 0; nextAvg_ = avgSec_; sinceMin_ = 0;
    win_.clear();
    if (rate_ > 0) configure(rate_, dial_);
}

void SidMonitor::configure(double rate, double dialHz) {
    rate_ = rate; dial_ = dialHz;
    nfft_ = rate > 0 && rate <= 300000 ? (int)nextPow2((size_t)(rate / 3.0)) : 0;
    w_.assign((size_t)std::max(nfft_, 1), 0.f);
    for (int i = 0; i < nfft_; i++) w_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / nfft_));
    win_.clear();
    acc_.assign((size_t)std::max(nfft_, 1), 0.0);
    accN_ = 0; nextAvg_ = avgSec_;
    int n = 0;
    const SidStation* t = sidStations(n);
    binLo_.assign((size_t)n, 0); binHi_.assign((size_t)n, 0); nzLo_.assign((size_t)n, 0); nzHi_.assign((size_t)n, 0);
    const double binHz = nfft_ ? rate / nfft_ : 1.0;
    for (int i = 0; i < n; i++) {
        const double off = t[i].kHz * 1000.0 - dialHz;
        const bool in = nfft_ && std::fabs(off) < rate / 2 * 0.85;
        st_.stations[(size_t)i].inSpan = in;
        if (!in) continue;
        const int c = (int)std::lround(off / binHz);
        const int h = std::max(1, (int)std::lround(60.0 / binHz));   // +-60 Hz holds an MSK signal
        binLo_[(size_t)i] = c - h; binHi_[(size_t)i] = c + h;
        nzLo_[(size_t)i] = c + (int)std::lround(150.0 / binHz); nzHi_[(size_t)i] = c + (int)std::lround(400.0 / binHz);
    }
}

void SidMonitor::feed(const std::complex<float>* x, size_t n) {
    if (nfft_ <= 0) return;
    static thread_local std::vector<cd> a;
    for (size_t i = 0; i < n; i++) {
        win_.push_back(x[i]);
        if ((int)win_.size() == nfft_) {
            a.assign((size_t)nfft_, cd(0, 0));
            for (int k = 0; k < nfft_; k++) a[(size_t)k] = cd(win_[(size_t)k].real(), win_[(size_t)k].imag()) * (double)w_[(size_t)k];
            fft(a);
            for (int k = 0; k < nfft_; k++) acc_[(size_t)k] += std::norm(a[(size_t)k]);
            accN_++;
            win_.clear();
            st_.timeSec += (double)nfft_ / rate_;
            if (st_.timeSec >= nextAvg_) { finishAverage(); nextAvg_ += avgSec_; }
        }
    }
}

void SidMonitor::finishAverage() {
    if (accN_ == 0) return;
    const int n = (int)st_.stations.size();
    std::vector<float> lv((size_t)n, -200.f);
    const double norm = 1.0 / ((double)accN_ * (double)nfft_ * (double)nfft_ * 0.375);   // Hann: sum w^2 = 3/8 N
    auto bin = [&](int k) { return acc_[(size_t)(((k % nfft_) + nfft_) % nfft_)]; };
    std::vector<float> absv((size_t)n, -200.f);
    for (int i = 0; i < n; i++) {
        if (!st_.stations[(size_t)i].inSpan) continue;
        double sig = 0, nz = 0;
        int ns = 0, nn = 0;
        for (int k = binLo_[(size_t)i]; k <= binHi_[(size_t)i]; k++) { sig += bin(k); ns++; }
        // the noise reference: the quieter side band (150 .. 400 Hz on either side), skipping nothing else (a second station that close is rare)
        double lo = 0, hi = 0;
        int nl = 0, nh = 0;
        const int c = (binLo_[(size_t)i] + binHi_[(size_t)i]) / 2;
        for (int k = c - (nzHi_[(size_t)i] - c); k <= c - (nzLo_[(size_t)i] - c); k++) { lo += bin(k); nl++; }
        for (int k = nzLo_[(size_t)i]; k <= nzHi_[(size_t)i]; k++) { hi += bin(k); nh++; }
        nz = std::min(nl ? lo / nl : 1e30, nh ? hi / nh : 1e30);
        (void)nn;
        if (nz <= 0 || nz > 1e29) continue;
        const double sigMean = sig / ns;
        lv[(size_t)i] = (float)(10 * std::log10(std::max(1e-12, sigMean / nz)));
        absv[(size_t)i] = (float)(10 * std::log10(std::max(1e-30, sig * norm)));
    }
    for (int i = 0; i < n; i++) st_.stations[(size_t)i].absDb = absv[(size_t)i];
    std::fill(acc_.begin(), acc_.end(), 0.0);
    accN_ = 0;
    pushLevels(lv, avgSec_);
}

void SidMonitor::pushLevels(const std::vector<float>& lv, double dt) {
    const size_t n = std::min(lv.size(), st_.stations.size());
    if (nfft_ == 0 && st_.timeSec >= 0) st_.timeSec += dt;   // test hook without a receiver: time moves with the levels
    sinceMin_ += dt;
    for (size_t i = 0; i < n; i++) {
        SidStationState& s = st_.stations[i];
        if (lv[i] <= -199) { s.present = false; continue; }
        s.inSpan = true;
        s.levelDb = lv[i];
        s.present = lv[i] >= 6.0f;
        recent_[i].push_back(lv[i]);
        if (recent_[i].size() > (size_t)(3600.0 / std::max(0.5, dt)) + 1) recent_[i].erase(recent_[i].begin());
        minuteAcc_[i].push_back(lv[i]);
        evaluate(i);
    }
    if (sinceMin_ >= 60.0) {
        sinceMin_ = 0;
        for (size_t i = 0; i < st_.stations.size(); i++) {
            auto& m = minuteAcc_[i];
            if (m.empty()) continue;
            float sum = 0; for (float v : m) sum += v;
            st_.stations[i].history.push_back(sum / (float)m.size());
            if (st_.stations[i].history.size() > 1440) st_.stations[i].history.erase(st_.stations[i].history.begin());
            m.clear();
        }
    }
}

void SidMonitor::evaluate(size_t i) {
    SidStationState& s = st_.stations[i];
    const auto& r = recent_[i];
    const double dt = avgSec_;
    const size_t per2 = std::max<size_t>(1, (size_t)(120.0 / dt));
    if (r.size() < (size_t)(15 * 60.0 / dt)) return;            // 15 minutes of history first
    // now: mean of the last 2 min; before: mean of a 2 min window that ended 10 min ago; baseline: median of the last 60 min
    auto mean = [&](size_t a, size_t b) { double m = 0; for (size_t k = a; k < b; k++) m += r[k]; return m / (double)(b - a); };
    const size_t N = r.size();
    const size_t back10 = (size_t)(600.0 / dt);
    const double now = mean(N - per2, N);
    const double before = mean(N - back10 - per2, N - back10);
    std::vector<float> t(r);
    std::nth_element(t.begin(), t.begin() + (long)t.size() / 2, t.end());
    s.baselineDb = t[t.size() / 2];
    s.riseDb = (float)(now - s.baselineDb);
    if (s.baselineDb < 6.0f) { s.event = false; return; }   // a station that is not there for most of the last hour cannot have a flare
    const double fast = now - before;
    const bool was = s.event;
    if (!s.event && std::fabs(fast) >= riseDb_ && s.present) {
        s.event = true; s.eventPeakDb = fast;
        st_.eventsTotal++; st_.lastEventAgoSec = 0; st_.lastEventRiseDb = fast; st_.lastEventCall = s.call;
    } else if (s.event) {
        if (std::fabs(now - s.baselineDb) < 0.5 * riseDb_) s.event = false;
        else if (std::fabs(fast) > std::fabs(s.eventPeakDb)) { s.eventPeakDb = fast; st_.lastEventRiseDb = fast; }
    }
    (void)was;
    if (st_.lastEventAgoSec >= 0) st_.lastEventAgoSec += dt / (double)std::max<size_t>(1, st_.stations.size());
}

std::vector<std::complex<float>> sidRender(double rate, double dialHz, double seconds, const std::vector<std::pair<double, double>>& carriers, double noiseRms,
                                           const std::vector<std::vector<float>>& gainDbPerSecond, uint32_t seed) {
    const size_t n = (size_t)(seconds * rate);
    std::vector<std::complex<float>> y(n);
    Rng r(seed);
    for (size_t i = 0; i < n; i++) y[i] = std::complex<float>((float)(noiseRms * r.gauss() / std::sqrt(2.0)), (float)(noiseRms * r.gauss() / std::sqrt(2.0)));
    for (size_t c = 0; c < carriers.size(); c++) {
        const double w = 2 * kPi * (carriers[c].first - dialHz) / rate;
        const double amp = noiseRms * std::pow(10.0, carriers[c].second / 20.0);
        cd osc(1, 0);
        const cd step(std::cos(w), std::sin(w));
        double g = 1.0;
        for (size_t i = 0; i < n; i++) {
            if ((i % (size_t)rate) == 0 && c < gainDbPerSecond.size() && !gainDbPerSecond[c].empty()) {
                const size_t sec = std::min(gainDbPerSecond[c].size() - 1, (size_t)((double)i / rate));
                g = std::pow(10.0, gainDbPerSecond[c][sec] / 20.0);
            }
            y[i] += std::complex<float>((float)(amp * g * osc.real()), (float)(amp * g * osc.imag()));
            osc *= step;
            if ((i & 1023) == 0) osc /= std::abs(osc);
        }
    }
    return y;
}

// ================================================================================================================================ Riometer
double Riometer::siderealHours(double unixTime, double lonDeg) {
    const double jd = unixTime / 86400.0 + 2440587.5;
    const double d = jd - 2451545.0;
    double gmst = 18.697374558 + 24.06570982441908 * d;
    double lst = gmst + lonDeg / 15.0;
    lst = std::fmod(lst, 24.0);
    if (lst < 0) lst += 24.0;
    return lst;
}

Riometer::Riometer() { qdc_.assign(288, 0.f); qdcN_.assign(288, 0); reset(); }

void Riometer::configure(double rate) { rate_ = rate; sampleN_ = 0; blk_.clear(); blkAcc_ = 0; blkN_ = 0; secs_.clear(); minuteEnd_ = 60; floorSecs_.clear(); }

void Riometer::reset() {
    const auto q = qdc_; const auto qn = qdcN_;
    st_ = RioState();
    qdc_ = q; qdcN_ = qn;
    sampleN_ = 0; blk_.clear(); blkAcc_ = 0; blkN_ = 0; secs_.clear(); minuteEnd_ = 60; floorSecs_.clear();
    rejects_ = minutes_ = 0;
    int fill = 0;
    for (int i = 0; i < 288; i++) if (qdc_[(size_t)i] > 0) fill++;
    st_.qdcFill = fill;
    st_.haveQdc = fill >= 36;
}

void Riometer::feed(const std::complex<float>* x, size_t n) {
    if (rate_ <= 0) return;
    // short block means of the noise floor: the median over the bins of a 1024-point spectrum rejects narrow carriers, the lowest quarter over the blocks of a
    // second rejects bursts of interference
    static constexpr int kN = 1024;
    static thread_local std::vector<cd> a;
    static thread_local std::vector<std::complex<float>> buf;
    static thread_local std::vector<float> w;
    if (w.empty()) { w.resize(kN); for (int i = 0; i < kN; i++) w[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kN)); }
    const size_t perBlock = std::max<size_t>(kN, (size_t)std::llround(rate_ * 0.02));
    for (size_t i = 0; i < n; i++) {
        buf.push_back(x[i]);
        if (buf.size() >= perBlock) {
            // average of the 1024-point spectra of the block
            std::vector<double> P(kN, 0.0);
            int cnt = 0;
            for (size_t s = 0; s + kN <= buf.size(); s += kN, cnt++) {
                a.assign(kN, cd(0, 0));
                for (int k = 0; k < kN; k++) a[(size_t)k] = cd(buf[s + (size_t)k].real(), buf[s + (size_t)k].imag()) * (double)w[(size_t)k];
                fft(a);
                for (int k = 0; k < kN; k++) P[(size_t)k] += std::norm(a[(size_t)k]);
            }
            std::vector<double> t(P);
            std::nth_element(t.begin(), t.begin() + kN / 2, t.end());
            const double med = t[kN / 2] / 0.693 / ((double)cnt * kN * 0.375);   // exponential median -> mean; Hann: sum w^2 = 3/8 N; per bin of white noise of power 1 this is 1
            floorSecs_.push_back((float)med);
            buf.clear();
            sampleN_ += perBlock;
        }
        // the one second boundary: counted on the samples consumed so far (+ the buffer)
    }
    const double t = ((double)sampleN_) / rate_;
    // finish seconds
    while (t >= (double)(secs_.size() + 1) && !floorSecs_.empty()) {
        std::vector<float> f(floorSecs_);
        floorSecs_.clear();
        std::sort(f.begin(), f.end());
        const double low = f[f.size() / 4];
        const double med = f[f.size() / 2];
        secs_.push_back(low);
        if (med > 1.35 * low) rejects_++;
    }
    while (secs_.size() >= 60) {
        std::vector<double> m(secs_.begin(), secs_.begin() + 60);
        secs_.erase(secs_.begin(), secs_.begin() + 60);
        std::sort(m.begin(), m.end());
        const bool bad = rejects_ > 20;
        rejects_ = 0;
        pushMinute(m[20], startUnix_ + minuteEnd_, bad);
        minuteEnd_ += 60;
    }
}

void Riometer::pushMinute(double powerLin, double unixTime, bool interfered) {
    st_.timeSec = unixTime - startUnix_;
    const double lst = siderealHours(unixTime, lon_);
    st_.lstHours = lst;
    const int bin = std::min(287, (int)(lst / 24.0 * 288.0));
    minutes_++;
    st_.interferedFrac = st_.interferedFrac * 0.98 + (interfered ? 0.02 : 0.0);
    const float p = (float)powerLin;
    st_.powerDb = (float)(10 * std::log10(std::max(1e-30, powerLin)));
    float& q = qdc_[(size_t)bin];
    if (!interfered) {
        if (q <= 0) { q = p; qdcN_[(size_t)bin] = 1; }
        else if (p > q && p < q * 1.15f) { q = 0.8f * q + 0.2f * p; qdcN_[(size_t)bin]++; }   // the upper envelope of quiet minutes; a jump over 0.6 dB is interference, not sky
        else if (p <= q) { q = q * 0.999f + p * 0.001f; }                                    // the curve is slowly pulled down when the sky stays under it (gain drift)
    }
    int fill = 0;
    for (float v : qdc_) if (v > 0) fill++;
    st_.qdcFill = fill;
    st_.haveQdc = fill >= 36 && q > 0;
    st_.qdcDb = q > 0 ? (float)(10 * std::log10(q)) : -200.f;
    if (st_.haveQdc) {
        st_.absorptionDb = std::max(-2.f, (float)(10 * std::log10(q / std::max(1e-30, powerLin))));
        st_.event = st_.absorptionDb >= thr_;
    } else { st_.absorptionDb = 0; st_.event = false; }
    st_.absHistory.push_back(st_.absorptionDb);
    st_.powerHistory.push_back(st_.powerDb);
    if (st_.absHistory.size() > 1440) { st_.absHistory.erase(st_.absHistory.begin()); st_.powerHistory.erase(st_.powerHistory.begin()); }
}

std::string Riometer::exportQdc() const {
    std::ostringstream o;
    o << "qdc1";
    for (float v : qdc_) { char b[24]; std::snprintf(b, sizeof b, " %.4g", (double)v); o << b; }
    return o.str();
}
bool Riometer::importQdc(const std::string& s) {
    std::istringstream in(s);
    std::string tag;
    in >> tag;
    if (tag != "qdc1") return false;
    std::vector<float> v;
    float f;
    while (in >> f) v.push_back(f);
    if (v.size() != 288) return false;
    for (float x : v) if (!(x >= 0) || !std::isfinite(x)) return false;
    qdc_ = v;
    int fill = 0;
    for (float x : qdc_) if (x > 0) fill++;
    st_.qdcFill = fill;
    st_.haveQdc = fill >= 36;
    return true;
}

std::vector<std::complex<float>> rioRender(double rate, double seconds, double powerLin, double unixStart, double lonDeg, double modDepth,
                                           const std::vector<std::pair<double, double>>& absorbSecDb, double interferenceFrac, uint32_t seed) {
    const size_t n = (size_t)(seconds * rate);
    std::vector<std::complex<float>> y(n);
    Rng r(seed);
    const size_t blk = std::max<size_t>(1, (size_t)(rate * 0.02));
    for (size_t b = 0; b * blk < n; b++) {
        const double t = (double)(b * blk) / rate;
        const double lst = Riometer::siderealHours(unixStart + t, lonDeg);
        double pw = powerLin * (1.0 + modDepth * std::cos(2 * kPi * (lst - 17.7) / 24.0));
        for (auto& ab : absorbSecDb) if (t >= ab.first) pw *= std::pow(10.0, -ab.second / 10.0);
        bool burst = r.uni() < interferenceFrac;
        const double amp = std::sqrt(pw / 2.0);
        for (size_t i = b * blk; i < std::min(n, (b + 1) * blk); i++) {
            double g = amp;
            if (burst) g *= 3.0;   // +9.5 dB
            y[i] = std::complex<float>((float)(g * r.gauss()), (float)(g * r.gauss()));
        }
    }
    return y;
}

// ================================================================================================================================ pulsars
namespace {
// From memory: periods are for orientation only. Check against the ATNF Pulsar Catalogue (psrcat) and correct for the Doppler shift of the Earth's orbit
// (up to 1e-4 of the period) before a long fold; the search over +-2e-4 of the period covers that for a nominal period that is right to 1e-5.
const PulsarTarget kPsr[] = {
    {"B0329+54", 0.714520, 26.76, 1.5, 408, "the brightest pulsar in the northern sky; flux about 1.5 Jy near 400 MHz"},
    {"B0950+08", 0.253065, 2.97, 0.4, 408, "close, low dispersion, flux about 0.4 Jy near 400 MHz"},
    {"B1133+16", 1.187911, 4.84, 0.26, 408, "flux about 0.26 Jy near 400 MHz"},
    {"B1929+10", 0.226518, 3.18, 0.3, 408, "flux about 0.3 Jy near 400 MHz"},
    {"B0809+74", 1.292241, 5.75, 0.56, 408, "flux about 0.56 Jy near 400 MHz"},
    {"B2016+28", 0.557953, 14.18, 0.31, 408, "flux about 0.3 Jy near 400 MHz"},
    {"B1919+21", 1.337302, 12.43, 0.2, 408, "the first pulsar found (1967)"},
    {"B0531+21 (Crab)", 0.033392, 56.77, 0.65, 408, "mean flux about 0.65 Jy; the giant pulses are the way amateurs see it"},
    {"B0833-45 (Vela)", 0.089367, 67.99, 5.0, 408, "southern sky; flux about 5 Jy near 400 MHz"},
};
}  // namespace

const PulsarTarget* pulsarTargets(int& count) { count = (int)(sizeof kPsr / sizeof kPsr[0]); return kPsr; }

double pulsarDelaySec(double dm, double loMHz, double hiMHz) { return 4.148808e3 * dm * (1.0 / (loMHz * loMHz) - 1.0 / (hiMHz * hiMHz)); }

double pulsarRequiredAreaM2(double fluxJy, double bandwidthHz, double tsysK, double seconds, double duty, double snr, int npol) {
    // S/N = S * G * sqrt(npol * BW * t) / Tsys * sqrt((1 - duty) / duty), G = A / (2 k) in K / Jy
    const double d = std::clamp(duty, 1e-4, 0.5);
    const double G = snr * tsysK / (fluxJy * std::sqrt((double)npol * bandwidthHz * seconds) * std::sqrt((1 - d) / d));
    return G * 2.0 * 1.380649e-23 / 1e-26;
}

PulsarFolder::PulsarFolder(const PulsarConfig& c) { configure(c); }

void PulsarFolder::configure(const PulsarConfig& c) {
    cfg_ = c;
    cfg_.channels = (int)std::clamp<size_t>(nextPow2((size_t)std::max(8, c.channels)), 8, 1024);
    fftN_ = cfg_.channels;
    rowTarget_ = std::max(1, (int)std::llround(c.sampleSec * c.rateHz / fftN_));
    cfg_.sampleSec = (double)rowTarget_ * fftN_ / c.rateHz;
    win_.assign((size_t)fftN_, 0.f);
    for (int i = 0; i < fftN_; i++) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / fftN_));
    reset();
}

void PulsarFolder::reset() { fft_.clear(); fftFill_ = 0; rowAcc_.assign((size_t)cfg_.channels, 0.0); rowCnt_ = 0; series_.clear(); samplesOut_ = 0; }

void PulsarFolder::pushRow(const float* p) {
    const size_t maxFloats = 16u * 1024 * 1024;   // 64 MB
    series_.insert(series_.end(), p, p + cfg_.channels);
    samplesOut_++;
    if (series_.size() > maxFloats) series_.erase(series_.begin(), series_.begin() + (long)(series_.size() - maxFloats));
}

void PulsarFolder::feed(const std::complex<float>* x, size_t n) {
    static thread_local std::vector<cd> a;
    for (size_t i = 0; i < n; i++) {
        fft_.push_back(x[i]);
        if ((int)fft_.size() == fftN_) {
            a.assign((size_t)fftN_, cd(0, 0));
            for (int k = 0; k < fftN_; k++) a[(size_t)k] = cd(fft_[(size_t)k].real(), fft_[(size_t)k].imag()) * (double)win_[(size_t)k];
            detail::fft(a);
            // channel order from the lowest frequency: FFT bins N/2 .. N-1 are the negative frequencies (lowest), 0 .. N/2-1 the upper half
            for (int k = 0; k < fftN_; k++) rowAcc_[(size_t)k] += std::norm(a[(size_t)((k + fftN_ / 2) % fftN_)]);
            fft_.clear();
            if (++rowCnt_ >= rowTarget_) {
                std::vector<float> row((size_t)fftN_);
                for (int k = 0; k < fftN_; k++) row[(size_t)k] = (float)(rowAcc_[(size_t)k] / rowCnt_);
                pushRow(row.data());
                std::fill(rowAcc_.begin(), rowAcc_.end(), 0.0);
                rowCnt_ = 0;
            }
        }
    }
}

namespace {
double bestWidthSnr(const std::vector<double>& prof, int* peakBin, int* widthOut, double* dutyOut) {
    const int B = (int)prof.size();
    double bestSnr = -1e9;
    int bestW = 1, bestPk = 0;
    // off-pulse statistics from the profile without its strongest 25 %: robust against the pulse itself
    std::vector<double> s(prof);
    std::sort(s.begin(), s.end());
    const int keep = B * 3 / 4;
    double mean = 0;
    for (int i = 0; i < keep; i++) mean += s[(size_t)i];
    mean /= keep;
    double var = 0;
    for (int i = 0; i < keep; i++) var += (s[(size_t)i] - mean) * (s[(size_t)i] - mean);
    // the lowest 75 % of a Gaussian have a variance of about 0.5 of the whole
    const double sd = std::sqrt(var / keep / 0.5) + 1e-30;
    for (int w : {1, 2, 3, 4, 6, 8, 12, 16}) {
        if (w > B / 3) break;
        for (int st = 0; st < B; st++) {
            double sum = 0;
            for (int k = 0; k < w; k++) sum += prof[(size_t)((st + k) % B)] - mean;
            const double snr = sum / (sd * std::sqrt((double)w));
            if (snr > bestSnr) { bestSnr = snr; bestW = w; bestPk = (st + w / 2) % B; }
        }
    }
    *peakBin = bestPk; *widthOut = bestW; *dutyOut = (double)bestW / B;
    return bestSnr;
}
}  // namespace

PulsarResult PulsarFolder::search() {
    PulsarResult res;
    const int C = cfg_.channels;
    const size_t R = series_.size() / (size_t)C;
    const double dt = cfg_.sampleSec;
    res.observedSec = (double)R * dt;
    if (R < (size_t)(8.0 * cfg_.periodS / dt) || R < 200) return res;
    // channel frequencies (MHz), low first
    std::vector<double> fMHz((size_t)C);
    const double bw = cfg_.rateHz / 1e6;
    for (int c = 0; c < C; c++) fMHz[(size_t)c] = cfg_.centreHz / 1e6 - bw / 2 + (c + 0.5) * bw / C;
    const double fHi = fMHz[(size_t)C - 1];
    // normalise: per channel remove a running mean (1 s blocks) and scale to unit rms
    std::vector<float> z(series_.size());
    {
        const size_t blk = std::max<size_t>(8, (size_t)(1.0 / dt));
        for (int c = 0; c < C; c++) {
            double ss = 0;
            size_t cnt = 0;
            for (size_t b0 = 0; b0 < R; b0 += blk) {
                const size_t b1 = std::min(R, b0 + blk);
                double m = 0;
                for (size_t r = b0; r < b1; r++) m += series_[r * (size_t)C + (size_t)c];
                m /= (double)(b1 - b0);
                for (size_t r = b0; r < b1; r++) { const double v = series_[r * (size_t)C + (size_t)c] / std::max(1e-30, m) - 1.0; z[r * (size_t)C + (size_t)c] = (float)v; ss += v * v; cnt++; }
            }
            const double rms = std::sqrt(ss / std::max<size_t>(1, cnt)) + 1e-30;
            for (size_t r = 0; r < R; r++) z[r * (size_t)C + (size_t)c] = (float)(z[r * (size_t)C + (size_t)c] / rms);
        }
    }
    const int nDm = std::max(1, cfg_.dmSteps), nP = std::max(1, cfg_.periodSteps);
    std::vector<float> dedisp(R);
    double bestSnr = -1e9;
    res.dmCurve.assign((size_t)nDm, -1e9f);
    res.periodCurve.assign((size_t)nP, -1e9f);
    for (int di = 0; di < nDm; di++) {
        const double dm = nDm == 1 ? cfg_.dm : cfg_.dm - cfg_.dmSpan + 2 * cfg_.dmSpan * di / (nDm - 1);
        if (dm < 0) continue;
        std::vector<long> shift((size_t)C);
        long maxShift = 0;
        for (int c = 0; c < C; c++) { shift[(size_t)c] = (long)std::llround(pulsarDelaySec(dm, fMHz[(size_t)c], fHi) / dt); maxShift = std::max(maxShift, shift[(size_t)c]); }
        if ((size_t)maxShift + 100 >= R) continue;
        const size_t Rd = R - (size_t)maxShift;
        for (size_t r = 0; r < Rd; r++) {
            float sum = 0;
            for (int c = 0; c < C; c++) sum += z[(r + (size_t)shift[(size_t)c]) * (size_t)C + (size_t)c];
            dedisp[r] = sum;
        }
        for (int pi = 0; pi < nP; pi++) {
            const double P = nP == 1 ? cfg_.periodS : cfg_.periodS * (1.0 + cfg_.periodRelSpan * (2.0 * pi / (nP - 1) - 1.0));
            std::vector<double> prof((size_t)cfg_.bins, 0.0), cnt((size_t)cfg_.bins, 0.0);
            for (size_t r = 0; r < Rd; r++) {
                double ph = std::fmod((double)r * dt / P, 1.0);
                if (ph < 0) ph += 1.0;
                const int b = std::min(cfg_.bins - 1, (int)(ph * cfg_.bins));
                prof[(size_t)b] += dedisp[r]; cnt[(size_t)b] += 1.0;
            }
            for (int b = 0; b < cfg_.bins; b++) prof[(size_t)b] = cnt[(size_t)b] > 0 ? prof[(size_t)b] / cnt[(size_t)b] : 0.0;
            int pk = 0, w = 1;
            double duty = 0;
            const double snr = bestWidthSnr(prof, &pk, &w, &duty);
            // the profile is a mean of many samples: the S/N above is relative to the scatter of the bins, which already includes the averaging
            res.dmCurve[(size_t)di] = std::max(res.dmCurve[(size_t)di], (float)snr);
            res.periodCurve[(size_t)pi] = std::max(res.periodCurve[(size_t)pi], (float)snr);
            if (snr > bestSnr) {
                bestSnr = snr;
                res.snr = snr; res.periodS = P; res.dm = dm; res.phasePeak = (double)pk / cfg_.bins; res.dutyCycle = duty;
                // the profile in units of the scatter of its bins, mean removed
                double mean = 0;
                for (double v : prof) mean += v;
                mean /= prof.size();
                double sd = 0;
                std::vector<double> sorted(prof);
                std::sort(sorted.begin(), sorted.end());
                const int keep = cfg_.bins * 3 / 4;
                double m2 = 0;
                for (int k = 0; k < keep; k++) m2 += sorted[(size_t)k];
                m2 /= keep;
                for (int k = 0; k < keep; k++) sd += (sorted[(size_t)k] - m2) * (sorted[(size_t)k] - m2);
                sd = std::sqrt(sd / keep / 0.5) + 1e-30;
                res.profile.resize(prof.size());
                // rotate so the peak sits in the middle
                for (int b = 0; b < cfg_.bins; b++) res.profile[(size_t)((b - pk + cfg_.bins / 2 + cfg_.bins) % cfg_.bins)] = (float)((prof[(size_t)b] - m2) / sd);
                (void)mean;
            }
        }
    }
    res.ok = bestSnr >= 7.0;   // about 1 false alarm in 10^6 trials of a Gaussian: the trial count here is a few thousand
    return res;
}

std::vector<float> pulsarRenderRows(const PulsarConfig& cIn, double seconds, double snr, uint32_t seed, double duty, double trueDm, double trueP, double noiseRel) {
    PulsarConfig c = cIn;
    const int C = (int)nextPow2((size_t)std::max(8, c.channels));
    const double dt = c.sampleSec;
    const size_t R = (size_t)(seconds / dt);
    const double dm = trueDm >= 0 ? trueDm : c.dm, P = trueP > 0 ? trueP : c.periodS;
    std::vector<double> fMHz((size_t)C);
    const double bw = c.rateHz / 1e6;
    for (int k = 0; k < C; k++) fMHz[(size_t)k] = c.centreHz / 1e6 - bw / 2 + (k + 0.5) * bw / C;
    const double fHi = fMHz[(size_t)C - 1];
    std::vector<double> delay((size_t)C);
    for (int k = 0; k < C; k++) delay[(size_t)k] = pulsarDelaySec(dm, fMHz[(size_t)k], fHi);
    std::vector<float> out(R * (size_t)C);
    Rng r(seed);
    const double sigma = noiseRel > 0 ? noiseRel : 0.05;   // scatter of the mean noise power per row and channel (an average over many FFT frames)
    const double w = duty * P;
    for (size_t i = 0; i < R; i++) {
        const double t = (double)i * dt;
        for (int k = 0; k < C; k++) {
            // gain drift per channel: a slow ripple the folder must remove
            const double drift = 1.0 + 0.02 * std::sin(2 * kPi * t / 37.0 + k);
            double v = drift * (1.0 + sigma * r.gauss());
            // the pulse arrives later at the lower frequencies: its start time at this channel is (n P + delay)
            double ph = std::fmod(t - delay[(size_t)k], P);
            if (ph < 0) ph += P;
            if (ph < w) {
                const double u = ph / w;
                v += snr * sigma * 1.0 * (0.5 - 0.5 * std::cos(2 * kPi * u)) * 2.0;   // raised cosine, mean amplitude snr * sigma
            }
            out[i * (size_t)C + (size_t)k] = (float)v;
        }
    }
    return out;
}

std::vector<std::complex<float>> pulsarRender(const PulsarConfig& c, double seconds, double snr, uint32_t seed, double duty, double trueDm, double trueP) {
    const size_t n = (size_t)(seconds * c.rateHz);
    std::vector<std::complex<float>> y(n);
    Rng r(seed);
    for (size_t i = 0; i < n; i++) y[i] = std::complex<float>((float)(r.gauss() / std::sqrt(2.0)), (float)(r.gauss() / std::sqrt(2.0)));
    const int C = (int)nextPow2((size_t)std::max(8, c.channels));
    const double dm = trueDm >= 0 ? trueDm : c.dm, P = trueP > 0 ? trueP : c.periodS;
    const double bw = c.rateHz / 1e6;
    std::vector<double> fMHz((size_t)C);
    for (int k = 0; k < C; k++) fMHz[(size_t)k] = c.centreHz / 1e6 - bw / 2 + (k + 0.5) * bw / C;
    const double fHi = fMHz[(size_t)C - 1];
    const double a = std::sqrt(snr / C);   // per channel tone: the pulse power is C a^2 = snr x the noise power (1)
    const double w = duty * P;
    for (int k = 0; k < C; k++) {
        const double fOff = (fMHz[(size_t)k] - c.centreHz / 1e6) * 1e6;
        const double delay = pulsarDelaySec(dm, fMHz[(size_t)k], fHi);
        const double phase0 = 2 * kPi * r.uni();
        for (double t0 = std::fmod(delay, P); t0 < seconds; t0 += P) {
            const size_t i0 = (size_t)(t0 * c.rateHz), i1 = std::min(n, (size_t)((t0 + w) * c.rateHz));
            for (size_t i = i0; i < i1; i++) {
                const double u = (double)(i - i0) / (double)std::max<size_t>(1, i1 - i0);
                const double env = 0.5 - 0.5 * std::cos(2 * kPi * u);
                const double ph = phase0 + 2 * kPi * fOff * (double)i / c.rateHz;
                y[i] += std::complex<float>((float)(a * env * 2 * std::cos(ph)), (float)(a * env * 2 * std::sin(ph)));
            }
        }
    }
    return y;
}

}  // namespace hamsci
}  // namespace dect2

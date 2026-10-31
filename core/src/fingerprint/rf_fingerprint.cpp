// RF fingerprint of an NFM / WFM / AM transmitter: see core/include/dect2/fingerprint/rf_fingerprint.h and docs/RF-FINGERPRINT.md.
#include "dect2/fingerprint/rf_fingerprint.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace dect2::fp {

namespace {
const double kPi = 3.14159265358979323846;

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, (size_t)(p / 100.0 * (double)(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    return v[k];
}

// zero-phase windowed-sinc low pass; the L/2 samples at each end are dropped
std::vector<float> lowpass(const std::vector<float>& x, double fs, double fc) {
    int L = (int)(6.6 * fs / fc) | 1;
    L = std::clamp(L, 5, 1201);
    if ((size_t)L * 2 > x.size()) return {};
    std::vector<float> h((size_t)L);
    double sum = 0;
    for (int j = 0; j < L; j++) {
        const double t = j - (L - 1) / 2.0, a = 2 * kPi * fc / fs * t;
        const double s = std::fabs(a) < 1e-9 ? 1.0 : std::sin(a) / a;
        h[(size_t)j] = (float)(s * (0.54 - 0.46 * std::cos(2 * kPi * j / (L - 1))));
        sum += h[(size_t)j];
    }
    for (auto& v : h) v = (float)(v / sum);
    std::vector<float> y(x.size() - (size_t)L + 1);
    for (size_t i = 0; i < y.size(); i++) {
        double acc = 0;
        for (int j = 0; j < L; j++) acc += (double)h[(size_t)j] * x[i + (size_t)j];
        y[i] = (float)acc;
    }
    return y;
}

// x minus its moving average of w samples (a crude high pass below fs/w), same length minus w
std::vector<float> highpassBox(const std::vector<float>& x, size_t w) {
    if (x.size() <= w + 2 || w < 2) return x;
    std::vector<float> y(x.size() - w);
    double s = 0;
    for (size_t i = 0; i < w; i++) s += x[i];
    for (size_t i = 0; i < y.size(); i++) {
        y[i] = (float)(x[i + w / 2] - s / (double)w);
        s += x[i + w] - x[i];
    }
    return y;
}

double rmsOf(const std::vector<float>& x) {
    if (x.empty()) return 0;
    double s = 0;
    for (float v : x) s += (double)v * v;
    return std::sqrt(s / (double)x.size());
}

// Hann-windowed single-bin DFT amplitude of a real signal at f (peak amplitude of a sine there)
double toneAmp(const std::vector<float>& x, double fs, double f) {
    const size_t n = x.size();
    if (n < 16) return 0;
    std::complex<double> acc(0, 0);
    double ws = 0;
    const double w0 = 2 * kPi * f / fs;
    std::complex<double> rot = std::polar(1.0, -w0), ph(1, 0);
    for (size_t i = 0; i < n; i++) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)n);
        acc += (double)x[i] * w * ph;
        ws += w;
        ph *= rot;
        if ((i & 255) == 255) ph /= std::abs(ph);
    }
    return 2.0 * std::abs(acc) / ws;
}

// Welch PSD of a real series (segment 1024, Hann): bin power, bin Hz
void welchReal(const std::vector<float>& x, double fs, std::vector<double>& p, double& binHz) {
    const int N = 1024;
    p.assign(N / 2, 0.0);
    binHz = fs / N;
    if (x.size() < (size_t)N) return;
    Fft fft(N);
    std::vector<cf32> buf((size_t)N);
    std::vector<float> w((size_t)N);
    for (int i = 0; i < N; i++) w[(size_t)i] = 0.5f - 0.5f * std::cos(2 * 3.14159265f * i / N);
    int segs = 0;
    for (size_t s = 0; s + N <= x.size(); s += N / 2) {
        for (int i = 0; i < N; i++) buf[(size_t)i] = cf32(x[s + (size_t)i] * w[(size_t)i], 0.f);
        fft.forward(buf.data());
        for (int i = 0; i < N / 2; i++) p[(size_t)i] += std::norm(buf[(size_t)i]);
        segs++;
    }
    for (auto& v : p) v /= std::max(1, segs);
}

// pre-emphasis slope, dB per octave, between flo and fhi
bool tiltOf(const std::vector<float>& x, double fs, double flo, double fhi, double& out) {
    std::vector<double> p;
    double bh;
    welchReal(x, fs, p, bh);
    if (p.empty()) return false;
    // regress 10log10(P) on log2(f)
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (size_t k = 1; k < p.size(); k++) {
        const double f = (double)k * bh;
        if (f < flo || f > fhi || p[k] <= 0) continue;
        const double X = std::log2(f), Y = 10 * std::log10(p[k]);
        sx += X; sy += Y; sxx += X * X; sxy += X * Y; n++;
    }
    if (n < 6) return false;
    const double d = n * sxx - sx * sx;
    if (d < 1e-9) return false;
    out = (n * sxy - sx * sy) / d;
    return true;
}

// lag-1 (Kay) frequency estimate of z[a,b), Hz, and the summed lag product
double kay(const cf32* z, size_t a, size_t b, double fs, std::complex<double>* rOut = nullptr) {
    std::complex<double> r(0, 0);
    for (size_t i = a; i + 1 < b; i++) {
        const std::complex<float> p = z[i + 1] * std::conj(z[i]);
        r += std::complex<double>(p.real(), p.imag());
    }
    if (rOut) *rOut = r;
    return std::arg(r) * fs / (2 * kPi);
}

double kindDevDefault(Kind k) { return k == Kind::NFM ? 300.0 : k == Kind::WFM ? 500.0 : 2.0; }
}  // namespace

const char* kindName(Kind k) { return k == Kind::NFM ? "NFM" : k == Kind::WFM ? "WFM" : "AM"; }

// ---------------------------------------------------------------- Channel

Channel::Channel(double inRate, double off, Kind kind, double bw) : inRate_(inRate) {
    double target = kind == Kind::WFM ? 300e3 : kind == Kind::NFM ? 50e3 : 24e3;
    if (bw > 0) target = std::max(target, 2.4 * bw);
    dec_ = std::max(1, (int)std::floor(inRate / target));
    outRate_ = inRate / dec_;
    const double defBw = kind == Kind::WFM ? 200e3 : kind == Kind::NFM ? 25e3 : 10e3;
    double fc = std::min(0.45 * outRate_, (bw > 0 ? bw : defBw) * 0.6);
    fc = std::min(fc, 0.45 * inRate);
    const double tw = std::max(0.1 * outRate_, outRate_ - 2 * fc);
    int L = (int)std::ceil(3.3 * inRate / tw) | 1;
    L = std::clamp(L, 9, 4001);
    theta_ = 2 * kPi * off / inRate;
    taps_.resize((size_t)L);
    std::vector<double> h((size_t)L);
    double sum = 0;
    for (int j = 0; j < L; j++) {
        const double t = j - (L - 1) / 2.0, a = 2 * kPi * fc / inRate * t;
        const double s = std::fabs(a) < 1e-9 ? 1.0 : std::sin(a) / a;
        h[(size_t)j] = s * (0.54 - 0.46 * std::cos(2 * kPi * j / (L - 1)));
        sum += h[(size_t)j];
    }
    // reversed, so the inner loop reads the input forwards; modulated to move the carrier to 0 Hz
    for (int j = 0; j < L; j++) taps_[(size_t)(L - 1 - j)] = (cf32)std::polar((float)(h[(size_t)j] / sum), (float)std::fmod(theta_ * j, 2 * kPi));
    hist_.assign((size_t)L - 1, cf32(0, 0));
}

void Channel::push(const cf32* x, size_t n, std::vector<cf32>& out) {
    const size_t L = taps_.size();
    std::vector<cf32> buf;
    buf.reserve(hist_.size() + n);
    buf.insert(buf.end(), hist_.begin(), hist_.end());
    buf.insert(buf.end(), x, x + n);
    size_t i = skip_;
    for (; i < n; i += (size_t)dec_) {
        float re = 0, im = 0;
        const cf32* b = buf.data() + i;
        const cf32* t = taps_.data();
        for (size_t k = 0; k < L; k++) {
            re += t[k].real() * b[k].real() - t[k].imag() * b[k].imag();
            im += t[k].real() * b[k].imag() + t[k].imag() * b[k].real();
        }
        const std::complex<double> rot = std::polar(1.0, -ph_);
        out.emplace_back((float)(re * rot.real() - im * rot.imag()), (float)(re * rot.imag() + im * rot.real()));
        ph_ = std::fmod(ph_ + theta_ * dec_, 2 * kPi);
    }
    skip_ = i - n;
    hist_.assign(buf.end() - (long)(L - 1), buf.end());
}

// ---------------------------------------------------------------- analyse

Features analyse(const cf32* z, size_t n, double fs, const Options& o) {
    Features f;
    f.kind = o.kind; f.tunedHz = o.tunedHz; f.rate = fs;
    if (n < (size_t)(fs * 0.03) || fs <= 0) return f;

    // 1. keying: block power at 0.5 ms
    const size_t W = std::max<size_t>(2, (size_t)(fs * 0.0005));
    const size_t nb = n / W;
    std::vector<double> pb(nb);
    for (size_t b = 0; b < nb; b++) {
        double s = 0;
        for (size_t i = 0; i < W; i++) s += std::norm(z[b * W + i]);
        pb[b] = s / (double)W;
    }
    const double lo = pct(pb, 10), hi = pct(pb, 90);
    const bool keyed = nb > 20 && hi > lo * 10.0 && lo > 0;
    size_t s0 = 0, e1 = n, onBlock = 0;
    double noise = o.noiseFloorPow;
    size_t t50 = 0;
    if (keyed) {
        const double thr = std::sqrt(lo * hi);
        size_t b = 0;
        while (b + 3 < nb && !(pb[b] > thr && pb[b + 1] > thr && pb[b + 2] > thr)) b++;
        onBlock = b;
        size_t be = nb;
        while (be > onBlock + 1 && pb[be - 1] < thr) be--;
        const size_t after = be * W - onBlock * W;
        const size_t skipS = std::min<size_t>((size_t)(fs * 0.06), after * 4 / 10);
        s0 = onBlock * W + skipS;
        e1 = std::min(n, be * W);
        if (e1 > (size_t)(fs * 0.004) && be < nb) e1 -= (size_t)(fs * 0.002);
        if (noise <= 0) {
            double s = 0; size_t c = 0;
            for (size_t k = 0; k < onBlock; k++) { s += pb[k]; c++; }
            if (c >= 4) noise = s / (double)c;
        }
    }
    if (e1 <= s0 + (size_t)(fs * 0.01)) return f;
    const size_t N = e1 - s0;
    f.durationS = (double)N / fs;

    // 2. power and SNR
    double ptot = 0;
    for (size_t i = s0; i < e1; i++) ptot += std::norm(z[i]);
    ptot /= (double)N;
    if (noise <= 0) {   // median of the averaged spectrum = the noise floor
        const int M = 512;
        Fft fft(M);
        std::vector<cf32> buf((size_t)M);
        std::vector<double> acc((size_t)M, 0.0);
        double wp = 0;
        std::vector<float> w((size_t)M);
        for (int i = 0; i < M; i++) { w[(size_t)i] = 0.5f - 0.5f * std::cos(2 * 3.14159265f * i / M); wp += (double)w[(size_t)i] * w[(size_t)i]; }
        int segs = 0;
        for (size_t a0 = s0; a0 + M <= e1; a0 += M / 2) {
            for (int i = 0; i < M; i++) buf[(size_t)i] = z[a0 + (size_t)i] * w[(size_t)i];
            fft.forward(buf.data());
            for (int i = 0; i < M; i++) acc[(size_t)i] += std::norm(buf[(size_t)i]);
            segs++;
        }
        if (segs > 0) { for (auto& v : acc) v /= segs * wp; noise = pct(acc, 50); }
    }
    noise = std::max(noise, 1e-20);
    f.snrDb = 10 * std::log10(std::max(ptot - noise, 1e-20) / noise);
    if (f.snrDb < 3) { f.valid = false; return f; }

    // 3. carrier offset, segments, drift
    std::complex<double> R;
    const double fRes = kay(z, s0, e1, fs, &R);
    f.cfoHz = fRes;
    {
        double v[4];
        for (int k = 0; k < 4; k++) v[k] = kay(z, s0 + N * (size_t)k / 4, s0 + N * (size_t)(k + 1) / 4, fs);
        double m = 0;
        for (double x : v) m += x;
        m /= 4;
        double var = 0;
        for (double x : v) var += (x - m) * (x - m);
        f.cfoStdHz = std::sqrt(var / 3.0) / 2.0;
        // unwrap against fRes (all within fs/2 of each other in practice)
    }
    if (N >= (size_t)(fs * 0.2)) {   // drift: slope of 10 ms estimates
        const size_t seg = std::max<size_t>(8, (size_t)(fs * 0.01));
        const size_t K = N / seg;
        double st = 0, sf = 0, stt = 0, stf = 0;
        for (size_t k = 0; k < K; k++) {
            const double t = ((double)k + 0.5) * (double)seg / fs, ff = kay(z, s0 + k * seg, s0 + (k + 1) * seg, fs);
            st += t; sf += ff; stt += t * t; stf += t * ff;
        }
        const double d = (double)K * stt - st * st;
        if (K >= 6 && d > 0) f.driftHzPerS = ((double)K * stf - st * sf) / d;
    }

    // 4. demodulation
    const bool fm = o.kind != Kind::AM;
    std::vector<float> dem(N - 1);
    const double argR = std::arg(R);
    const std::complex<float> unrot = std::polar(1.0f, (float)-argR);
    if (fm) {
        for (size_t i = 0; i + 1 < N; i++) dem[i] = (float)(std::arg(z[s0 + i + 1] * std::conj(z[s0 + i]) * unrot) * fs / (2 * kPi));
    } else {
        double mean = 0;
        for (size_t i = 0; i < N; i++) mean += std::abs(z[s0 + i]);
        mean /= (double)N;
        for (size_t i = 0; i + 1 < N; i++) dem[i] = (float)(100.0 * (std::abs(z[s0 + i]) / mean - 1.0));   // percent depth
    }
    const double audioHi = o.kind == Kind::WFM ? 15000.0 : 3000.0;
    std::vector<float> aud = fs > 2.4 * audioHi ? lowpass(dem, fs, audioHi) : std::vector<float>();
    std::vector<float> audHp = aud;
    if (o.kind != Kind::WFM && !aud.empty()) audHp = highpassBox(aud, std::max<size_t>(4, (size_t)(fs / 250.0)));
    if (!audHp.empty()) {
        f.devRmsHz = rmsOf(audHp);
        std::vector<double> a;
        a.reserve(audHp.size());
        for (float v : audHp) a.push_back(std::fabs(v));
        f.devP95Hz = pct(a, 95);
    }
    const double cthr = o.carrierOnlyDevHz > 0 ? o.carrierOnlyDevHz : kindDevDefault(o.kind);
    f.carrierOnly = f.snrDb >= 6 && !audHp.empty() && f.devRmsHz < cthr;
    const bool modulated = !f.carrierOnly && f.snrDb >= 12 && !audHp.empty();
    if (modulated) {
        double t;
        const bool ok = o.kind == Kind::WFM ? fs > 30000 && tiltOf(dem, fs, 2000, 12000, t) : tiltOf(dem, fs, 400, 2800, t);
        if (ok && f.snrDb >= 15) { f.tiltDbOct = t; f.tiltValid = true; }
    }
    // hum
    if (!aud.empty() && aud.size() >= (size_t)(fs * 0.4)) {
        static const double hz[4] = {50, 60, 100, 120};
        static const double nb_[6] = {-23, -17, -11, 11, 17, 23};
        for (int k = 0; k < 4; k++) {
            const double a = toneAmp(aud, fs, hz[k]);
            std::vector<double> ref;
            for (double d : nb_) {
                const double fr = hz[k] + d;
                bool near = fr < 10;
                for (double h : hz) if (std::fabs(fr - h) < 5) near = true;
                if (!near) ref.push_back(toneAmp(aud, fs, fr));
            }
            const double r = pct(ref, 50);
            const double minAbs = fm ? 0.5 : 0.05;
            if (!ref.empty() && a > 4 * r && a > minAbs) f.hum[k] = a;
        }
    }
    // WFM pilot
    if (o.kind == Kind::WFM && fs > 60000 && dem.size() > (size_t)(fs * 0.3)) {
        const size_t B = (size_t)(fs / 100);
        const size_t K = dem.size() / B;
        std::vector<std::complex<double>> c19(K), c21(K);
        for (size_t k = 0; k < K; k++) {
            std::complex<double> a(0, 0), b(0, 0);
            const double w1 = 2 * kPi * 19000.0 / fs, w2 = 2 * kPi * 21000.0 / fs;
            const std::complex<double> r1 = std::polar(1.0, -w1), r2 = std::polar(1.0, -w2);
            std::complex<double> p1 = std::polar(1.0, -w1 * (double)(k * B)), p2 = std::polar(1.0, -w2 * (double)(k * B));
            for (size_t i = 0; i < B; i++) {
                a += (double)dem[k * B + i] * p1; b += (double)dem[k * B + i] * p2;
                p1 *= r1; p2 *= r2;
            }
            c19[k] = a; c21[k] = b;
        }
        double m19 = 0, m21 = 0;
        for (size_t k = 0; k < K; k++) { m19 += std::abs(c19[k]); m21 += std::abs(c21[k]); }
        m19 = m19 / (double)K * 2.0 / (double)B;
        m21 = m21 / (double)K * 2.0 / (double)B;
        if (m19 > 150.0 && m19 > 4 * m21) {
            std::complex<double> r(0, 0);
            for (size_t k = 0; k + 1 < K; k++) r += c19[k + 1] * std::conj(c19[k]);
            const double dph = std::arg(r);
            f.pilot = true;
            f.pilotDevHz = m19;
            f.pilotErrHz = dph / (2 * kPi) * fs / (double)B;
            // jitter: residual of the unwrapped phase around its line
            std::vector<double> ph(K);
            double acc = 0;
            for (size_t k = 0; k < K; k++) {
                const double a = std::arg(c19[k] * std::polar(1.0, -dph * (double)k));
                ph[k] = a;
            }
            // remove the mean (circular) then rms
            std::complex<double> mc(0, 0);
            for (size_t k = 0; k < K; k++) mc += std::polar(1.0, ph[k]);
            const double m = std::arg(mc);
            for (size_t k = 0; k < K; k++) { double d = ph[k] - m; d = std::remainder(d, 2 * kPi); acc += d * d; }
            f.pilotJitterDeg = std::sqrt(acc / (double)K) * 180.0 / kPi;
        }
    }

    // 5. transient
    if (keyed && onBlock * W >= (size_t)(fs * 0.003) && f.snrDb >= 12) {
        const size_t sw = std::max<size_t>(1, (size_t)(fs * 50e-6));
        const size_t from = onBlock * W > (size_t)(fs * 0.003) ? onBlock * W - (size_t)(fs * 0.003) : 0;
        const size_t to = std::min(n, s0 + (size_t)(fs * 0.002));
        std::vector<double> amp(to - from);
        {
            double acc = 0;
            std::vector<double> ab(to - from + sw, 0.0);
            for (size_t i = 0; i < to - from + sw && from + i < n; i++) ab[i] = std::abs(z[from + i]);
            for (size_t i = 0; i < sw; i++) acc += ab[i];
            for (size_t i = 0; i < amp.size(); i++) { amp[i] = acc / (double)sw; acc += ab[i + sw] - ab[i]; }
        }
        std::vector<double> sa;
        for (size_t i = s0; i < e1; i++) sa.push_back(std::abs(z[i]));
        const double A = pct(sa, 50);
        double var = 0;
        for (double v : sa) var += (v - A) * (v - A);
        const double sigA = std::sqrt(var / (double)sa.size());
        size_t i50 = amp.size();
        for (size_t i = 0; i < amp.size(); i++) if (amp[i] >= 0.5 * A) { i50 = i; break; }
        if (i50 < amp.size() && i50 > 2) {
            size_t i10 = i50;
            while (i10 > 0 && amp[i10 - 1] >= 0.1 * A) i10--;
            size_t i90 = i50;
            while (i90 < amp.size() && amp[i90] < 0.9 * A) i90++;
            if (i10 > 0 && i90 < amp.size()) {
                f.riseUs = (double)(i90 - i10) / fs * 1e6;
                double pk = 0;
                const size_t pe = std::min(n - from, i50 + (size_t)(fs * 0.05));
                // amp only covers up to s0+2ms: extend with a running mean on the fly
                for (size_t i = i50; i < amp.size(); i++) pk = std::max(pk, amp[i]);
                for (size_t i = to; i < from + pe && i + sw < n; i++) {
                    double a = 0;
                    for (size_t k = 0; k < sw; k++) a += std::abs(z[i + k]);
                    pk = std::max(pk, a / (double)sw);
                }
                f.overshootPct = std::max(0.0, (pk - A) / A * 100.0 - 3 * sigA / A * 100.0);
                // frequency right after the carrier reached its level, in 1 ms windows
                t50 = from + i50;
                const size_t t90 = from + i90;
                const size_t win = std::max<size_t>(4, (size_t)(fs * 0.001));
                const double Fs = kay(z, s0, e1, fs);
                std::vector<double> dev;
                for (size_t k = 0; k < 220 && t90 + (k + 1) * win < e1; k++) dev.push_back(kay(z, t90 + k * win, t90 + (k + 1) * win, fs) - Fs);
                // spread of 1 ms estimates in the steady part
                std::vector<double> st;
                for (size_t a = s0; a + win < e1 && st.size() < 100; a += win) st.push_back(kay(z, a, a + win, fs) - Fs);
                double sd = 0;
                for (double v : st) sd += v * v;
                sd = st.empty() ? 0 : std::sqrt(sd / (double)st.size());
                if (!dev.empty()) {
                    f.chirpHz = dev[0];
                    const double tol = std::max(3 * sd, 2.0);
                    size_t last = 0;
                    for (size_t k = 0; k < dev.size(); k++) if (std::fabs(dev[k]) > tol) last = k + 1;
                    f.settleMs = (double)last;
                    f.transient = true;
                }
            }
        }
    }
    f.valid = true;
    return f;
}

// ---------------------------------------------------------------- WidePsd

WidePsd::WidePsd(double rate, double memorySec) : rate_(rate) {
    win_.resize(kN);
    for (int i = 0; i < kN; i++) win_[(size_t)i] = 0.5f - 0.5f * std::cos(2 * 3.14159265f * i / kN);
    p_.assign(kN, 0.0);
    const double segT = kN / rate;
    alpha_ = std::exp(-segT / std::max(memorySec, segT));
    skipSeg_ = (size_t)std::max(1.0, std::floor(rate / kN / 150.0));
}

void WidePsd::add(const cf32* x, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (discard_ > 0) { const size_t d = std::min(discard_, n - i); discard_ -= d; i += d; continue; }
        const size_t take = std::min((size_t)kN - acc_.size(), n - i);
        acc_.insert(acc_.end(), x + i, x + i + take);
        i += take;
        if (acc_.size() == (size_t)kN) {
            double wp = 0;
            for (int k = 0; k < kN; k++) { acc_[(size_t)k] *= win_[(size_t)k]; wp += (double)win_[(size_t)k] * win_[(size_t)k]; }
            fft_.forward(acc_.data());
            const double a = segs_ == 0 ? 0.0 : alpha_;
            for (int k = 0; k < kN; k++) {
                const double v = std::norm(acc_[(size_t)k]) / wp;
                double& d = p_[(size_t)((k + kN / 2) % kN)];
                d = a * d + (1 - a) * v;
            }
            segs_++;
            acc_.clear();
            discard_ = (skipSeg_ - 1) * (size_t)kN;
        }
    }
}

void WidePsd::spurs(double carrierOffsetHz, double occBwHz, Features& f) const {
    f.spurs.clear(); f.imageDbc = 99; f.spurValid = false; f.rxOffsetHz = carrierOffsetHz;
    if (segs_ == 0) return;
    const double bh = binHz();
    const int kc = (int)std::lround(carrierOffsetHz / bh) + kN / 2;
    const int occ = std::max(2, (int)std::ceil(occBwHz / 2 / bh));
    if (kc - occ < 2 || kc + occ >= kN - 2) return;
    double pc = 0;
    for (int k = kc - occ; k <= kc + occ; k++) pc += p_[(size_t)k];
    if (pc <= 0) return;
    // local floor per 64-bin block (median), skipping the carrier's own band
    const int BL = 64, nbk = kN / BL;
    std::vector<double> fl((size_t)nbk, 0.0);
    for (int b = 0; b < nbk; b++) {
        std::vector<double> v;
        for (int k = b * BL; k < (b + 1) * BL; k++) if (k < kc - occ - 4 || k > kc + occ + 4) v.push_back(p_[(size_t)k]);
        fl[(size_t)b] = v.empty() ? 0 : pct(v, 50);
    }
    auto floorAt = [&](int k) { return fl[(size_t)std::clamp(k / BL, 0, nbk - 1)]; };
    auto sum7 = [&](int k) { double s = 0; for (int j = -3; j <= 3; j++) s += p_[(size_t)std::clamp(k + j, 0, kN - 1)]; return s - 7 * floorAt(k); };
    const int kimg = kN - kc;
    f.imageDbc = 99;
    if (std::abs(kimg - kc) > occ + 8) {
        int bestk = kimg;
        for (int j = -2; j <= 2; j++) if (p_[(size_t)std::clamp(kimg + j, 0, kN - 1)] > p_[(size_t)bestk]) bestk = kimg + j;
        const double s = sum7(bestk);
        if (s > 7 * floorAt(bestk) * 3.0) f.imageDbc = 10 * std::log10(s / pc);
    }
    struct C { int k; double dbc; };
    std::vector<C> cand;
    for (int k = 4; k < kN - 4; k++) {
        if (std::abs(k - kc) <= occ + 4) continue;
        if (std::abs(k - kN / 2) <= 4) continue;        // the receiver's DC spike
        if (std::abs(k - kimg) <= 3) continue;
        bool pk = true;
        for (int j = -3; j <= 3; j++) if (p_[(size_t)(k + j)] > p_[(size_t)k]) { pk = false; break; }
        if (!pk || p_[(size_t)k] < floorAt(k) * 15.8) continue;   // 12 dB above the local floor
        const double s = sum7(k);
        if (s <= 0) continue;
        const double dbc = 10 * std::log10(s / pc);
        if (dbc > -85) cand.push_back({k, dbc});
    }
    std::sort(cand.begin(), cand.end(), [](const C& a, const C& b) { return a.dbc > b.dbc; });
    for (size_t i = 0; i < cand.size() && f.spurs.size() < 6; i++) {
        const int k = cand[i].k;
        const double l0 = std::log(p_[(size_t)k - 1] + 1e-30), l1 = std::log(p_[(size_t)k] + 1e-30), l2 = std::log(p_[(size_t)k + 1] + 1e-30);
        const double den = l0 - 2 * l1 + l2;
        const double dk = std::fabs(den) > 1e-9 ? std::clamp(0.5 * (l0 - l2) / den, -0.5, 0.5) : 0.0;
        f.spurs.push_back({((double)(k - kc) + dk) * bh, cand[i].dbc});
    }
    f.spurValid = true;
}

// ---------------------------------------------------------------- compare

namespace {
double absHz(const Features& a) { return (a.tunedHz + a.cfoHz) / (1.0 + a.rxPpm * 1e-6); }
double cdiff(double a, double b) { return std::fabs(a - b); }
}  // namespace

Match compare(const Features& a, const Features& b, const MatchOptions& mo) {
    Match m;
    if (!a.valid || !b.valid || a.kind != b.kind) return m;
    double num = 0, den = 0;
    auto add = [&](const char* name, double diff, double tol, double w) {
        const double d = std::fabs(diff) / tol;
        m.parts.push_back({name, d, w});
        num += w * d * d; den += w;
    };
    add("carrier offset", absHz(a) - absHz(b), std::max(mo.cfoTolHz, 3 * std::hypot(a.cfoStdHz, b.cfoStdHz)), 3.0);
    if (a.pilot && b.pilot) {
        add("pilot frequency", a.pilotErrHz - b.pilotErrHz, std::max(0.05, 3 * 0.01), 3.0);
        add("pilot level", a.pilotDevHz - b.pilotDevHz, 0.04 * std::max(a.pilotDevHz, b.pilotDevHz) + 30, 1.5);
        add("pilot jitter", a.pilotJitterDeg - b.pilotJitterDeg, 1.5 + 0.3 * std::max(a.pilotJitterDeg, b.pilotJitterDeg), 1.0);
    }
    if (a.carrierOnly && b.carrierOnly && std::min(a.durationS, b.durationS) > 0.3)
        add("warm-up drift", a.driftHzPerS - b.driftHzPerS, 0.5 + 0.3 * std::max(std::fabs(a.driftHzPerS), std::fabs(b.driftHzPerS)), 1.5);
    if (!a.carrierOnly && !b.carrierOnly) {
        add("deviation", a.devP95Hz - b.devP95Hz, 0.5 * std::max(a.devP95Hz, b.devP95Hz) + 1.0, 0.5);
        if (a.tiltValid && b.tiltValid) add("pre-emphasis", a.tiltDbOct - b.tiltDbOct, 3.0, 0.5);
    }
    for (int k = 0; k < 4; k++) {
        if (a.hum[k] <= 0 && b.hum[k] <= 0) continue;
        const double fl = a.kind == Kind::AM ? 0.05 : 0.5;
        const double da = 20 * std::log10(std::max(a.hum[k], fl) / fl), db = 20 * std::log10(std::max(b.hum[k], fl) / fl);
        add("mains hum", da - db, 6.0, 0.7);
    }
    if (a.transient && b.transient) {
        add("rise time", std::log(std::max(a.riseUs, 1.0) / std::max(b.riseUs, 1.0)), 0.35, 2.0);
        add("overshoot", a.overshootPct - b.overshootPct, 4.0, 1.0);
        add("start-up chirp", a.chirpHz - b.chirpHz, 25 + 0.25 * std::max(std::fabs(a.chirpHz), std::fabs(b.chirpHz)), 1.5);
        add("settling", a.settleMs - b.settleMs, 1.0 + 0.35 * std::max(a.settleMs, b.settleMs), 1.5);
    }
    if (a.spurValid && b.spurValid) {
        const double tolHz = 400.0;
        double sd = 0; int cnt = 0;
        auto scan = [&](const Features& p, const Features& q) {
            for (const Spur& s : p.spurs) {
                if (s.dbc < -60) continue;
                const Spur* hit = nullptr;
                for (const Spur& t : q.spurs) if (cdiff(s.offHz, t.offHz) < tolHz) { hit = &t; break; }
                sd += hit ? std::pow(std::fabs(s.dbc - hit->dbc) / 4.0, 2) : 9.0;
                cnt++;
            }
        };
        scan(a, b); scan(b, a);
        if (cnt > 0) { m.parts.push_back({"spurs", std::sqrt(sd / cnt), 2.0}); num += 2.0 * sd / cnt; den += 2.0; }
        if (a.imageDbc < 90 && b.imageDbc < 90 && cdiff(a.rxOffsetHz, b.rxOffsetHz) < 25000) add("I/Q image", a.imageDbc - b.imageDbc, 3.0, 0.7);
    }
    m.weight = den;
    m.score = 100.0 * std::exp(-0.5 * num / std::max(den, 1e-9));
    return m;
}

// ---------------------------------------------------------------- text

std::string featuresToText(const Features& f) {
    char b[1600];
    std::string s;
    auto kv = [&](const char* k, double v) { std::snprintf(b, sizeof b, "%s=%.10g;", k, v); s += b; };
    kv("kind", (int)f.kind); kv("tuned", f.tunedHz); kv("rate", f.rate); kv("dur", f.durationS);
    kv("cfo", f.cfoHz); kv("cfostd", f.cfoStdHz); kv("drift", f.driftHzPerS); kv("snr", f.snrDb); kv("ppm", f.rxPpm);
    kv("conly", f.carrierOnly); kv("devrms", f.devRmsHz); kv("devp95", f.devP95Hz); kv("tiltv", f.tiltValid); kv("tilt", f.tiltDbOct);
    for (int i = 0; i < 4; i++) { std::snprintf(b, sizeof b, "hum%d=%.10g;", i, f.hum[i]); s += b; }
    kv("pilot", f.pilot); kv("pdev", f.pilotDevHz); kv("perr", f.pilotErrHz); kv("pjit", f.pilotJitterDeg);
    kv("trans", f.transient); kv("rise", f.riseUs); kv("over", f.overshootPct); kv("chirp", f.chirpHz); kv("settle", f.settleMs);
    kv("spurv", f.spurValid); kv("rxoff", f.rxOffsetHz); kv("image", f.imageDbc);
    s += "spurs=";
    for (size_t i = 0; i < f.spurs.size(); i++) { std::snprintf(b, sizeof b, "%s%.6g:%.4g", i ? "," : "", f.spurs[i].offHz, f.spurs[i].dbc); s += b; }
    s += ";";
    return s;
}

bool featuresFromText(const std::string& s, Features& f) {
    f = Features();
    size_t p = 0;
    int got = 0;
    while (p < s.size()) {
        const size_t e = s.find(';', p);
        if (e == std::string::npos) break;
        const std::string kvs = s.substr(p, e - p);
        p = e + 1;
        const size_t q = kvs.find('=');
        if (q == std::string::npos) continue;
        const std::string k = kvs.substr(0, q), v = kvs.substr(q + 1);
        const double d = std::atof(v.c_str());
        got++;
        if (k == "kind") f.kind = (Kind)std::clamp((int)d, 0, 2);
        else if (k == "tuned") f.tunedHz = d; else if (k == "rate") f.rate = d; else if (k == "dur") f.durationS = d;
        else if (k == "cfo") f.cfoHz = d; else if (k == "cfostd") f.cfoStdHz = d; else if (k == "drift") f.driftHzPerS = d;
        else if (k == "snr") f.snrDb = d; else if (k == "ppm") f.rxPpm = d; else if (k == "conly") f.carrierOnly = d != 0;
        else if (k == "devrms") f.devRmsHz = d; else if (k == "devp95") f.devP95Hz = d; else if (k == "tiltv") f.tiltValid = d != 0;
        else if (k == "tilt") f.tiltDbOct = d;
        else if (k.size() == 4 && k.compare(0, 3, "hum") == 0 && k[3] >= '0' && k[3] <= '3') f.hum[k[3] - '0'] = d;
        else if (k == "pilot") f.pilot = d != 0; else if (k == "pdev") f.pilotDevHz = d; else if (k == "perr") f.pilotErrHz = d;
        else if (k == "pjit") f.pilotJitterDeg = d; else if (k == "trans") f.transient = d != 0; else if (k == "rise") f.riseUs = d;
        else if (k == "over") f.overshootPct = d; else if (k == "chirp") f.chirpHz = d; else if (k == "settle") f.settleMs = d;
        else if (k == "spurv") f.spurValid = d != 0; else if (k == "rxoff") f.rxOffsetHz = d; else if (k == "image") f.imageDbc = d;
        else if (k == "spurs") {
            size_t a = 0;
            while (a < v.size()) {
                size_t c = v.find(',', a);
                if (c == std::string::npos) c = v.size();
                const std::string it = v.substr(a, c - a);
                const size_t col = it.find(':');
                if (col != std::string::npos) f.spurs.push_back({std::atof(it.substr(0, col).c_str()), std::atof(it.substr(col + 1).c_str())});
                a = c + 1;
            }
        }
    }
    f.valid = got > 10;
    return f.valid;
}

// ---------------------------------------------------------------- Library

namespace {
std::string clean(std::string s) {
    for (char& c : s) if (c == '|' || c == '\n' || c == '\r' || c == '\t') c = ' ';
    return s;
}
}  // namespace

void Library::add(const std::string& name, const Features& f, const std::string& note) {
    Entry e;
    e.name = clean(name); e.note = clean(note); e.f = f;
    e.created = (int64_t)std::time(nullptr);
    for (auto& it : items) if (it.name == e.name) { e.created = it.created; it = e; return; }   // the same name updates it
    items.push_back(e);
}

bool Library::remove(size_t i) {
    if (i >= items.size()) return false;
    items.erase(items.begin() + (long)i);
    return true;
}

std::vector<std::pair<size_t, Match>> Library::rank(const Features& f, const MatchOptions& mo) const {
    std::vector<std::pair<size_t, Match>> r;
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].f.kind != f.kind) continue;
        r.emplace_back(i, compare(f, items[i].f, mo));
    }
    std::sort(r.begin(), r.end(), [](auto& x, auto& y) { return x.second.score > y.second.score; });
    return r;
}

std::string Library::toText() const {
    std::string s = "# OnAir RF fingerprints v1\n";
    for (const Entry& e : items) s += e.name + "|" + e.note + "|" + std::to_string(e.created) + "|" + featuresToText(e.f) + "\n";
    return s;
}

bool Library::fromText(const std::string& s) {
    items.clear();
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> p;
        size_t a = 0;
        for (int k = 0; k < 3; k++) {
            const size_t c = line.find('|', a);
            if (c == std::string::npos) break;
            p.push_back(line.substr(a, c - a));
            a = c + 1;
        }
        if (p.size() < 3) continue;
        Entry e;
        e.name = p[0]; e.note = p[1]; e.created = std::atoll(p[2].c_str());
        if (featuresFromText(line.substr(a), e.f)) items.push_back(e);
    }
    return true;
}

bool Library::save(const std::string& path) const {
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << toText();
    return (bool)o;
}

bool Library::load(const std::string& path) {
    std::ifstream i(path, std::ios::binary);
    if (!i) return false;
    std::stringstream ss;
    ss << i.rdbuf();
    return fromText(ss.str());
}

// ---------------------------------------------------------------- Watcher

Watcher::Watcher(double inRate, double off, Kind kind, double tuned, double bw)
    : ch_(inRate, off, kind, bw), psd_(inRate), kind_(kind), tuned_(tuned), off_(off), bw_(bw) {
    const double fs = ch_.outRate();
    blockN_ = std::max<size_t>(8, (size_t)(fs * 0.02));
    ring_.assign((size_t)(fs * 4.0), cf32(0, 0));
}

void Watcher::feed(const cf32* x, size_t n, int64_t wallNow) {
    psd_.add(x, n);
    tmp_.clear();
    ch_.push(x, n, tmp_);
    const double fs = ch_.outRate();
    for (const cf32& s : tmp_) {
        ring_[ringPos_] = s;
        ringPos_ = (ringPos_ + 1) % ring_.size();
        if (ringFill_ < ring_.size()) ringFill_++;
        if (on_ && cur_.size() < (size_t)(fs * 6.0)) cur_.push_back(s);
        blockPow_ += std::norm(s);
        if (havePrev_) lagAcc_ += std::complex<float>(s * std::conj(prev_));
        prev_ = s; havePrev_ = true;
        if (++blockFill_ < blockN_) continue;
        // end of a block
        const double P = blockPow_ / (double)blockN_;
        blockFill_ = 0; blockPow_ = 0;
        t_ += (double)blockN_ / fs;
        noiseMin_ = std::min(noiseMin_, P);
        if (t_ - noiseWinT_ >= 1.0) {
            noiseWinT_ = t_;
            noiseHist_.push_back(noiseMin_);
            if (noiseHist_.size() > 10) noiseHist_.erase(noiseHist_.begin());
            noiseMin_ = 1e30;
        }
        noise_ = noiseMin_;
        for (double v : noiseHist_) noise_ = std::min(noise_, v);
        noise_ = std::max(noise_, 1e-14);
        const double hiThr = noise_ * std::pow(10.0, onDb_ / 10), loThr = noise_ * std::pow(10.0, (onDb_ - 4) / 10);
        live_.levelDb = 10 * std::log10(std::max(P, 1e-14));
        live_.snrDb = 10 * std::log10(std::max(P / noise_ - 1.0, 1e-6));
        live_.cfoHz = std::arg(lagAcc_) * fs / (2 * kPi);
        live_.devHz = 0;
        lagAcc_ = 0;
        if (!on_) {
            if (P > hiThr) {
                if (++onRun_ >= 2) {
                    on_ = true; offRun_ = 0;
                    tOn_ = t_ - 2.0 * (double)blockN_ / fs;
                    wallOn_ = wallNow;
                    const size_t pre = std::min(ringFill_, (size_t)(fs * 0.12) + 2 * blockN_);
                    cur_.clear();
                    for (size_t i = 0; i < pre; i++) cur_.push_back(ring_[(ringPos_ + ring_.size() - pre + i) % ring_.size()]);
                }
            } else onRun_ = 0;
        } else {
            if (P < loThr) {
                if (++offRun_ >= 3) finish(t_ - 3.0 * (double)blockN_ / fs);
            } else offRun_ = 0;
        }
        live_.carrier = on_;
    }
}

void Watcher::finish(double tEnd) {
    Pending p;
    p.ev.tStart = tOn_;
    p.ev.durationS = std::max(0.0, tEnd - tOn_);
    p.ev.wallStart = wallOn_;
    p.z.swap(cur_);
    p.psd = psd_;
    p.noise = noise_;
    pending_.push_back(std::move(p));
    cur_.clear();
    on_ = false; onRun_ = 0; offRun_ = 0;
}

std::vector<Event> Watcher::poll() {
    std::vector<Event> out;
    for (Pending& p : pending_) {
        Options o;
        o.kind = kind_; o.tunedHz = tuned_; o.noiseFloorPow = p.noise;
        // trim the quiet tail the off-detector needed
        Event e = p.ev;
        e.f = analyse(p.z.data(), p.z.size(), ch_.outRate(), o);
        e.partial = e.durationS < 0.15 || !e.f.valid;
        if (e.f.valid && e.durationS >= 0.6 && p.psd.ready()) p.psd.spurs(off_, bw_ > 0 ? bw_ : (kind_ == Kind::WFM ? 200e3 : kind_ == Kind::NFM ? 25e3 : 10e3), e.f);
        out.push_back(std::move(e));
    }
    pending_.clear();
    return out;
}

std::vector<cf32> Watcher::recent(double sec) const {
    const size_t n = std::min(ringFill_, (size_t)(sec * ch_.outRate()));
    std::vector<cf32> r(n);
    for (size_t i = 0; i < n; i++) r[i] = ring_[(ringPos_ + ring_.size() - n + i) % ring_.size()];
    return r;
}

Features Watcher::captureNow(double sec) const {
    const std::vector<cf32> z = recent(sec);
    Options o;
    o.kind = kind_; o.tunedHz = tuned_;
    Features f = analyse(z.data(), z.size(), ch_.outRate(), o);
    if (f.valid && psd_.ready()) psd_.spurs(off_, bw_ > 0 ? bw_ : (kind_ == Kind::WFM ? 200e3 : kind_ == Kind::NFM ? 25e3 : 10e3), f);
    return f;
}

}  // namespace dect2::fp

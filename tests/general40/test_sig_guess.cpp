// Real modulated IQ -> Welch PSD -> measure() -> guess(). With frequency offset, noise and a span that is not centred on the signal.
#include "dect2/general/sig_guess.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace dect2::sigguess;
using cf = std::complex<double>;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); fails++; } }
static void fft(std::vector<cf>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) { const double ang = -2 * M_PI / (double)len; cf wl(std::cos(ang), std::sin(ang)); for (size_t i = 0; i < n; i += len) { cf w(1); for (size_t j = 0; j < len / 2; j++) { cf u = a[i + j], v = a[i + j + len / 2] * w; a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl; } } }
}
static const double FS = 240000;
static std::vector<float> psd(const std::vector<cf>& x, size_t nfft = 2048) {
    std::vector<double> acc(nfft, 0);
    int segs = 0;
    for (size_t s = 0; s + nfft <= x.size(); s += nfft / 2) {
        std::vector<cf> a(nfft);
        for (size_t i = 0; i < nfft; i++) a[i] = x[s + i] * (0.5 - 0.5 * std::cos(2 * M_PI * (double)i / (double)nfft));
        fft(a);
        for (size_t i = 0; i < nfft; i++) acc[(i + nfft / 2) % nfft] += std::norm(a[i]);
        segs++;
    }
    std::vector<float> db(nfft);
    for (size_t i = 0; i < nfft; i++) db[i] = (float)(10 * std::log10(acc[i] / segs + 1e-30));
    return db;
}
// voice-like audio: band-limited noise 300-2800 Hz with a syllable envelope
static std::vector<double> voice(size_t n, std::mt19937& g, double fs) {
    std::normal_distribution<double> nd(0, 1);
    std::vector<double> y(n);
    double lp1 = 0, lp2 = 0, hp = 0;
    for (size_t i = 0; i < n; i++) {
        const double w = nd(g);
        lp1 += 0.25 * (w - lp1); lp2 += 0.25 * (lp1 - lp2);
        const double hpv = lp2 - hp; hp += 0.04 * (lp2 - hp);
        y[i] = hpv * (0.6 + 0.4 * std::sin(2 * M_PI * 4.0 * (double)i / fs));
    }
    double mx = 0; for (double v : y) mx = std::max(mx, std::fabs(v));
    for (double& v : y) v /= mx;
    return y;
}
static void addNoise(std::vector<cf>& x, double sigma, std::mt19937& g) { std::normal_distribution<double> nd(0, sigma); for (auto& v : x) v += cf(nd(g), nd(g)); }
static void offset(std::vector<cf>& x, double hz) { for (size_t i = 0; i < x.size(); i++) x[i] *= std::polar(1.0, 2 * M_PI * hz * (double)i / FS); }
static void dump(const char* n, const Features& f, const std::vector<Guess>& g) { if (!getenv("DBG")) return; std::printf("%s: obw %.0f cen %.0f pkoff %.0f pkover %.1f sym %.2f flat %.1f humps %d snr %.1f ->", n, f.obwHz, f.centreOffHz, f.peakOffHz, f.peakOverDb, f.symmetry, f.flatnessDb, f.humps, f.snrDb); for (auto& x : g) std::printf(" %s(%.2f)", x.what.c_str(), x.confidence); std::printf("\n"); }
static bool top(const std::vector<Guess>& g, const char* what) { return !g.empty() && g[0].what == what; }
static bool has(const std::vector<Guess>& g, const char* what) { for (auto& x : g) if (x.what == what) return true; return false; }
int main() {
    const size_t N = 1 << 17;
    for (unsigned seed = 1; seed <= 4; seed++) {
        std::mt19937 g(seed);
        const double off = (seed % 2 ? 1 : -1) * (5000.0 * seed);   // off-centre
        auto au = voice(N, g, FS);
        // AM, 80 % modulation
        std::vector<cf> am(N);
        for (size_t i = 0; i < N; i++) am[i] = cf(1.0 + 0.8 * au[i], 0);
        offset(am, off); addNoise(am, 0.02, g);
        auto fm = measure(psd(am), FS / 2048);
        auto gs = guess(fm); dump("AM", fm, gs);
        check(fm.ok && fm.obwHz > 3000 && fm.obwHz < 16000, "AM bandwidth measured");
        check(top(gs, "AM"), "AM identified");
        // NFM, 3 kHz deviation (Carson ~ 11 kHz)
        std::vector<cf> nf(N);
        double ph = 0;
        for (size_t i = 0; i < N; i++) { ph += 2 * M_PI * 15000.0 * au[i] / FS; nf[i] = std::polar(1.0, ph); }
        offset(nf, off); addNoise(nf, 0.02, g);
        auto fn = measure(psd(nf), FS / 2048);
        gs = guess(fn); dump("NFM", fn, gs);
        check(top(gs, "NFM"), "NFM identified");
        check(!has(gs, "AM"), "NFM is not offered as AM");
        // USB: voice shifted up by 300 Hz guard, by Hilbert-like method: analytic signal via FFT-free approach: build from two-quadrature using phase-shifted sum of tones
        std::vector<cf> us(N);
        {
            // sum of 40 voice-band tones with random slowly varying amplitudes: USB means the positive frequencies only
            std::uniform_real_distribution<double> ud(0, 6.28);
            std::vector<double> f0, p0;
            for (int k = 0; k < 40; k++) { f0.push_back(300 + 62.5 * k); p0.push_back(ud(g)); }
            for (size_t i = 0; i < N; i++) {
                cf s(0);
                const double env = 0.7 + 0.3 * std::sin(2 * M_PI * 4.0 * (double)i / FS);
                for (int k = 0; k < 40; k++) s += std::polar(env * (1.0 / (1 + k * 0.05)), 2 * M_PI * f0[(size_t)k] * (double)i / FS + p0[(size_t)k]);
                us[i] = s / 12.0;
            }
        }
        offset(us, off); addNoise(us, 0.02, g);
        auto fu = measure(psd(us), FS / 2048);
        gs = guess(fu); dump("USB", fu, gs);
        check(has(gs, "SSB") && !has(gs, "AM"), "SSB offered, not AM");
        for (auto& x : gs) if (x.what == "SSB") check(x.mode == "USB" || x.mode == "LSB", "SSB names a sideband");
        // CW carrier
        std::vector<cf> cw(N, cf(1, 0));
        offset(cw, off); addNoise(cw, 0.05, g);
        { auto m = measure(psd(cw), FS / 2048); gs = guess(m); dump("CW", m, gs); }
        check(top(gs, "CW / carrier"), "CW carrier identified");
        // FSK 2 tones at +-2.4 kHz, 1200 baud
        std::vector<cf> fk(N);
        ph = 0;
        for (size_t i = 0; i < N; i++) { const int bit = (int)((double)i / (FS / 1200.0)) * 7 % 3 != 0; ph += 2 * M_PI * (bit ? 2400.0 : -2400.0) / FS; fk[i] = std::polar(1.0, ph); }
        offset(fk, off); addNoise(fk, 0.02, g);
        { auto m = measure(psd(fk), FS / 2048); gs = guess(m); dump("FSK", m, gs); }
        check(has(gs, "FSK / two tones"), "FSK offered");
        // noise only: nothing
        std::vector<cf> nz(N);
        addNoise(nz, 1.0, g);
        { auto m = measure(psd(nz), FS / 2048); gs = guess(m); dump("NOISE", m, gs); }
        check(gs.empty(), "noise only gives no guess (false-guess rate)");
    }
    // wide FM: 75 kHz deviation tone
    {
        std::mt19937 g(9);
        std::vector<cf> w(N);
        double ph = 0;
        for (size_t i = 0; i < N; i++) { const double m = std::sin(2 * M_PI * 1000.0 * (double)i / 480000.0) * 0.5 + std::sin(2 * M_PI * 7000.0 * (double)i / 480000.0) * 0.5; ph += 2 * M_PI * 75000.0 * m / 480000.0; w[i] = std::polar(1.0, ph); }
        // 480 kHz rate: measure at 480 kHz bins
        auto db = [&] { std::vector<double> acc(2048, 0); int s = 0; for (size_t o = 0; o + 2048 <= w.size(); o += 1024) { std::vector<cf> a(2048); for (size_t i = 0; i < 2048; i++) a[i] = w[o + i] * (0.5 - 0.5 * std::cos(2 * M_PI * (double)i / 2048)); fft(a); for (size_t i = 0; i < 2048; i++) acc[(i + 1024) % 2048] += std::norm(a[i]); s++; } std::vector<float> d(2048); for (size_t i = 0; i < 2048; i++) d[i] = (float)(10 * std::log10(acc[i] / s + 1e-30)); return d; }();
        auto gs = guess(measure(db, 480000.0 / 2048));
        check(has(gs, "WFM"), "wide FM identified");
    }
    // flat OFDM-like: random QPSK on 1024 subcarriers across 120 kHz of a 240 kHz span
    {
        std::mt19937 g(5);
        std::normal_distribution<double> nd(0, 1);
        std::vector<cf> x(N, cf(0));
        const size_t nfft = 1024;
        for (size_t s = 0; s + nfft <= N; s += nfft) {
            std::vector<cf> a(nfft, cf(0));
            for (size_t k = 0; k < 512; k++) { const size_t bin = (k < 256) ? k : nfft - (512 - k); a[bin] = cf(nd(g), nd(g)); }
            fft(a);   // forward FFT of the spectrum = time signal (conjugate symmetric mapping not needed)
            for (size_t i = 0; i < nfft; i++) x[s + i] = a[i] / 20.0;
        }
        addNoise(x, 0.02, g);
        auto gs = guess(measure(psd(x), FS / 2048));
        check(has(gs, "OFDM / wideband digital"), "flat OFDM identified");
        check(!has(gs, "AM") && !has(gs, "NFM"), "OFDM is not offered as AM / NFM");
    }
    check(guess(measure({}, 100)).empty() && guess(measure(std::vector<float>(100, -50.f), 0)).empty(), "degenerate input");
    std::printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}

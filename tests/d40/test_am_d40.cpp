#include "dect2/d40/am_d40.h"
#include <complex>
#include <cstdio>
#include <random>
using namespace dect2::d40;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// real power spectrum (dB) of an IQ block via a Hann-windowed DFT-by-FFT (radix 2), DC in the middle
static void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / (double)len; std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) { std::complex<double> w(1); for (size_t k = 0; k < len / 2; k++) { auto u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; } }
    }
}
static std::vector<float> spec(const std::vector<std::complex<double>>& x) {
    const size_t n = x.size(); std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; i++) a[i] = x[i] * (0.5 - 0.5 * std::cos(2 * M_PI * (double)i / (double)n));
    fft(a); std::vector<float> d(n);
    for (size_t i = 0; i < n; i++) d[i] = (float)(10 * std::log10(std::norm(a[(i + n / 2) % n]) + 1e-12));
    return d;
}
static std::vector<float> posHalf(const std::vector<float>& d) { return std::vector<float>(d.begin() + (long)d.size() / 2, d.end()); }
static std::vector<std::complex<double>> noise(size_t n, double sd, std::mt19937& r) {
    std::normal_distribution<double> g(0, sd); std::vector<std::complex<double>> x(n); for (auto& v : x) v = {g(r), g(r)}; return x;
}
int main() {
    std::mt19937 rng(11);
    const double fs = 48000; const size_t N = 16384; const double binHz = fs / (double)N;
    // carrier offset: carrier at 1 kHz = centre + 83.3 Hz (offset +33 Hz against 9 kHz raster 999)
    for (double off : {-120.0, -40.0, 0.0, 33.0, 90.0}) {
        auto x = noise(N, 0.05, rng);
        const double fc = 1250.0 + off;
        for (size_t i = 0; i < N; i++) x[i] += std::polar(1.0, 2 * M_PI * fc * (double)i / fs);
        auto r = carrierOffset(spec(x), binHz, 999.0 - 1.25);   // centre such that the carrier is at 999 kHz + off
        CHECK(r.ok && r.rasterKhz == 9);
        CHECK(std::fabs(r.offsetHz - off) < 10);
    }
    { auto r = carrierOffset(spec(noise(N, 1.0, rng)), binHz, 999); CHECK(!r.ok); }

    // heterodyne: audio = speech-like noise (band-limited) + 1 kHz beat
    {
        std::vector<std::complex<double>> x(N); std::normal_distribution<double> g(0, 1);
        double lp = 0;
        for (size_t i = 0; i < N; i++) { lp = 0.9 * lp + g(rng); x[i] = {lp * 0.3 + 0.8 * std::cos(2 * M_PI * 1000.4 * (double)i / fs), 0}; }
        auto t = findHeterodynes(posHalf(spec(x)), binHz);
        CHECK(!t.empty() && std::fabs(t[0].hz - 1000.4) < 3);
        // two tones
        for (size_t i = 0; i < N; i++) x[i] += 0.5 * std::cos(2 * M_PI * 4500.0 * (double)i / fs);
        CHECK(findHeterodynes(posHalf(spec(x)), binHz).size() >= 2);
        // speech-like only: no tone
        for (size_t i = 0; i < N; i++) { lp = 0.9 * lp + g(rng); x[i] = {lp, 0}; }
        CHECK(findHeterodynes(posHalf(spec(x)), binHz).empty());
    }
    // best sideband: carrier + noise, an interferer (strong tone) in the upper sideband at +3 kHz
    {
        auto x = noise(N, 0.1, rng);
        for (size_t i = 0; i < N; i++) { x[i] += std::polar(1.0, 0.0); x[i] += std::polar(0.5, 2 * M_PI * 3000.0 * (double)i / fs); }
        // add a broad hump so the dirty-level metric sees it: several tones
        for (int k = 0; k < 20; k++) for (size_t i = 0; i < N; i++) x[i] += std::polar(0.04, 2 * M_PI * (2500.0 + 45 * k) * (double)i / fs + k);
        auto r = bestSideband(spec(x), binHz);
        CHECK(r.kind == SidebandChoice::Lsb && r.advantageDb > 3);
        auto y = noise(N, 0.1, rng); for (size_t i = 0; i < N; i++) y[i] += 1.0;
        CHECK(bestSideband(spec(y), binHz).kind == SidebandChoice::Dsb);
        // mirrored
        auto z = noise(N, 0.1, rng);
        for (size_t i = 0; i < N; i++) z[i] += 1.0;
        for (int k = 0; k < 20; k++) for (size_t i = 0; i < N; i++) z[i] += std::polar(0.1, -2 * M_PI * (2500.0 + 45 * k) * (double)i / fs + k);
        CHECK(bestSideband(spec(z), binHz).kind == SidebandChoice::Usb);
    }
    // AM / DRM switch with a fading window
    {
        AmDrmSwitch sw;
        // AM: carrier + audio sidebands (noise shaped to 4.5 kHz)
        std::normal_distribution<double> g(0, 1);
        auto am = [&]() {
            std::vector<std::complex<double>> x = noise(N, 0.02, rng); double lp = 0;
            for (size_t i = 0; i < N; i++) { lp = 0.7 * lp + g(rng); x[i] += (1.0 + 0.2 * lp) * 1.0; }
            return spec(x); };
        // DRM: flat OFDM-like band +-4.5 kHz: random sum of carriers every ~ 94 Hz
        auto drm = [&]() {
            std::vector<std::complex<double>> x = noise(N, 0.02, rng);
            for (int k = -47; k <= 47; k++) { const double ph = (double)(rng() % 6283) / 1000.0; for (size_t i = 0; i < N; i++) x[i] += std::polar(0.1, 2 * M_PI * (k * 93.75) * (double)i / fs + ph); }
            return spec(x); };
        auto a = am();
        sw.feed(a, binHz); sw.feed(a, binHz); CHECK(sw.kind == AmDrmSwitch::Unknown); sw.feed(a, binHz); CHECK(sw.kind == AmDrmSwitch::Am);
        auto d = drm();
        sw.feed(d, binHz); CHECK(sw.kind == AmDrmSwitch::Am);   // one frame does not flip it
        sw.feed(d, binHz); sw.feed(d, binHz); std::printf("kind=%d\n", (int)sw.kind); CHECK(sw.kind == AmDrmSwitch::Drm);
    }
    // schedule table
    {
        ScheduleTable t;
        CHECK(t.load("# eibi\n6155,2200,0200,0,Radio X,D\n6155.5,0600,0700,12345,Radio Y,F\nbad line\n9500,2500,0100,0,bad,X\n") == 2);
        CHECK(t.lookup(6155, 3, 23 * 60).size() == 1 && t.lookup(6155, 3, 23 * 60)[0].name == "Radio X");
        CHECK(t.lookup(6155, 3, 60).size() == 1);              // past midnight
        CHECK(t.lookup(6155, 3, 3 * 60).empty());
        CHECK(t.lookup(6155, 3, 6 * 60 + 30).size() == 1);     // Radio Y, 0.5 kHz away, Wednesday
        CHECK(t.lookup(6155, 7, 6 * 60 + 30).empty());         // Sunday: not on the days list
    }
    std::printf(fails ? "FAILED\n" : "am_d40 ok\n");
    return fails ? 1 : 0;
}

// v4range: can an RTL-SDR (R828D / R820T2) receive above its rated 1.766 GHz by harmonic mixing, with no hardware change?
//
//   v4range                    default run: reference check below the limit (FM, GPS L1), then the 2.4 GHz band (Wi-Fi / Bluetooth) on the
//                              3rd and 5th harmonic, with harmonic identification. Writes v4range.csv.
//   v4range --full             the whole 1.8-3.6 GHz range on the 3rd and 5th harmonic
//   v4range --range 2200-2300  your own range (MHz); --harm 3,5  harmonics to try
//   v4range --table            print the LO / harmonic / alias table, no hardware needed
//   v4range --selftest         the identification logic on synthetic data, no hardware needed (exit code 0 = pass)
//
// How it works: the tuner mixer is driven by a square-ish LO, so it also mixes with LO x 3, x 5, x 7. The R828D PLL stops locking at about
// 1.766 GHz, but tuned to LO = (target -/+ IF) / N it puts the N-th harmonic on the target. The tool tunes the dongle to that LO through the
// normal librtlsdr call (set_center_freq = LO - IF), so the library, the V4 input switch and the tuner filters behave as for any frequency
// below 1.766 GHz. The harmonic is then identified by moving the LO by df and checking that the picture moves by N x df (and which way).
// Needs librtlsdr (found at run time: rtl-sdr-blog or osmocom, any version). No other dependency.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using cf = std::complex<float>;

// ---------------------------------------------------------------- settings
struct Cfg {
    double rate = 2.4e6;        // sample rate; about 2 MHz of it is usable
    double ifHz = 3.57e6;       // tuner IF (librtlsdr: 3.57 MHz for a 6 MHz IF filter, see ifBw)
    double ifBw = 6e6;          // IF filter bandwidth asked for: 6 MHz makes the library use the fixed 3.57 MHz IF (0 = leave to the library)
    double usable = 2.0e6;      // RF span credited to one step (the window minus the filter roll-off)
    double dwell = 0.25;        // seconds per step (Wi-Fi beacons come every 102 ms)
    double dfHz = 40e3;        // LO step used to identify the harmonic
    double gainDb = -1;         // < 0: highest gain
    int ppm = 0;
    double surveyDwell = 0.05;  // seconds per step of the quiet-LO survey
    double quietDb = 2.0;       // a slice is quiet when its power is this close to the quietest 15 percent
    bool allSteps = false;      // also sweep LO steps whose fundamental band is busy
    int device = 0;
    int fftN = 2048;
    double detDb = 3.0;         // a step counts as a detection when the band power is this far above the quiet baseline
    int maxDet = 24;            // identify at most this many detections per harmonic
    std::string csv = "v4range.csv";
    std::string lib;
};

// ---------------------------------------------------------------- FFT
void fft(std::vector<cf>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / (double)len;
        const cf wl((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cf w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const cf u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// What one dwell gives: mean and max-hold power spectrum (linear power per bin, DC in the middle, bin i <-> (i - n/2) * rate / n).
struct Spec {
    std::vector<float> mean, peak;
    double rate = 0;
};

Spec measure(const std::vector<cf>& iq, int n, double rate) {
    Spec s;
    s.rate = rate;
    s.mean.assign((size_t)n, 0.f);
    s.peak.assign((size_t)n, 0.f);
    std::vector<float> win((size_t)n);
    double wsum = 0;
    for (int i = 0; i < n; i++) { win[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2 * M_PI * i / n); wsum += (double)win[(size_t)i] * win[(size_t)i]; }
    std::vector<cf> buf((size_t)n);
    size_t frames = 0;
    for (size_t pos = 0; pos + (size_t)n <= iq.size(); pos += (size_t)n, frames++) {
        for (int i = 0; i < n; i++) buf[(size_t)i] = iq[pos + (size_t)i] * win[(size_t)i];
        fft(buf);
        for (int i = 0; i < n; i++) {
            const int j = (i + n / 2) % n;   // fftshift
            const float p = (float)(std::norm(buf[(size_t)j]) / wsum);
            s.mean[(size_t)i] += p;
            s.peak[(size_t)i] = std::max(s.peak[(size_t)i], p);
        }
    }
    if (frames) for (auto& v : s.mean) v /= (float)frames;
    return s;
}

float toDb(float p) { return 10.f * std::log10(std::max(p, 1e-20f)); }
double median(std::vector<double> v) { if (v.empty()) return 0; std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end()); return v[v.size() / 2]; }
double percentile(std::vector<double> v, double p) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[(size_t)std::min<double>((double)v.size() - 1, p * (double)v.size())]; }

// The band that counts: the middle part of the window without the DC spike and the filter edges.
void bandRange(int n, double rate, double usable, int& lo, int& hi, int& dcLo, int& dcHi) {
    const double bin = rate / n;
    lo = n / 2 - (int)(usable / 2 / bin);
    hi = n / 2 + (int)(usable / 2 / bin);
    dcLo = n / 2 - (int)std::ceil(8e3 / bin);
    dcHi = n / 2 + (int)std::ceil(8e3 / bin);
}

struct Level {
    double bandDb = 0;    // mean power of the band, dB (uncalibrated: relative only)
    double floorDb = 0;   // median bin of the mean spectrum, dB (the noise floor of this step)
    double peakDb = 0;    // strongest bin of the max-hold spectrum, dB
    double peakBbHz = 0;  // where that bin sits in the baseband
    double meanPeakDb = 0;  // strongest bin of the MEAN spectrum (steady carriers; max-hold of pure noise is already ~12 dB over the floor)
};

Level levels(const Spec& s, const Cfg& c) {
    int lo, hi, d0, d1;
    bandRange((int)s.mean.size(), s.rate, c.usable, lo, hi, d0, d1);
    double sum = 0;
    int cnt = 0;
    std::vector<double> bins;
    float pk = 0, mpk = 0;
    int pki = lo;
    for (int i = lo; i < hi; i++) {
        if (i >= d0 && i <= d1) continue;
        sum += s.mean[(size_t)i];
        cnt++;
        bins.push_back(toDb(s.mean[(size_t)i]));
        mpk = std::max(mpk, s.mean[(size_t)i]);
        if (s.peak[(size_t)i] > pk) { pk = s.peak[(size_t)i]; pki = i; }
    }
    Level l;
    l.bandDb = toDb((float)(sum / std::max(cnt, 1)));
    l.floorDb = median(bins);
    l.peakDb = toDb(pk);
    l.meanPeakDb = toDb(mpk);
    l.peakBbHz = (pki - (double)s.mean.size() / 2) * s.rate / (double)s.mean.size();
    return l;
}

// ---------------------------------------------------------------- harmonic identification
// A feature that sits at baseband b moves by -k * df when the LO is raised by df:
//   k = +1  the fundamental (an ordinary signal below the limit)
//   k = -1  the image / mirror of the fundamental
//   k = +-3, +-5, +-7  the 3rd / 5th / 7th harmonic (the sign says whether the spectrum is mirrored)
//   k = 0   fixed in the baseband (DC spike, IF filter shape, ADC spur): not a signal
struct Ident {
    int k = 0;
    double score = 0, second = 0;   // Pearson correlation of the best and of the runner-up hypothesis
    bool clear = false;
};

// Fractional-bin lookup with linear interpolation. Returns false outside the spectrum.
bool at(const std::vector<float>& v, double x, float& out) {
    if (x < 0 || x > (double)v.size() - 1.001) return false;
    const size_t i = (size_t)x;
    const float f = (float)(x - (double)i);
    out = v[i] * (1 - f) + v[i + 1] * f;
    return true;
}

// a: residual dB spectrum (fixed pattern removed) at LO, b: the same at LO + df.
Ident identify(const std::vector<float>& a, const std::vector<float>& b, double binHz, double dfHz, double usableHz) {
    static const int ks[] = {1, -1, 3, -3, 5, -5, 7, -7, 9, -9, 11, -11, 0};
    const int n = (int)a.size();
    const int lo = n / 2 - (int)(usableHz / 2 / binHz), hi = n / 2 + (int)(usableHz / 2 / binHz);
    Ident r;
    std::vector<std::pair<double, int>> sc;
    for (int k : ks) {
        // the feature in b is the feature in a moved by -k df: pred[j] = a[j + k df / bin]
        const double shift = k * dfHz / binHz;
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
        int cnt = 0;
        for (int j = lo; j < hi; j++) {
            float av;
            if (!at(a, j + shift, av)) continue;
            const double x = av, y = b[(size_t)j];
            sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y;
            cnt++;
        }
        double rho = -1;
        if (cnt > 50) {
            const double va = saa - sa * sa / cnt, vb = sbb - sb * sb / cnt;
            if (va > 1e-9 && vb > 1e-9) rho = (sab - sa * sb / cnt) / std::sqrt(va * vb);
        }
        sc.push_back({rho, k});
    }
    std::sort(sc.begin(), sc.end(), [](auto& x, auto& y) { return x.first > y.first; });
    r.k = sc[0].second;
    r.score = sc[0].first;
    r.second = sc[1].first;
    r.clear = r.score > 0.35 && r.score - r.second > 0.08;
    return r;
}

// Local version for one narrow feature: a weak harmonic beside a strong fundamental is found by looking at the neighbourhood of each peak alone.
Ident identifyPeak(const std::vector<float>& a, const std::vector<float>& b, double binHz, double dfHz, int centre, int hw) {
    static const int ks[] = {1, -1, 3, -3, 5, -5, 7, -7, 9, -9, 11, -11, 0};
    Ident r;
    std::vector<std::pair<double, int>> sc;
    const int n = (int)a.size();
    for (int k : ks) {
        const double shift = k * dfHz / binHz;
        const int j0 = (int)std::floor(centre - shift);
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
        int cnt = 0;
        for (int j = j0 - hw; j <= j0 + hw; j++) {
            float av;
            if (j < 0 || j >= n || !at(a, j + shift, av)) continue;
            const double x = av, y = b[(size_t)j];
            sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y;
            cnt++;
        }
        double rho = -1;
        if (cnt > hw) {
            const double va = saa - sa * sa / cnt, vb = sbb - sb * sb / cnt;
            if (va > 1e-9 && vb > 1e-9) rho = (sab - sa * sb / cnt) / std::sqrt(va * vb);
        }
        sc.push_back({rho, k});
    }
    std::sort(sc.begin(), sc.end(), [](auto& x, auto& y) { return x.first > y.first; });
    r.k = sc[0].second; r.score = sc[0].first; r.second = sc[1].first;
    r.clear = r.score > 0.7 && r.score - r.second > 0.15;
    return r;
}

// Narrow peaks of a residual dB spectrum (bin indices, strongest first), at least thrDb over the median of the band.
std::vector<int> findPeaks(const std::vector<float>& res, double binHz, double usableHz, double thrDb, int maxPeaks) {
    const int n = (int)res.size();
    const int lo = n / 2 - (int)(usableHz / 2 / binHz), hi = n / 2 + (int)(usableHz / 2 / binHz);
    std::vector<double> band(res.begin() + lo, res.begin() + hi);
    const double med = median(band);
    std::vector<std::pair<float, int>> c;
    for (int i = lo + 3; i < hi - 3; i++) {
        if (std::abs(i - n / 2) * binHz < 12e3) continue;
        bool mx = true;
        for (int d = -3; d <= 3 && mx; d++) if (d && res[(size_t)(i + d)] > res[(size_t)i]) mx = false;
        if (mx && res[(size_t)i] > med + thrDb) c.push_back({res[(size_t)i], i});
    }
    std::sort(c.begin(), c.end(), [](auto& x, auto& y) { return x.first > y.first; });
    std::vector<int> out;
    for (auto& p : c) {
        bool near = false;
        for (int o : out) if (std::abs(o - p.second) * binHz < 60e3) near = true;
        if (!near) out.push_back(p.second);
        if ((int)out.size() >= maxPeaks) break;
    }
    return out;
}

std::string label(const Ident& id) {
    if (!id.clear) return "unclear";
    switch (id.k) {
    case 1: return "fundamental";
    case -1: return "image (mirror of fundamental)";
    case 0: return "fixed in baseband (artifact)";
    default: break;
    }
    char b[64];
    snprintf(b, sizeof b, "%d%s harmonic (%s)", std::abs(id.k), std::abs(id.k) == 3 ? "rd" : "th", id.k > 0 ? "upright" : "mirrored");
    return b;
}

// dB spectrum of the band minus the fixed pattern.
std::vector<float> residual(const Spec& s, const std::vector<float>& fixedDb) {
    std::vector<float> r(s.mean.size());
    for (size_t i = 0; i < r.size(); i++) r[i] = toDb(s.mean[i]) - fixedDb[i];
    return r;
}

// ---------------------------------------------------------------- known signals
struct Known { const char* name; double mhz, widthMhz; };
const Known kKnown[] = {
    {"Wi-Fi ch1", 2412, 22}, {"Wi-Fi ch6", 2437, 22}, {"Wi-Fi ch11", 2462, 22},
    {"Bluetooth/ISM band", 2441, 80}, {"GPS L1", 1575.42, 2}, {"S-band (2.2-2.3 GHz)", 2250, 100},
};

std::string knownAt(double fromMhz, double toMhz) {
    std::string r;
    for (auto& k : kKnown) {
        if (k.mhz + k.widthMhz / 2 >= fromMhz && k.mhz - k.widthMhz / 2 <= toMhz) {
            if (std::string(k.name) == "Bluetooth/ISM band" && !r.empty()) continue;
            if (!r.empty()) r += "+";
            r += k.name;
        }
    }
    return r;
}

// ---------------------------------------------------------------- the table
// For a target T (MHz) and harmonic N the tuner is asked for LO = (T +/- IF) / N and the dongle for F_set = LO - IF.
void printTable(const Cfg& c) {
    const double ifm = c.ifHz / 1e6;
    printf("Harmonic mapping, IF = %.2f MHz. Dongle LO for target T on harmonic N (hypothesis A: T = N*LO - IF; B: T = N*LO + IF is 2*IF higher).\n", ifm);
    printf("The odd harmonics of a square-wave mixer fall off about as 1/N: expected extra loss vs the fundamental 3rd -9.5 dB, 5th -14, 7th -17, 9th -19, 11th -21.\n");
    printf("(a textbook figure, not measured here; the real tuner filters add to it.)\n\n");
    printf("%8s |", "T MHz");
    for (int n : {3, 5, 7, 9, 11}) printf(" LO N=%-2d  |", n);
    printf("\n");
    for (double t = 1800; t <= 3600 + 1; t += 200) {
        printf("%8.0f |", t);
        for (int n : {3, 5, 7, 9, 11}) printf(" %8.2f |", (t + ifm) / n);
        printf("\n");
    }
    printf("\nWhile tuned for harmonic N, the dongle also receives its fundamental at LO - IF (the F_set column of the doc), and the other odd products\n"
           "at n*LO - IF. A strong signal at the fundamental is what hid the harmonics in the first run; the survey picks LO windows where it is quiet.\n");
}

// ---------------------------------------------------------------- the radio
struct Rtl {
    typedef void* dev_t;
#ifdef _WIN32
    HMODULE h = nullptr;
    void* sym(const char* n) { return (void*)GetProcAddress(h, n); }
#else
    void* h = nullptr;
    void* sym(const char* n) { return dlsym(h, n); }
#endif
    int (*get_device_count)() = nullptr;
    int (*get_usb_strings)(uint32_t, char*, char*, char*) = nullptr;
    int (*open)(dev_t*, uint32_t) = nullptr;
    int (*close)(dev_t) = nullptr;
    int (*set_center_freq)(dev_t, uint32_t) = nullptr;
    int (*set_sample_rate)(dev_t, uint32_t) = nullptr;
    int (*set_gain_mode)(dev_t, int) = nullptr;
    int (*set_gain)(dev_t, int) = nullptr;
    int (*get_gains)(dev_t, int*) = nullptr;
    int (*set_agc)(dev_t, int) = nullptr;
    int (*reset_buffer)(dev_t) = nullptr;
    int (*read_sync)(dev_t, void*, int, int*) = nullptr;
    int (*get_tuner_type)(dev_t) = nullptr;
    int (*set_ppm)(dev_t, int) = nullptr;
    int (*set_bw)(dev_t, uint32_t) = nullptr;
    int (*set_bias)(dev_t, int) = nullptr;
    int (*set_direct)(dev_t, int) = nullptr;
    dev_t d = nullptr;

    bool load(const std::string& path, std::string& err) {
        std::vector<std::string> names;
        if (!path.empty()) names.push_back(path);
#ifdef _WIN32
        for (const char* n : {"rtlsdr.dll", "librtlsdr.dll"}) names.push_back(n);
        for (auto& n : names) { h = LoadLibraryA(n.c_str()); if (h) break; }
#else
        for (const char* n : {"librtlsdr.so.2", "librtlsdr.so.0", "librtlsdr.so", "librtlsdr.2.dylib", "librtlsdr.0.dylib", "librtlsdr.dylib",
                              "/opt/homebrew/lib/librtlsdr.dylib", "/usr/local/lib/librtlsdr.dylib"})
            names.push_back(n);
        for (auto& n : names) { h = dlopen(n.c_str(), RTLD_NOW); if (h) break; }
#endif
        if (!h) { err = "librtlsdr not found (install rtl-sdr-blog or osmocom librtlsdr, or pass --lib <path>)"; return false; }
#define NEED(f, s) if (!(*(void**)&f = sym(s))) { err = std::string("librtlsdr lacks ") + s; return false; }
#define OPT(f, s) *(void**)&f = sym(s)
        NEED(get_device_count, "rtlsdr_get_device_count") NEED(open, "rtlsdr_open") NEED(close, "rtlsdr_close")
        NEED(set_center_freq, "rtlsdr_set_center_freq") NEED(set_sample_rate, "rtlsdr_set_sample_rate")
        NEED(set_gain_mode, "rtlsdr_set_tuner_gain_mode") NEED(set_gain, "rtlsdr_set_tuner_gain") NEED(get_gains, "rtlsdr_get_tuner_gains")
        NEED(reset_buffer, "rtlsdr_reset_buffer") NEED(read_sync, "rtlsdr_read_sync")
        OPT(get_usb_strings, "rtlsdr_get_device_usb_strings"); OPT(set_agc, "rtlsdr_set_agc_mode"); OPT(get_tuner_type, "rtlsdr_get_tuner_type");
        OPT(set_ppm, "rtlsdr_set_freq_correction"); OPT(set_bw, "rtlsdr_set_tuner_bandwidth"); OPT(set_bias, "rtlsdr_set_bias_tee");
        OPT(set_direct, "rtlsdr_set_direct_sampling");
        return true;
    }

    bool start(const Cfg& c, std::string& err) {
        if (get_device_count() <= c.device) { err = "no RTL-SDR found (is it plugged in and not used by another program?)"; return false; }
        char man[256] = "", prod[256] = "", ser[256] = "";
        if (get_usb_strings) get_usb_strings((uint32_t)c.device, man, prod, ser);
        if (open(&d, (uint32_t)c.device) < 0) { err = "could not open the RTL-SDR (busy? permissions? on Linux blacklist dvb_usb_rtl28xxu)"; return false; }
        const int tt = get_tuner_type ? get_tuner_type(d) : 0;
        printf("Dongle: %s / %s, tuner type %d (5 = R820T/R820T2, 6 = R828D)%s\n", man, prod, tt, std::string(prod).find("V4") != std::string::npos ? "  [RTL-SDR Blog V4]" : "");
        if (tt != 5 && tt != 6) printf("WARNING: this tuner is not an R82xx: harmonic reception will not work as described.\n");
        if (set_direct) set_direct(d, 0);
        if (set_bias) set_bias(d, 0);
        if (set_ppm && c.ppm) set_ppm(d, c.ppm);
        if (set_sample_rate(d, (uint32_t)c.rate) < 0) { err = "sample rate refused"; return false; }
        if (set_bw && c.ifBw > 0) set_bw(d, (uint32_t)c.ifBw);
        set_gain_mode(d, 1);
        if (set_agc) set_agc(d, 0);
        int gains[64];
        const int ng = get_gains(d, nullptr);
        int sel = 0;
        if (ng > 0 && ng <= 64) {
            get_gains(d, gains);
            sel = gains[ng - 1];
            if (c.gainDb >= 0) {
                int best = gains[0];
                for (int i = 0; i < ng; i++) if (std::abs(gains[i] - (int)(c.gainDb * 10)) < std::abs(best - (int)(c.gainDb * 10))) best = gains[i];
                sel = best;
            }
        }
        set_gain(d, sel);
        printf("Gain: %.1f dB, sample rate %.3f Msps, IF filter request %.1f MHz\n", sel / 10.0, c.rate / 1e6, c.ifBw / 1e6);
        return true;
    }

    // tune to LO (Hz): the library's F_set = LO - IF
    bool tuneLo(double loHz, const Cfg& c) { return set_center_freq(d, (uint32_t)std::llround(loHz - c.ifHz)) >= 0; }
    bool tuneRf(double hz) { return set_center_freq(d, (uint32_t)std::llround(hz)) >= 0; }

    bool capture(double sec, const Cfg& c, std::vector<cf>& iq) {
        const size_t n = (size_t)(sec * c.rate);
        const size_t bytes = ((n * 2 + 511) / 512) * 512;   // bulk reads come in multiples of 512
        std::vector<uint8_t> raw(bytes);
        reset_buffer(d);
        // throw the first part away: the PLL and the filters settle
        std::vector<uint8_t> junk(65536);
        int got = 0;
        read_sync(d, junk.data(), (int)junk.size(), &got);
        size_t done = 0;
        while (done < bytes) {
            const int want = (int)std::min<size_t>(bytes - done, 262144);
            const int rc = read_sync(d, raw.data() + done, want, &got);
            if (rc < 0 || got <= 0) { fprintf(stderr, "read_sync rc=%d got=%d\n", rc, got); return false; }
            done += (size_t)got;
        }
        iq.resize(n);
        for (size_t i = 0; i < n; i++) iq[i] = cf(((int)raw[2 * i] - 127.5f) / 128.f, ((int)raw[2 * i + 1] - 127.5f) / 128.f);
        return true;
    }
};

// ---------------------------------------------------------------- the quiet-LO survey
// The fundamental band of a harmonic step is what the dongle hears at F_set = LO - IF (+-1 MHz). The survey measures that whole range once
// (short dwell), so that harmonic steps can be put where the fundamental is quiet.
struct Survey {
    double f0 = 0, step = 0;
    std::vector<Level> lv;
    std::vector<char> quiet;
    double baseline = 0;
    double powerAt(double fSet) const {
        if (lv.empty()) return 1e9;
        const long i = std::lround((fSet - f0) / step);
        return i < 0 || i >= (long)lv.size() ? 1e9 : lv[(size_t)i].bandDb;
    }
    bool quietAt(double fSet) const {
        if (lv.empty()) return false;
        const long i = std::lround((fSet - f0) / step);
        if (i < 1 || i + 1 >= (long)lv.size()) return false;
        return quiet[(size_t)i - 1] && quiet[(size_t)i] && quiet[(size_t)i + 1];   // the 6 MHz IF filter also lets the neighbours in
    }
};

bool runSurvey(Rtl& rtl, const Cfg& c, double fromHz, double toHz, Survey& sv) {
    sv.f0 = std::max(fromHz, 30e6);
    sv.step = c.usable;
    const double top = std::min(toHz, 1.7e9);
    const int total = (int)((top - sv.f0) / sv.step) + 1;
    printf("\nQuiet-LO survey: dongle %.0f - %.0f MHz in %d steps of %.1f MHz (dwell %.2f s)\n", sv.f0 / 1e6, top / 1e6, total, sv.step / 1e6, c.surveyDwell);
    std::vector<cf> iq;
    for (int i = 0; i < total; i++) {
        if (!rtl.tuneRf(sv.f0 + i * sv.step) || !rtl.capture(c.surveyDwell, c, iq)) { printf("  read failed at %.1f MHz\n", (sv.f0 + i * sv.step) / 1e6); return false; }
        sv.lv.push_back(levels(measure(iq, c.fftN, c.rate), c));
        if (i % 25 == 0) { printf("\r  %3d%%", 100 * i / total); fflush(stdout); }
    }
    printf("\r  100%%\n");
    std::vector<double> p;
    for (auto& l : sv.lv) p.push_back(l.bandDb);
    sv.baseline = percentile(p, 0.15);
    for (auto& l : sv.lv) sv.quiet.push_back(l.bandDb <= sv.baseline + c.quietDb && l.meanPeakDb - l.floorDb < 6.0);
    {
        size_t byPower = 0, byPeak = 0;
        std::vector<double> pk;
        for (auto& l : sv.lv) { byPower += l.bandDb <= sv.baseline + c.quietDb; byPeak += l.meanPeakDb - l.floorDb < 6.0; pk.push_back(l.peakDb - l.floorDb); }
        printf("  band power within %.1f dB of the floor: %zu slices; no steady peak over 6 dB: %zu slices (median peak-over-floor %.1f dB)\n", c.quietDb, byPower, byPeak, median(pk));
    }
    size_t q = (size_t)std::count(sv.quiet.begin(), sv.quiet.end(), 1);
    printf("  quietest 15%% of slices at %.1f dB; %zu of %zu slices quiet (within %.1f dB of that, no steady peak over 6 dB)\n", sv.baseline, q, sv.quiet.size(), c.quietDb);
    return true;
}

struct Band { const char* name; double from, to; };

// For each target band and harmonic: which LO windows are quiet, and where their target lands.
void printQuiet(const Cfg& c, const Survey& sv, const std::vector<int>& harms) {
    printf("\n== Quiet LO windows (fundamental band empty), by target band and harmonic\n");
    for (Band b : {Band{"Wi-Fi / Bluetooth 2400-2483", 2400, 2483}, Band{"S-band 2200-2300", 2200, 2300}}) {
        printf("%s\n", b.name);
        for (int n : harms) {
            const double loA = (b.from * 1e6 + c.ifHz) / n, loB = (b.to * 1e6 + c.ifHz) / n;
            int tot = 0, qt = 0;
            std::string win;
            double runStart = -1, last = 0;
            for (double lo = loA; lo <= loB + 1; lo += sv.step / 2) {
                tot++;
                const bool q = sv.quietAt(lo - c.ifHz);
                qt += q;
                if (q && runStart < 0) runStart = lo;
                if (runStart >= 0 && (!q || lo + sv.step / 2 > loB + 1)) { char t[64]; snprintf(t, sizeof t, " %.1f-%.1f", runStart / 1e6, (q ? lo : last) / 1e6); win += t; runStart = -1; }
                last = lo;
            }
            printf("  N=%-2d LO %7.2f-%7.2f MHz: %3d%% quiet%s%s\n", n, loA / 1e6, loB / 1e6, tot ? 100 * qt / tot : 0, win.empty() ? "" : "   quiet LO windows (MHz):", win.c_str());
        }
    }
    printf("Whole range, share of quiet LO per 200 MHz of target (1800..3600):\n");
    for (int n : harms) {
        printf("  N=%-2d", n);
        for (double t = 1800; t < 3600; t += 200) {
            int tot = 0, qt = 0;
            for (double lo = (t * 1e6 + c.ifHz) / n; lo <= ((t + 200) * 1e6 + c.ifHz) / n; lo += sv.step / 2) { tot++; qt += sv.quietAt(lo - c.ifHz); }
            printf(" %3d%%", tot ? 100 * qt / tot : 0);
        }
        printf("\n");
    }
}

// ---------------------------------------------------------------- the sweep
struct Step {
    double lo = 0;   // Hz
    Spec spec;
    Level lv;
    double excess = 0;
    bool det = false, quiet = false;
    Ident id;
    bool identified = false;
    std::vector<std::pair<double, Ident>> peaks;   // narrow peaks: baseband Hz and their local identification
};

struct Run {
    int harm = 0;
    std::vector<Step> steps;
    double baseline = 0;
    size_t skipped = 0;
    std::vector<float> fixedDb;
};

double dfFor(const Cfg& c) { return std::min(c.dfHz, 0.5e6 / 11); }   // the biggest candidate (11th) must stay inside the window

void sweepHarmonic(Rtl& rtl, const Cfg& c, const Survey& sv, int n, double fromMhz, double toMhz, Run& run) {
    run.harm = n;
    // the product N*LO -/+ IF covers T; sweep N*LO over [from - IF - span, to + IF + span] so that either hypothesis is covered
    const double nloFrom = fromMhz * 1e6 - c.ifHz - c.usable / 2, nloTo = toMhz * 1e6 + c.ifHz + c.usable / 2;
    const double stepLo = c.usable / n;
    const int total = (int)((nloTo - nloFrom) / c.usable) + 1;
    printf("\n%d%s harmonic: %d candidate steps, LO %.3f - %.3f MHz, dongle %.3f - %.3f MHz%s\n", n, n == 3 ? "rd" : "th", total, nloFrom / n / 1e6, nloTo / n / 1e6,
           (nloFrom / n - c.ifHz) / 1e6, (nloTo / n - c.ifHz) / 1e6, c.allSteps ? " (all steps)" : " (quiet fundamental only)");
    std::vector<cf> iq;
    // no quiet step at all in this range: fall back to the 20 percent least busy ones (marked as busy LO in the output, never hidden)
    int nQuiet = 0;
    std::vector<double> pw;
    for (int i = 0; i < total; i++) { const double f = nloFrom / n + i * stepLo - c.ifHz; nQuiet += sv.quietAt(f); pw.push_back(sv.powerAt(f)); }
    const double leastBusy = nQuiet == 0 ? percentile(pw, 0.2) : -1e9;
    if (nQuiet == 0 && !c.allSteps) printf("  no quiet LO window for this harmonic: measuring the 20%% least busy steps instead\n");
    for (int i = 0; i < total; i++) {
        Step st;
        st.lo = nloFrom / n + i * stepLo;
        st.quiet = sv.quietAt(st.lo - c.ifHz);
        // the steps that cover a Wi-Fi channel are always measured (busy or not): their level against the floor goes into the verdict
        bool wifi = false;
        for (double ch : {2412.0, 2437.0, 2462.0}) {
            const double ta = (st.lo * n - c.ifHz) / 1e6, tb = (st.lo * n + c.ifHz) / 1e6;
            if (std::fabs(ta - ch) <= 11 || std::fabs(tb - ch) <= 11) wifi = true;
        }
        if (!st.quiet && !c.allSteps && !wifi && !(nQuiet == 0 && pw[(size_t)i] <= leastBusy)) { run.skipped++; continue; }
        if (!rtl.tuneLo(st.lo, c) || !rtl.capture(c.dwell, c, iq)) { printf("  read failed at LO %.3f MHz\n", st.lo / 1e6); break; }
        st.spec = measure(iq, c.fftN, c.rate);
        st.lv = levels(st.spec, c);
        run.steps.push_back(std::move(st));
        if (i % 25 == 0) { printf("\r  %3d%%", 100 * i / total); fflush(stdout); }
    }
    printf("\r  100%%  measured %zu steps, skipped %zu with a busy fundamental\n", run.steps.size(), run.skipped);
    if (run.steps.empty()) return;
    const int nb = c.fftN;
    run.fixedDb.assign((size_t)nb, 0.f);
    {
        std::vector<double> col(run.steps.size());
        for (int b = 0; b < nb; b++) {
            for (size_t s = 0; s < run.steps.size(); s++) col[s] = toDb(run.steps[s].spec.mean[(size_t)b]);
            run.fixedDb[(size_t)b] = (float)median(col);
        }
    }
    std::vector<double> lv, lq;
    for (auto& s : run.steps) { lv.push_back(s.lv.bandDb); if (s.quiet) lq.push_back(s.lv.bandDb); }
    run.baseline = lq.size() >= 5 ? median(lq) : percentile(lv, 0.2);   // the noise floor of the quiet band
    for (auto& s : run.steps) {
        s.excess = s.lv.bandDb - run.baseline;
        s.det = s.excess >= c.detDb || (s.lv.peakDb - s.lv.floorDb) >= 17.0;
    }
}

void identifyRun(Rtl& rtl, const Cfg& c, Run& run) {
    std::vector<size_t> idx;
    for (size_t i = 0; i < run.steps.size(); i++) if (run.steps[i].det) idx.push_back(i);
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return run.steps[a].excess > run.steps[b].excess; });
    const size_t nDet = idx.size();
    if ((int)idx.size() > c.maxDet) idx.resize((size_t)c.maxDet);
    const double df = dfFor(c);
    printf("  %zu detections, identifying the %zu strongest (LO step %.0f kHz)\n", nDet, idx.size(), df / 1e3);
    std::vector<cf> iq;
    const double binHz = c.rate / c.fftN;
    for (size_t i : idx) {
        Step& st = run.steps[i];
        if (!rtl.tuneLo(st.lo + df, c) || !rtl.capture(c.dwell, c, iq)) continue;
        const Spec b = measure(iq, c.fftN, c.rate);
        const auto ra = residual(st.spec, run.fixedDb), rb = residual(b, run.fixedDb);
        st.id = identify(ra, rb, binHz, df, c.usable);
        st.identified = true;
        for (int pk : findPeaks(ra, binHz, c.usable, 12.0, 4))
            st.peaks.push_back({(pk - c.fftN / 2) * binHz, identifyPeak(ra, rb, binHz, df, pk, (int)(40e3 / binHz))});
    }
}

bool isHarmonicLabel(const Ident& id) { return id.clear && std::abs(id.k) >= 3; }
bool stepHasHarmonic(const Step& st) {
    if (st.identified && isHarmonicLabel(st.id)) return true;
    for (auto& p : st.peaks) if (isHarmonicLabel(p.second)) return true;
    return false;
}

std::string peaksText(const Step& st) {
    std::string r;
    for (auto& p : st.peaks) {
        char t[80];
        snprintf(t, sizeof t, "%s%.0fkHz:%s%d", r.empty() ? "" : ";", p.first / 1e3, p.second.clear ? "k" : "?k", p.second.k);
        r += t;
    }
    return r;
}

void report(const Cfg& c, std::vector<Run>& runs, FILE* csv) {
    if (csv) fprintf(csv, "harmonic,lo_mhz,dongle_mhz,target_if_minus_mhz,target_if_plus_mhz,quiet_fundamental,band_db,floor_db,peak_db,peak_baseband_khz,excess_db,detected,identified_k,identified_as,score,peaks,known_signal\n");
    for (auto& run : runs) {
        printf("\n== %d%s harmonic: quiet-band noise floor %.1f dB (uncalibrated)\n", run.harm, run.harm == 3 ? "rd" : "th", run.baseline);
        int shown = 0;
        for (auto& st : run.steps) {
            const double nlo = st.lo * run.harm;
            const double ta = (nlo - c.ifHz) / 1e6, tb = (nlo + c.ifHz) / 1e6;
            const std::string kn = knownAt(std::min(ta, tb) - 1, std::max(ta, tb) + 1);
            if (csv) fprintf(csv, "%d,%.4f,%.4f,%.3f,%.3f,%d,%.2f,%.2f,%.2f,%.1f,%.2f,%d,%d,\"%s\",%.3f,\"%s\",\"%s\"\n", run.harm, st.lo / 1e6, (st.lo - c.ifHz) / 1e6, ta, tb, st.quiet ? 1 : 0, st.lv.bandDb, st.lv.floorDb,
                             st.lv.peakDb, st.lv.peakBbHz / 1e3, st.excess, st.det ? 1 : 0, st.identified ? st.id.k : 99, st.identified ? label(st.id).c_str() : "", st.identified ? st.id.score : 0.0, peaksText(st).c_str(), kn.c_str());
            if (st.det && shown < 40) {
                printf("  target %9.2f / %9.2f MHz (A/B): +%5.1f dB, peak +%4.1f over floor, %s: %s%s%s%s\n", ta, tb, st.excess, st.lv.peakDb - st.lv.floorDb, st.quiet ? "quiet LO" : "busy LO",
                       st.identified ? label(st.id).c_str() : "(not identified)", st.peaks.empty() ? "" : "  peaks ", peaksText(st).c_str(), kn.empty() ? "" : ("   known: " + kn).c_str());
                shown++;
            }
        }
        if (!shown) printf("  nothing above the quiet-band noise floor%s\n", run.steps.empty() ? " (no quiet LO step in this range, nothing measured)" : "");
    }
}

// The verdict. Yes only when a detection is labelled 3rd/5th/7th/9th/11th with a clear margin AND its target is a known signal. The level of
// each Wi-Fi channel against the quiet-band noise floor is printed whatever happens.
bool verdict(const Cfg& c, const std::vector<Run>& runs) {
    printf("\n================ VERDICT ================\n");
    bool yes = false;
    for (auto& run : runs) {
        for (auto& st : run.steps) {
            if (!stepHasHarmonic(st)) continue;
            const double nlo = st.lo * run.harm, ta = (nlo - c.ifHz) / 1e6, tb = (nlo + c.ifHz) / 1e6;
            const std::string kn = knownAt(std::min(ta, tb) - 1, std::max(ta, tb) + 1);
            if (st.excess >= c.detDb && !kn.empty()) { yes = true; printf("  harmonic detection: N=%d at target %.1f/%.1f MHz (+%.1f dB), known: %s\n", run.harm, ta, tb, st.excess, kn.c_str()); }
        }
    }
    printf("Wi-Fi channels against the quiet-band noise floor (best step within 11 MHz of the centre, either hypothesis):\n");
    for (Known k : {Known{"ch1", 2412, 0}, Known{"ch6", 2437, 0}, Known{"ch11", 2462, 0}}) {
        for (auto& run : runs) {
            double best = -99;
            const Step* bs = nullptr;
            char hyp = 0;
            for (auto& st : run.steps) {
                const double nlo = st.lo * run.harm, ta = (nlo - c.ifHz) / 1e6, tb = (nlo + c.ifHz) / 1e6;
                if (std::fabs(ta - k.mhz) <= 11 && st.excess > best) { best = st.excess; bs = &st; hyp = 'A'; }
                if (std::fabs(tb - k.mhz) <= 11 && st.excess > best) { best = st.excess; bs = &st; hyp = 'B'; }
            }
            if (!bs) { printf("  %s  N=%-2d  no quiet-LO step covers it, nothing measured\n", k.name, run.harm); continue; }
            printf("  %s  N=%-2d  %+6.1f dB over the quiet floor (hyp %c, %s LO)  %s\n", k.name, run.harm, best, hyp, bs->quiet ? "quiet" : "busy",
                   bs->identified ? label(bs->id).c_str() : (bs->det ? "not identified" : "below the detection threshold"));
        }
    }
    printf("\nharmonic reception demonstrated: %s\n", yes ? "YES" : "NO");
    if (!yes) printf("(no detection with a clear 3rd/5th/7th/9th/11th label on a known signal; fundamentals, unclear and fixed ones do not count)\n");
    return yes;
}

// ---------------------------------------------------------------- reference below the limit
void reference(Rtl& rtl, const Cfg& c) {
    printf("\n== Reference below the rated limit (checks that the rig works and the antenna sees something)\n");
    std::vector<cf> iq;
    struct R { const char* name; double from, to; };
    for (R r : {R{"FM broadcast 88-108 MHz", 88e6, 108e6}, R{"GPS L1 1575.42 MHz (needs an active antenna; passive shows noise only)", 1575.42e6, 1575.42e6}}) {
        double best = -200, bestF = 0, floorDb = 0;
        std::vector<double> floors;
        for (double f = r.from; f <= r.to + 1; f += std::max(c.usable, 1.0)) {
            if (!rtl.tuneRf(f) || !rtl.capture(c.dwell, c, iq)) { printf("  read failed\n"); return; }
            const Level l = levels(measure(iq, c.fftN, c.rate), c);
            floors.push_back(l.floorDb);
            if (l.peakDb > best) { best = l.peakDb; bestF = f + l.peakBbHz; }
            if (r.from == r.to) break;
        }
        floorDb = median(floors);
        printf("  %s: strongest bin %.1f dB at %.3f MHz, noise floor %.1f dB, margin %.1f dB\n", r.name, best, bestF / 1e6, floorDb, best - floorDb);
    }
}

// ---------------------------------------------------------------- selftest
// Synthetic radio: signals at RF frequencies, mixer product k (see Ident) with the given LO, IF, white noise, a fixed spur and a DC spike.
struct Synth {
    struct Sig { double rf, width, amp; int prod = 0, sgn = 0; };   // prod/sgn: own mixer product (0 = the Synth default)
    std::vector<Sig> sigs;
    int n = 3;             // harmonic of the wanted product
    int sign = 1;          // +1 upright, -1 mirrored
    double ifHz = 3.57e6;
    unsigned seed = 1;

    std::vector<cf> gen(double loHz, size_t count, double rate, std::mt19937& rng) const {
        std::vector<cf> x(count);
        std::normal_distribution<float> g(0.f, 0.02f);
        for (auto& v : x) v = cf(g(rng), g(rng));
        // the tuner's fixed baseband response: DC spike and a spur at -0.7 MHz
        const double spurs[][2] = {{0.0, 0.3}, {-0.7e6, 0.12}};
        for (auto& s : spurs) for (size_t i = 0; i < count; i++) x[i] += (float)s[1] * cf((float)std::cos(2 * M_PI * s[0] * i / rate + 0.3), (float)std::sin(2 * M_PI * s[0] * i / rate + 0.3));
        std::uniform_real_distribution<double> ph(0, 2 * M_PI);
        for (auto& sg : sigs) {
            // baseband position of RF sg.rf: b = sign * (rf - (n*lo - ifHz))
            const int pn = sg.prod ? sg.prod : n, ps = sg.sgn ? sg.sgn : sign;
            const double b = ps * (sg.rf - (pn * loHz - ifHz));
            const int tones = sg.width < 50e3 ? 1 : (int)(sg.width / 25e3);
            for (int t = 0; t < tones; t++) {
                const double off = tones == 1 ? 0 : (t - (tones - 1) / 2.0) * 25e3;
                const double bb = b + ps * off;
                if (std::fabs(bb) > rate / 2 * 0.98) continue;
                const double p0 = ph(rng);
                // slow roll-off at the edges of a wide signal
                const double a = sg.amp / std::sqrt((double)tones) * 2.0;
                for (size_t i = 0; i < count; i++) {
                    const double a2 = 2 * M_PI * bb * i / rate + p0;
                    x[i] += cf((float)(a * std::cos(a2)), (float)(a * std::sin(a2)));
                }
            }
        }
        return x;
    }
};

int check(bool ok, const char* what) { printf("  %-70s %s\n", what, ok ? "ok" : "FAIL"); return ok ? 0 : 1; }

int selftest() {
    printf("selftest: harmonic identification on synthetic data\n");
    Cfg c;
    c.fftN = 2048;
    c.dwell = 0.04;
    int fails = 0;
    const double rate = c.rate;
    const size_t cnt = (size_t)(c.dwell * rate);
    struct Case { int n, sign; double width; const char* name; };
    const Case cases[] = {
        {1, 1, 30e3, "fundamental, narrow"},   {1, -1, 30e3, "image of fundamental, narrow"},
        {3, 1, 30e3, "3rd upright, narrow"},   {3, -1, 30e3, "3rd mirrored, narrow"},
        {5, 1, 30e3, "5th upright, narrow"},   {5, -1, 30e3, "5th mirrored, narrow"},
        {7, 1, 30e3, "7th upright, narrow"},
        {3, 1, 600e3, "3rd upright, wideband (Wi-Fi like)"}, {5, -1, 600e3, "5th mirrored, wideband"},
        {5, 1, 600e3, "5th upright, wideband"},  {3, -1, 600e3, "3rd mirrored, wideband"},
    };
    for (const Case& cs : cases) {
        std::mt19937 rng(7);
        // a quiet pattern from some steps without signals, for the fixed spectrum
        std::vector<std::vector<float>> quietDb;
        const double loCenter = 800e6;   // 3rd: RF 2.4 GHz
        Synth sy;
        sy.n = cs.n; sy.sign = cs.sign;
        // start so that the feature stays inside the window and clear of the spur after the LO step (it moves by -k df)
        const double b0 = 0.6e6 + cs.sign * cs.n * c.dfHz / 2;
        sy.sigs.push_back({cs.n * loCenter - 3.57e6 + cs.sign * b0, cs.width, 0.12});
        // fixed pattern from 9 quiet steps far away
        std::vector<std::vector<float>> qs;
        for (int q = 0; q < 9; q++) {
            Synth qq; qq.n = cs.n; qq.sign = cs.sign;
            qs.push_back([&] { Spec s = measure(qq.gen(loCenter + (q - 4) * 5e6, cnt, rate, rng), c.fftN, rate); std::vector<float> d(s.mean.size()); for (size_t i = 0; i < d.size(); i++) d[i] = toDb(s.mean[i]); return d; }());
        }
        std::vector<float> fixedDb(c.fftN);
        for (int b = 0; b < c.fftN; b++) { std::vector<double> col; for (auto& q : qs) col.push_back(q[(size_t)b]); fixedDb[(size_t)b] = (float)median(col); }
        const Spec a = measure(sy.gen(loCenter, cnt, rate, rng), c.fftN, rate);
        const Spec b = measure(sy.gen(loCenter + c.dfHz, cnt, rate, rng), c.fftN, rate);
        const Ident id = identify(residual(a, fixedDb), residual(b, fixedDb), rate / c.fftN, c.dfHz, c.usable);
        // expected k: the product at baseband b = sign*(rf - (n lo - IF)): lo up by df -> b moves by -sign*n*df -> k = sign*n
        const int want = cs.sign * cs.n;
        char msg[160];
        snprintf(msg, sizeof msg, "%s: expect k=%d, got k=%d (score %.2f, runner-up %.2f)%s", cs.name, want, id.k, id.score, id.second, id.clear ? "" : " [unclear]");
        fails += check(id.k == want && id.clear, msg);
    }
    // a strong fundamental plus a weak N-th harmonic in the same IF: the weak one must still be identified, and the strong one as k = 1
    {
        struct W { int n, sign; const char* name; };
        for (W w : {W{9, 1, "9th upright"}, W{7, -1, "7th mirrored"}, W{11, 1, "11th upright"}, W{5, 1, "5th upright"}, W{3, -1, "3rd mirrored"}}) {
            std::mt19937 rng(21);
            const double loCenter = 343e6, df = std::min(c.dfHz, 0.5e6 / 11);
            Synth sy;
            sy.n = w.n; sy.sign = w.sign;
            // strong fundamental at baseband -0.45 MHz (22 dB over the weak one), weak harmonic at +0.5 MHz after the step
            sy.sigs.push_back({loCenter - 3.57e6 - 0.45e6, 20e3, 0.15, 1, 1});
            const double b0 = 0.45e6 + w.sign * w.n * df / 2;
            sy.sigs.push_back({w.n * loCenter - 3.57e6 + w.sign * b0, 20e3, 0.010, w.n, w.sign});
            Synth qq;
            std::vector<std::vector<float>> qs;
            for (int q = 0; q < 9; q++) {
                Spec sp = measure(qq.gen(loCenter + (q - 4) * 5e6, cnt, rate, rng), c.fftN, rate);
                std::vector<float> d(sp.mean.size());
                for (size_t i = 0; i < d.size(); i++) d[i] = toDb(sp.mean[i]);
                qs.push_back(d);
            }
            std::vector<float> fixedDb(c.fftN);
            for (int b = 0; b < c.fftN; b++) { std::vector<double> col; for (auto& q : qs) col.push_back(q[(size_t)b]); fixedDb[(size_t)b] = (float)median(col); }
            const Spec a = measure(sy.gen(loCenter, cnt * 2, rate, rng), c.fftN, rate);
            const Spec b = measure(sy.gen(loCenter + df, cnt * 2, rate, rng), c.fftN, rate);
            const auto ra = residual(a, fixedDb), rb = residual(b, fixedDb);
            const double binHz = rate / c.fftN;
            bool gotStrong = false, gotWeak = false;
            for (int pk : findPeaks(ra, binHz, c.usable, 10.0, 4)) {
                const Ident id = identifyPeak(ra, rb, binHz, df, pk, (int)(40e3 / binHz));
                const double bb = (pk - c.fftN / 2) * binHz;
                if (std::fabs(bb + 0.45e6) < 20e3 && id.k == 1 && id.clear) gotStrong = true;
                if (std::fabs(bb - b0) < 20e3 && id.k == w.sign * w.n && id.clear) gotWeak = true;
            }
            char msg[160];
            snprintf(msg, sizeof msg, "strong fundamental + weak %s (22 dB down) identified separately", w.name);
            fails += check(gotStrong && gotWeak, msg);
        }
    }
    // a signal that does not move (baseband artifact) must come out as k = 0, not as a harmonic
    {
        std::mt19937 rng(9);
        Synth sy;   // no RF signals: only the fixed spurs, but no fixed pattern removal this time
        std::vector<float> zero(c.fftN, 0.f);
        const Spec a = measure(sy.gen(800e6, cnt, rate, rng), c.fftN, rate);
        const Spec b = measure(sy.gen(800e6 + c.dfHz, cnt, rate, rng), c.fftN, rate);
        const Ident id = identify(residual(a, zero), residual(b, zero), rate / c.fftN, c.dfHz, c.usable);
        fails += check(id.k == 0, "baseband spur does not move: k = 0");
    }
    // the table mapping: A hypothesis for N=5, 2412 MHz
    {
        const double t = 2412e6, lo = (t + 3.57e6) / 5;
        fails += check(std::fabs(5 * lo - 3.57e6 - t) < 1.0, "LO for 2412 MHz on the 5th harmonic maps back to the target");
        fails += check(lo > 27e6 && lo < 1.766e9, "that LO is inside the tuner's valid range");
        fails += check(std::string(knownAt(2411, 2413)).find("Wi-Fi ch1") != std::string::npos, "known-signal list finds Wi-Fi channel 1");
    }
    printf("%s\n", fails ? "SELFTEST FAILED" : "selftest passed");
    return fails ? 1 : 0;
}

bool parseRange(const std::string& s, double& a, double& b) { return sscanf(s.c_str(), "%lf-%lf", &a, &b) == 2 && a < b; }

}  // namespace

int main(int argc, char** argv) {
    Cfg c;
    bool surveyOnly = false, table = false, test = false, noRef = false;
    double from = 2380, to = 2500;
    std::vector<int> harms = {3, 5, 7, 9, 11};
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto nx = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--selftest") test = true;
        else if (a == "--table") table = true;
        else if (a == "--full") { from = 1800; to = 3600; }
        else if (a == "--range") { if (!parseRange(nx(), from, to)) { fprintf(stderr, "--range MHz-MHz\n"); return 2; } }
        else if (a == "--harm") { harms.clear(); std::string s = nx(); size_t p0 = 0; while (p0 < s.size()) { const int v = atoi(s.c_str() + p0); if (v == 3 || v == 5 || v == 7 || v == 9 || v == 11) harms.push_back(v); const size_t q = s.find(',', p0); if (q == std::string::npos) break; p0 = q + 1; } }
        else if (a == "--all-steps") c.allSteps = true;
        else if (a == "--survey-only") surveyOnly = true;
        else if (a == "--gain") c.gainDb = atof(nx());
        else if (a == "--dwell") c.dwell = atof(nx());
        else if (a == "--df") c.dfHz = atof(nx()) * 1e3;
        else if (a == "--ppm") c.ppm = atoi(nx());
        else if (a == "--device") c.device = atoi(nx());
        else if (a == "--csv") c.csv = nx();
        else if (a == "--lib") c.lib = nx();
        else if (a == "--if") c.ifHz = atof(nx()) * 1e6;
        else if (a == "--if-bw") c.ifBw = atof(nx()) * 1e6;
        else if (a == "--thr") c.detDb = atof(nx());
        else if (a == "--no-reference") noRef = true;
        else { printf("usage: v4range [--full | --range MHz-MHz] [--harm 3,5,7,9,11] [--all-steps] [--survey-only] [--gain dB] [--dwell s] [--df kHz] [--csv file] [--lib path] [--ppm n]\n"
                      "               [--if MHz] [--if-bw MHz] [--thr dB] [--no-reference] | --table | --selftest\n"); return a == "--help" ? 0 : 2; }
    }
    if (test) return selftest();
    if (table) { printTable(c); return 0; }
    if (harms.empty()) { fprintf(stderr, "no harmonic selected\n"); return 2; }

    Rtl rtl;
    std::string err;
    if (!rtl.load(c.lib, err) || !rtl.start(c, err)) { fprintf(stderr, "v4range: %s\n", err.c_str()); return 1; }
    if (!noRef) reference(rtl, c);
    // the dongle ranges the harmonic steps need as fundamentals
    double fMin = 1e12, fMax = 0;
    for (int n : harms) {
        fMin = std::min(fMin, ((from * 1e6 - c.ifHz - c.usable / 2) / n - c.ifHz) - c.usable);
        fMax = std::max(fMax, ((to * 1e6 + c.ifHz + c.usable / 2) / n - c.ifHz) + c.usable);
    }
    Survey sv;
    if (!runSurvey(rtl, c, fMin, fMax, sv)) { rtl.close(rtl.d); return 1; }
    printQuiet(c, sv, harms);
    if (surveyOnly) { rtl.close(rtl.d); return 0; }
    std::vector<Run> runs;
    for (int n : harms) {
        Run r;
        sweepHarmonic(rtl, c, sv, n, from, to, r);
        identifyRun(rtl, c, r);
        runs.push_back(std::move(r));
    }
    FILE* f = fopen(c.csv.c_str(), "w");
    report(c, runs, f);
    if (f) { fclose(f); printf("\nCSV: %s\n", c.csv.c_str()); }
    printf("\nHow to read it: a real harmonic reception is a detection whose label is 3rd/5th/7th/9th/11th (upright or mirrored) and whose target matches a\n"
           "known signal (Wi-Fi 2412/2437/2462) under hypothesis A or B. 'fundamental' or 'image' = a strong signal below 1.766 GHz leaking in (needs a filter).\n"
           "'fixed in baseband' or 'unclear' = no evidence. See docs/V4-RANGE-EXPERIMENT.md.\n");
    verdict(c, runs);
    rtl.close(rtl.d);
    return 0;
}

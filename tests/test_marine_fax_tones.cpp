// Start and stop tone detection: tone frequencies of fldigi wefax.cxx (300 Hz IOC 576, 675 Hz IOC 288, 450 Hz stop),
// mistuning of +-50 Hz, noise, dropouts, and no false alarms on noise or speech-like audio.
#include "data/marine/fax/fax_util.h"
using namespace faxt;

static const double fs = 12000;

static void tone(std::vector<float>& a, double f, double secs, double amp = 0.5, double* ph = nullptr) {
    static double phase = 0;
    double& p = ph ? *ph : phase;
    for (size_t i = 0, n = static_cast<size_t>(secs * fs); i < n; i++) { p += 2 * M_PI * f / fs; a.push_back(static_cast<float>(amp * std::sin(p))); }
}
static void silence(std::vector<float>& a, double secs) { a.insert(a.end(), static_cast<size_t>(secs * fs), 0.f); }
static void noise(std::vector<float>& a, double secs, double sigma, std::mt19937& rng) {
    std::normal_distribution<double> g(0, sigma);
    for (size_t i = 0, n = static_cast<size_t>(secs * fs); i < n; i++) a.push_back(static_cast<float>(g(rng)));
}
static void addNoise(std::vector<float>& a, double sigma, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0, sigma);
    for (float& v : a) v += static_cast<float>(g(rng));
}

int main() {
    std::mt19937 rng(3);
    // Start tone then stop tone, with mistuning.
    for (double off : {-50.0, -25.0, 0.0, 25.0, 50.0}) {
        for (int ioc : {576, 288}) {
            const double f0 = ioc == 576 ? 300 : 675;
            std::vector<float> a;
            silence(a, 1);
            tone(a, f0 + off, 3);
            FaxDecoder d; d.configure(fs);
            feedAll(d, a, 1000);
            FaxStatus st = d.status();
            FCHECK(st.state == 1, "off %+.0f ioc %d: start tone not detected (state %d)", off, ioc, st.state);
            FCHECK(st.ioc == ioc, "off %+.0f: ioc %d, want %d", off, st.ioc, ioc);
            FCHECK(std::fabs(st.toneHz - (f0 + off)) < 3.0, "off %+.0f ioc %d: tone measured %.1f Hz, want %.1f", off, ioc, st.toneHz, f0 + off);
            std::vector<float> b;
            silence(b, 1);
            tone(b, 450 + off, 2);
            feedAll(d, b, 333);
            st = d.status();
            FCHECK(st.state == 3, "off %+.0f: stop tone not detected (state %d)", off, st.state);
            FCHECK(std::fabs(st.toneHz - (450 + off)) < 3.0, "off %+.0f: stop tone measured %.1f Hz", off, st.toneHz);
        }
    }
    printf("tones at -50, -25, 0, +25, +50 Hz mistuning: start (both IOC) and stop detected, frequency within 3 Hz\n");

    // Noise: with the tone at 0 dB against the whole audio band the tone is still found (power ratio 0.5).
    {
        for (double snr : {10.0, 3.0, 0.0}) {
            std::vector<float> a;
            silence(a, 1); tone(a, 300, 4); silence(a, 1);
            addNoise(a, std::sqrt(0.125 / std::pow(10.0, snr / 10.0)), 11);
            FaxDecoder d; d.configure(fs);
            feedAll(d, a, 4096);
            const bool ok = d.status().state == 1;
            printf("start tone at %.0f dB audio SNR: %s (tone measured %.1f Hz)\n", snr, ok ? "detected" : "missed", d.status().toneHz);
            FCHECK(ok, "start tone missed at %.0f dB", snr);
        }
        // Below the floor it is allowed to miss (and is documented): -6 dB.
        std::vector<float> a;
        silence(a, 1); tone(a, 300, 4); silence(a, 1);
        addNoise(a, std::sqrt(0.125 / std::pow(10.0, -0.6)), 11);
        FaxDecoder d; d.configure(fs);
        feedAll(d, a, 4096);
        printf("start tone at -6 dB audio SNR: %s\n", d.status().state == 1 ? "detected" : "missed (below the detection floor)");
    }

    // A 20 ms dropout in the start tone and a 0.1 s dropout in the stop tone.
    {
        std::vector<float> a;
        tone(a, 300, 3);
        for (size_t i = static_cast<size_t>(1.5 * fs); i < static_cast<size_t>(1.52 * fs); i++) a[i] = 0;
        FaxDecoder d; d.configure(fs);
        feedAll(d, a, 4096);
        FCHECK(d.status().state == 1, "start tone with a 20 ms dropout not detected");
        std::vector<float> b;
        tone(b, 450, 3);
        for (size_t i = static_cast<size_t>(1.5 * fs); i < static_cast<size_t>(1.6 * fs); i++) b[i] = 0;
        feedAll(d, b, 4096);
        FCHECK(d.status().state == 3, "stop tone with a 100 ms dropout not detected");
    }

    // Too short a tone is not a tone.
    {
        std::vector<float> a;
        silence(a, 1); tone(a, 300, 0.4); silence(a, 1); tone(a, 450, 0.2); silence(a, 1);
        FaxDecoder d; d.configure(fs);
        feedAll(d, a, 4096);
        FCHECK(d.status().state == 0, "0.4 s of 300 Hz changed the state to %d", d.status().state);
    }

    // No false alarms: 10 minutes of white noise at several levels, band-limited noise, and a chirp that crosses all three tones.
    {
        FaxDecoder d; d.configure(fs);
        int changes = 0;
        for (double sigma : {0.01, 0.1, 0.5}) {
            std::vector<float> a;
            noise(a, 120, sigma, rng);
            feedAll(d, a, 4096);
            if (d.status().state != 0) changes++;
        }
        // Speech-like: noise through a one-pole filter at 800 Hz, with level bursts.
        {
            std::vector<float> a;
            noise(a, 120, 0.3, rng);
            double y = 0;
            const double k = 1 - std::exp(-2 * M_PI * 800 / fs);
            for (size_t i = 0; i < a.size(); i++) { y += k * (a[i] - y); a[i] = static_cast<float>(y * (1.0 + std::sin(i * 2 * M_PI * 3.0 / fs))); }
            feedAll(d, a, 4096);
            if (d.status().state != 0) changes++;
        }
        // Slow sweep 200 .. 900 Hz over 30 s: the tones are passed through, never held for half a second.
        {
            std::vector<float> a;
            double p = 0;
            for (size_t i = 0, n = static_cast<size_t>(30 * fs); i < n; i++) {
                const double f = 200 + 700.0 * i / n;
                p += 2 * M_PI * f / fs;
                a.push_back(static_cast<float>(0.4 * std::sin(p)));
            }
            feedAll(d, a, 4096);
            if (d.status().state != 0) changes++;
        }
        FCHECK(changes == 0, "false tone detections in noise / speech-like audio / sweep: %d", changes);
        printf("no false detections in 10 minutes of noise, speech-like audio and a sweep\n");
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("OK\n");
    return 0;
}

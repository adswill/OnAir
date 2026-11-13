// SPDX-License-Identifier: CC0-1.0
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tone detector: finds the strongest tone in the channel and reports its frequency offset and level. About 100 lines, no dependencies beyond the C ABI.
#include "onair_plugin.h"
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr uint32_t N = 4096;

struct Tone {
    const OnAirHostApi* host;
    double centre = 0, rate = 0;
    std::vector<float> re, im;   // collected block
    uint32_t fill = 0;
    uint64_t blocks = 0;
    std::vector<float> db;
};

void fft(std::vector<float>& re, std::vector<float>& im) {   // in-place radix-2
    const uint32_t n = (uint32_t)re.size();
    for (uint32_t i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (uint32_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / len;
        const float wr = (float)cos(ang), wi = (float)sin(ang);
        for (uint32_t i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (uint32_t k = 0; k < len / 2; k++) {
                const uint32_t a = i + k, b = i + k + len / 2;
                const float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
                const float nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

void* create(const OnAirHostApi* host, const char*) {
    auto* t = new Tone{host, 0, 0, std::vector<float>(N), std::vector<float>(N), 0, 0, std::vector<float>(N)};
    const OnAirColumn cols[] = {{"Time", ONAIR_COL_TIME}, {"Offset", ONAIR_COL_FREQ_HZ}, {"Level dBFS", ONAIR_COL_FLOAT}};
    host->table_define(host->ctx, "tones", "Strongest tone", 3, cols);
    host->status(host->ctx, "tone", "waiting for samples", ONAIR_LAMP_OFF);
    return t;
}
void destroy(void* p) { delete (Tone*)p; }
void configure(void* p, double centre, double rate) { auto* t = (Tone*)p; t->centre = centre; t->rate = rate; t->fill = 0; }

void process(void* p, const float* iq, uint32_t n, const OnAirIqMeta* meta) {
    auto* t = (Tone*)p;
    for (uint32_t i = 0; i < n; i++) {
        t->re[t->fill] = iq[2 * i]; t->im[t->fill] = iq[2 * i + 1];
        if (++t->fill < N) continue;
        t->fill = 0;
        if (++t->blocks % 8 != 0) continue;   // about 10 results a second at 400 ksps, fewer at lower rates
        for (uint32_t k = 0; k < N; k++) { const float w = 0.5f - 0.5f * (float)cos(2 * M_PI * k / N); t->re[k] *= w; t->im[k] *= w; }
        fft(t->re, t->im);
        uint32_t best = 0; float bestP = -1;
        for (uint32_t k = 0; k < N; k++) {
            const uint32_t s = (k + N / 2) % N;   // shifted: bin 0 = most negative frequency
            const float pw = t->re[k] * t->re[k] + t->im[k] * t->im[k];
            t->db[s] = 10.0f * log10f(pw / (float)N / (float)N + 1e-20f) + 6.02f;   // +6 dB: the Hann window passes a tone at half amplitude
            if (pw > bestP) { bestP = pw; best = s; }
        }
        const double hz = ((double)best - N / 2.0) * t->rate / N;
        const float level = t->db[best];
        char text[96];
        snprintf(text, sizeof text, "%+.1f Hz from centre, %.1f dBFS", hz, level);
        t->host->status(t->host->ctx, "tone", text, level > -60 ? ONAIR_LAMP_OK : ONAIR_LAMP_WARN);
        t->host->plot(t->host->ctx, "spectrum", "Spectrum", ONAIR_PLOT_SPECTRUM, t->db.data(), N, -t->rate / 2, t->rate / N);
        if (t->blocks % 80 == 0) {
            const OnAirCell cells[] = {{ONAIR_COL_TIME, 0, meta ? meta->timestamp : 0, nullptr}, {ONAIR_COL_FREQ_HZ, 0, hz, nullptr}, {ONAIR_COL_FLOAT, 0, level, nullptr}};
            t->host->table_row(t->host->ctx, "tones", 3, cells);
        }
    }
}

const OnAirPluginVTable kVt = {sizeof(OnAirPluginVTable), ONAIR_PLUGIN_ABI_VERSION, ONAIR_CAP_IQ_SINK, "tone-detector", create, destroy, configure, process, nullptr, nullptr, nullptr};
}  // namespace

extern "C" ONAIR_EXPORT const OnAirPluginVTable* onair_plugin_entry(uint32_t host_abi) { return host_abi >= ONAIR_PLUGIN_ABI_VERSION ? &kVt : nullptr; }

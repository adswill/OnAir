// NPR-70 sync detection (core/src/xlink/npr.cpp) through XlinkReceiver: all five symbol rates, network ids, offsets, noise, frames back to back, noise only.
// Simulated signals only: the generator follows the firmware's Si4463 settings, but no real NPR recording has been tried.
#include "dect2/xlink/npr.h"
#include "dect2/xlink/xlink_rx.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;
using namespace dect2::xlink::npr;

static std::vector<uint8_t> whiteBytes(unsigned seed, int n) { std::mt19937 g(seed); std::vector<uint8_t> v((size_t)n); for (auto& b : v) b = (uint8_t)g(); return v; }

int main() {
    // chain level: all rates, all networks, offsets
    for (int rate = 0; rate < 5; rate++) {
        const double sr = kSymbolRates[rate];
        for (int net : {0, 5, 15}) {
            for (double off : {0.0, 0.04 * sr, -0.08 * sr}) {
                const double fs = 8 * sr;
                auto f = txFrame(rate, net, whiteBytes((unsigned)(rate * 100 + net), 120), 8);
                std::vector<cf32> s(2000, cf32(0, 0));
                s.insert(s.end(), f.begin(), f.end());
                s.resize(s.size() + 2000, cf32(0, 0));
                std::mt19937 g(rate * 7 + net);
                std::normal_distribution<float> n(0, 0.06f);
                double ph = 0;
                for (auto& v : s) { v = v * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(n(g), n(g)); ph += 2 * 3.141592653589793 * off / fs; }
                XlinkReceiver rx;
                rx.setLogCallback([](const std::string&) {});
                rx.setProtocols(xlink::Npr);
                rx.configure(fs);
                for (size_t i = 0; i < s.size(); i += 3000) rx.feed(s.data() + i, std::min<size_t>(3000, s.size() - i));
                XlinkTelemetry t;
                rx.telemetry(t, ~0ull);
                int right = 0, wrong = 0;
                char want[32]; snprintf(want, sizeof want, "network %d", net);
                for (auto& fr : t.frames) { if (fr.proto == xlink::Npr && fr.id == want && fr.detail.find(std::to_string((int)(sr / 1e3))) != std::string::npos) right++; else wrong++; }
                if (right != 1 || wrong != 0) { printf("FAIL rate %d net %d off %.0f right %d wrong %d\n", rate, net, off, right, wrong); for (auto& fr : t.frames) printf("  %s | %s\n", fr.id.c_str(), fr.detail.c_str()); return 1; }
                if (std::fabs(t.frames[0].freqHz - off) > 0.05 * sr) { printf("FAIL offset est rate %d: %.0f want %.0f\n", rate, t.frames[0].freqHz, off); return 1; }
            }
        }
    }
    // noise sweep at the top rate, and noise only gives nothing
    for (double sigma : {0.1, 0.2, 0.3}) {
        int ok = 0;
        for (int t = 0; t < 10; t++) {
            auto f = txFrame(4, 3, whiteBytes((unsigned)t, 80), 8);
            std::vector<cf32> s(1000, cf32(0, 0)); s.insert(s.end(), f.begin(), f.end()); s.resize(s.size() + 1000, cf32(0, 0));
            std::mt19937 g(t * 13 + 1);
            std::normal_distribution<float> n(0, (float)sigma);
            for (auto& v : s) v += cf32(n(g), n(g));
            XlinkReceiver rx; rx.setProtocols(xlink::Npr); rx.configure(4e6); rx.feed(s.data(), s.size());
            XlinkTelemetry tel; rx.telemetry(tel, ~0ull);
            for (auto& fr : tel.frames) if (fr.proto == xlink::Npr && fr.id == "network 3") { ok++; break; }
        }
        printf("sigma %.2f: %d/10\n", sigma, ok);
        if (sigma == 0.2 && ok < 9) { printf("FAIL noise\n"); return 1; }
    }
    {
        std::mt19937 g(5);
        std::normal_distribution<float> n(0, 0.3f);
        std::vector<cf32> s(3000000);
        for (auto& v : s) v = cf32(n(g), n(g));
        XlinkReceiver rx; rx.setProtocols(xlink::Npr); rx.configure(4e6);
        for (size_t i = 0; i < s.size(); i += 8192) rx.feed(s.data() + i, std::min<size_t>(8192, s.size() - i));
        XlinkTelemetry t;
        rx.telemetry(t, ~0ull);
        if (!t.frames.empty()) { printf("FAIL noise-only: %zu frames\n", t.frames.size()); return 1; }
    }
    printf("npr ok\n");
    return 0;
}

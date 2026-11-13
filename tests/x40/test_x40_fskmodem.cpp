// Telephone-style FSK modems on a radio channel: Bell 103 (originate, answer), V.21 (channels 1 and 2), V.23 mode 2 and Bell 202, as audio tones in the upper or the lower sideband. Asynchronous bytes (8N1 and 8N2) with a
// dial error of up to +-30 Hz, a clock error of 1 %, noise and gaps between the bytes; the standard is named from the two tones, the bytes are compared with what was sent. Noise alone, a single tone,
// a tone pair that is not a standard and a burst with the wrong baud rate give no message. Simulated signals only: no recording of these modems was available.
#include "x40_gen4.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<X40Row> msgs(const X40Telemetry& t) { std::vector<X40Row> v; for (auto& r : t.rows) if (r.kind == "Message") v.push_back(r); return v; }
// the bytes of a message row: "N bytes: HH HH ... | ascii | ..."
static std::vector<int> bytesOf(const std::string& s) { std::vector<int> v; const size_t a = s.find(": "), b = s.find("| "); if (a == std::string::npos || b == std::string::npos) return v; std::string h = s.substr(a + 2, b - a - 2); for (size_t i = 0; i + 1 < h.size(); i += 3) v.push_back((int)strtol(h.substr(i, 2).c_str(), nullptr, 16)); return v; }
static std::vector<uint8_t> payload(unsigned seed, size_t n) { std::vector<uint8_t> v; Rng r(seed); for (size_t i = 0; i < n; i++) v.push_back((uint8_t)(32 + (int)std::fabs(r.g() * 30) % 90)); return v; }

int main() {
    const double fs = 48000;
    const auto data = payload(7, 24);
    // every standard, both sidebands, dial errors, 1 or 2 stop bits, a 1 % clock error, noise at 25 dB in 3 kHz
    for (int s = 0; s < 6; s++) for (bool lower : {false, true}) for (double dial : {0.0, 28.0, -30.0}) {
        const int stops = (s + (lower ? 1 : 0)) % 2 ? 2 : 1; const double ppm = dial > 0 ? 10000.0 : dial < 0 ? -10000.0 : 0.0;
        auto x = modemBurst(s, data, fs, 1.0, 1.0, 0.4, dial, lower, noiseFor(25, 3000, fs) * 0.25, 3 + (unsigned)s, ppm, stops, dial != 0 ? 1 : 0);
        auto t = run(x40::FskModem, x, fs);
        const auto m = msgs(t);
        CHECK(m.size() == 1, "%s lower %d dial %.0f: %zu messages", modemStd(s).name, (int)lower, dial, m.size());
        if (m.size() == 1) {
            CHECK(m[0].id.rfind(modemStd(s).name, 0) == 0, "%s: named %s", modemStd(s).name, m[0].id.c_str());
            const auto b = bytesOf(m[0].text);
            int bad = 0; for (size_t i = 0; i < data.size(); i++) if (i >= b.size() || b[i] != data[i]) bad++;
            CHECK(bad == 0 && b.size() >= data.size(), "%s lower %d dial %.0f: %d of %zu bytes wrong (%zu read)", modemStd(s).name, (int)lower, dial, bad, data.size(), b.size());
            CHECK(m[0].text.find(lower ? "lower sideband" : "upper sideband") != std::string::npos, "%s: sideband in '%s'", modemStd(s).name, m[0].text.c_str());
        }
    }
    // short bursts like the frames of CHU: 10 characters of 8N2 at 300 baud (0.37 s) with a short idle mark in front, every second, on the answer tones, with a dial error and noise
    { int frames = 0, perfect = 0, partial = 0;
      for (unsigned k = 0; k < 6; k++) { const auto d10 = payload(20 + k, 10); auto x = modemBurst(1, d10, fs, 0.6, 0.6, 0.06, k % 2 ? 22.0 : -17.0, false, noiseFor(22, 3000, fs) * 0.25, 40 + k, 0, 2, 0);
        auto t = run(x40::FskModem, x, fs); const auto m = msgs(t); frames++;
        if (m.size() == 1) { const auto b = bytesOf(m[0].text); int ok = 0; for (size_t i = 0; i < d10.size() && i < b.size(); i++) ok += b[i] == d10[i]; if (ok == 10 && b.size() == 10) perfect++; else if (ok >= 9) partial++; } }
      printf("  CHU-like 0.37 s frames: %d of %d read perfectly, %d with one byte missing\n", perfect, frames, partial);
      CHECK(perfect + partial >= 5 && perfect >= 4, "short frames: %d perfect %d partial of %d", perfect, partial, frames); }
    // noise sweep: bytes right of 24 (signal to noise in 3 kHz)
    printf("noise sweep (tone power to noise in 3 kHz): messages / bytes right of %zu\n", data.size());
    for (int s : {1, 5}) for (double snr : {30.0, 20.0, 14.0, 10.0, 6.0, 3.0}) {
        int nm = 0, good = 0;
        for (unsigned seed : {1u, 2u, 3u}) { auto x = modemBurst(s, data, fs, 1.0, 1.0, 0.4, 0, false, noiseFor(snr, 3000, fs) * 0.25, seed); auto t = run(x40::FskModem, x, fs); const auto m = msgs(t); nm += (int)m.size();
            if (!m.empty()) { const auto b = bytesOf(m[0].text); for (size_t i = 0; i < data.size() && i < b.size(); i++) good += b[i] == data[i]; } }
        printf("  %-18s %4.0f dB: %d messages, %d / %zu bytes right\n", modemStd(s).name, snr, nm, good, 3 * data.size());
        if (snr >= 14) CHECK(nm == 3 && good >= 3 * (int)data.size() - 3, "%s at %.0f dB: %d messages %d bytes right", modemStd(s).name, snr, nm, good);
    }
    // false reports: noise only, one tone, a tone pair that is not a standard, a 600 baud burst on the V.21 tones
    { std::vector<cf32> n((size_t)(fs * 20), cf32(0, 0)); addNoise(n, 0.01, 4); auto t = run(x40::FskModem, n, fs); CHECK(msgs(t).empty(), "noise: %zu messages", msgs(t).size()); }
    { std::vector<cf32> n((size_t)(fs * 10)); double ph = 0; for (auto& v : n) { ph += 2 * kPi * 1500.0 / fs; v = 0.5f * cf32((float)std::cos(ph), (float)std::sin(ph)); } addNoise(n, 0.001, 5); auto t = run(x40::FskModem, n, fs); CHECK(msgs(t).empty(), "one tone: %zu messages", msgs(t).size()); }
    { // two tones 700 Hz apart: not a standard
        std::vector<cf32> n((size_t)(fs * 6), cf32(0, 0)); double p1 = 0, p2 = 0; for (size_t i = (size_t)fs; i < (size_t)(5 * fs); i++) { p1 += 2 * kPi * 1000.0 / fs; p2 += 2 * kPi * 1700.0 / fs; n[i] = 0.3f * (cf32((float)std::cos(p1), (float)std::sin(p1)) + cf32((float)std::cos(p2), (float)std::sin(p2))); } addNoise(n, 0.001, 6);
        auto t = run(x40::FskModem, n, fs); CHECK(msgs(t).empty(), "two other tones: %zu messages", msgs(t).size()); }
    { auto x = modemBurst(2, data, fs, 1.0, 1.0, 0.4, 0, false, noiseFor(25, 3000, fs) * 0.25, 9, 0, 1, 0, 0.5, 2.0); auto t = run(x40::FskModem, x, fs);
      for (auto& r : msgs(t)) { const auto b = bytesOf(r.text); int ok = 0; for (size_t i = 0; i < data.size() && i < b.size(); i++) ok += b[i] == data[i]; CHECK(ok < 8, "600 baud burst read as 300 baud: %d bytes right", ok); } }
    printf(fails ? "x40 fskmodem: %d FAILED\n" : "x40 fskmodem: ok\n", fails);
    return fails ? 1 : 0;
}

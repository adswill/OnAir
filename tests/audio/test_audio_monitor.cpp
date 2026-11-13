// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Monitor audio": the tuned channel of any mode demodulated to the speaker / a second device / a TCP stream. A fake sink stands in for the sound
// devices: USB, LSB, NFM and AM tones at an offset from the capture centre, a wrong shift, yielding to a mode that plays, the TCP stream, device list.
#include "dect2/audio/audio_monitor.h"
#include "dect2/audio/audioout.h"
#include "dect2/audio/hf_demod.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double kRate = 240000;
static double goertzel(const std::vector<float>& s, double f, double fs) {   // left channel, power at f
    double c = 2 * std::cos(2 * M_PI * f / fs), a = 0, b = 0;
    for (size_t i = 0; i + 1 < s.size(); i += 2) { const double t = s[i] + c * a - b; b = a; a = t; }
    return (a * a + b * b - c * a * b) / std::max<size_t>(1, s.size() / 2);
}
// one second of the signal, the channel at `offHz` from the centre. kind: 0 USB tone, 1 LSB tone, 2 NFM, 3 AM; tone at 1 kHz
static std::vector<cf32> make(int kind, double offHz, double sec) {
    std::vector<cf32> x((size_t)(kRate * sec));
    double ph = 0;
    for (size_t i = 0; i < x.size(); i++) {
        const double t = i / kRate, tone = std::sin(2 * M_PI * 1000 * t);
        std::complex<double> v;
        if (kind == 0) v = std::polar(0.5, 2 * M_PI * 1000 * t);
        else if (kind == 1) v = std::polar(0.5, -2 * M_PI * 1000 * t);
        else if (kind == 2) { ph += 2 * M_PI * 3000 * tone / kRate; v = std::polar(0.5, ph); }
        else v = 0.5 * (1 + 0.6 * tone);
        v *= std::polar(1.0, 2 * M_PI * offHz * t);
        x[i] = cf32((float)v.real(), (float)v.imag());
    }
    return x;
}
static std::vector<float> run(int demod, int kind, double offHz, double shift) {
    AudioMonitor m;
    std::vector<float> out;
    m.setSink([&](const float* s, int n) { out.insert(out.end(), s, s + 2 * n); });
    MonitorConfig c; c.on = true; c.demod = demod; c.yield = false;
    m.configure(c);
    const auto x = make(kind, offHz, 1.0);
    for (size_t i = 0; i < x.size(); i += 24000) m.processBlock(x.data() + i, std::min<size_t>(24000, x.size() - i), kRate, shift);
    out.erase(out.begin(), out.begin() + std::min<size_t>(out.size(), 48000));   // the filters settle in the first 0.5 s
    return out;
}

int main() {
    const char* names[] = {"USB", "LSB", "NFM", "AM"};
    for (int d = 0; d < 4; d++) {
        const auto o = run(d, d, 30000, 30000);
        const double p1 = goertzel(o, 1000, 48000), p2 = goertzel(o, 2300, 48000);
        CHECK(!o.empty() && p1 > 1e-4 && p1 > 30 * p2, "%s: 1 kHz power %g vs 2.3 kHz %g (%zu samples)", names[d], p1, p2, o.size());
        // the same signal with the wrong shift: nothing of it comes out
        const auto w = run(d, d, 30000, -60000);   // (FM and AM scale an empty channel up to noise: only the sidebands are checked)
        if (d < 2) CHECK(goertzel(w, 1000, 48000) < p1 / 20, "%s: a wrong shift still hears the tone (%g vs %g)", names[d], goertzel(w, 1000, 48000), p1);
    }
    {   // a capture centred elsewhere: below the centre
        const auto o = run(HfDemodNfm, 2, -45000, -45000);
        CHECK(goertzel(o, 1000, 48000) > 1e-4, "NFM below the centre not heard");
    }
    {   // the mode plays its own sound: the monitor yields (silent), then speaks once the mode has been quiet
        AudioOut mode;
        std::vector<float> one(2 * 480, 0.1f);
        mode.write(one.data(), 480);
        AudioMonitor m; std::vector<float> out;
        m.setSink([&](const float* s, int n) { out.insert(out.end(), s, s + 2 * n); });
        MonitorConfig c; c.on = true; c.demod = HfDemodNfm; c.yield = true;
        m.configure(c);
        const auto x = make(2, 0, 0.5);
        m.processBlock(x.data(), x.size(), kRate, 0);
        CHECK(out.empty() && m.stats().yielded, "monitor did not yield to a playing mode");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        m.processBlock(x.data(), x.size(), kRate, 0);
        CHECK(!out.empty() && !m.stats().yielded, "monitor stayed silent after the mode went quiet");
        mode.setMuted(true);   // a muted mode does not count as playing
        mode.write(one.data(), 480);
        out.clear();
        m.processBlock(x.data(), x.size(), kRate, 0);
        CHECK(!out.empty(), "a muted mode silenced the monitor");
        m.setMuted(true);   // the app's mute reaches the monitor's speaker too
        out.clear();
        m.processBlock(x.data(), x.size(), kRate, 0);
        CHECK(out.empty(), "the monitor played while the app was muted");
        m.setMuted(false);
        m.processBlock(x.data(), x.size(), kRate, 0);
        CHECK(!out.empty(), "the monitor stayed silent after unmuting");
    }
    {   // the TCP stream: WAV header, then 16-bit stereo; bound to the loopback address
        const int port = 17300 + (int)(std::chrono::steady_clock::now().time_since_epoch().count() % 500);
        AudioMonitor m; std::vector<float> out;
        m.setSink([&](const float* s, int n) { out.insert(out.end(), s, s + 2 * n); });
        MonitorConfig c; c.on = true; c.demod = HfDemodNfm; c.yield = false; c.net = true; c.port = port; c.wav = true;
        m.configure(c);
        const auto x = make(2, 0, 0.4);
        m.processBlock(x.data(), 24000, kRate, 0);   // opens the listener
#ifdef _WIN32
        WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
        int s = (int)socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        const bool ok = connect(s, (sockaddr*)&a, sizeof a) == 0;
        CHECK(ok, "cannot connect to the audio stream on 127.0.0.1:%d (%s)", port, m.stats().error.c_str());
        if (ok) {
            m.processBlock(x.data() + 24000, 48000, kRate, 0);   // accepts the listener, sends the header and audio
            char buf[4096]; int got = 0;
            for (int t = 0; t < 50 && got < 44 + 1000; t++) {
#ifdef _WIN32
                const int r = recv(s, buf + got, (int)sizeof buf - got, 0);
#else
                const int r = (int)recv(s, buf + got, sizeof buf - got, MSG_DONTWAIT);
#endif
                if (r > 0) got += r; else std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            CHECK(got >= 44 + 1000 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4) && !memcmp(buf + 36, "data", 4), "no WAV header and audio on the stream (%d bytes)", got);
            CHECK(m.stats().clients == 1, "client count %d", m.stats().clients);
        }
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
    }
    {   // the device list and a device that does not exist (falls back to the default)
        const auto dev = AudioOut::devices();
        printf("output devices: %zu\n", dev.size());
        AudioOut o; o.setDevice("no such device"); o.setMonitorRole(true);
        CHECK(o.start(48000), "a missing device did not fall back to the default");
        o.stop();
    }
    printf(fails ? "audio_monitor: %d FAILED\n" : "audio_monitor: all passed\n", fails);
    return fails ? 1 : 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sound-card IQ source: AudioIn Tone/Silence through USB analytic IQ, and listAudioInputs() does not crash.
#include "dect2/audio/audioin.h"
#include "dect2/source/source.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "dect2/common/mathconst.h"

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void testList() {
    const auto v = listAudioInputs();
    CHECK(!v.empty() && v[0].name == "Default input" && v[0].id.empty(), "default input first (%zu)", v.size());
    const auto d = listAudioDevices();
    CHECK(!d.empty() && d[0].isAudio() && d[0].name.find("Sound card:") == 0, "listAudioDevices names a sound card");
}

static double goertzel(const std::vector<cf32>& x, int skip, double hz, double rate, bool pos) {
    const double w = (pos ? -1 : 1) * 2 * dect2::math::kPi * hz / rate;
    cf32 acc = 0;
    for (size_t i = (size_t)skip; i < x.size(); i++) {
        const float c = (float)std::cos(w * (double)(i - (size_t)skip)), s = (float)std::sin(w * (double)(i - (size_t)skip));
        acc += x[i] * cf32(c, s);
    }
    return std::norm(acc);
}

static void testUsbTone() {
    AudioIn in;
    std::string err;
    CHECK(in.start(AudioIn::Tone, 48000, {}, 1500, err), "Tone start: %s", err.c_str());
    const int n = 2048;
    std::vector<float> st((size_t)n * 2);
    in.read(st.data(), n);
    AudioHilbert h;
    std::vector<cf32> iq((size_t)n);
    audioStereoToIq(st.data(), n, AudioIqMode::Usb, iq.data(), h);
    double p = 0, q = 0;
    for (int i = 80; i < n; i++) { p += (double)iq[i].real() * iq[i].real(); q += (double)iq[i].imag() * iq[i].imag(); }
    CHECK(p > 1 && q > 1, "USB analytic IQ has energy on I and Q (I %g Q %g)", p, q);
    const double pos = goertzel(iq, 80, 1500, 48000, true), neg = goertzel(iq, 80, 1500, 48000, false);
    CHECK(pos > 8 * neg, "USB puts the 1500 Hz tone on the positive side (pos %g neg %g)", pos, neg);

    AudioHilbert h2;
    audioStereoToIq(st.data(), n, AudioIqMode::Lsb, iq.data(), h2);
    const double posL = goertzel(iq, 80, 1500, 48000, true), negL = goertzel(iq, 80, 1500, 48000, false);
    CHECK(negL > 8 * posL, "LSB puts the tone on the negative side (pos %g neg %g)", posL, negL);

    AudioHilbert h3;
    audioStereoToIq(st.data(), n, AudioIqMode::MonoReal, iq.data(), h3);
    double qi = 0;
    for (int i = 0; i < n; i++) qi += (double)iq[i].imag() * iq[i].imag();
    CHECK(qi < 1e-6, "mono real has Q = 0 (%g)", qi);
    in.stop();
}

static void testSourceTone() {
    DeviceInfo d;
    d.kind = DeviceInfo::Audio;
    d.serial = "tone:1500";
    d.name = "Sound card: test tone";
    auto src = makeAudioSource(d);
    CHECK(src != nullptr, "makeAudioSource");
    TuneSettings t;
    t.radio["audio_iq"] = "1";   // USB
    IqRing ring(1u << 16);
    std::string err;
    CHECK(src->start(t, ring, err), "audio source start: %s", err.c_str());
    CHECK(std::fabs(src->sampleRate() - 48000) < 1, "reports 48 kHz (%g)", src->sampleRate());
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::vector<cf32> x(4096);
    const size_t got = ring.read(x.data(), x.size());
    CHECK(got > 200, "source produced samples (%zu)", got);
    x.resize(got);
    const double pos = goertzel(x, 80, 1500, src->sampleRate(), true), neg = goertzel(x, 80, 1500, src->sampleRate(), false);
    CHECK(pos > 4 * neg, "source USB tone on the positive side (pos %g neg %g, n %zu)", pos, neg, got);
    src->stop();

    d.serial = "silence";
    src = makeAudioSource(d);
    t.radio["audio_iq"] = "0";
    CHECK(src->start(t, ring, err), "silence start: %s", err.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    src->stop();
}

int main() {
    testList();
    testUsbTone();
    testSourceTone();
    if (fails) { printf("audio_source: %d checks FAILED\n", fails); return 1; }
    printf("audio_source: all checks passed\n");
    return 0;
}

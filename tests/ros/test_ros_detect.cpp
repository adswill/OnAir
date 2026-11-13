// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ROS identifier (dect2/ros/ros_detect.h): test signals with the publicly reported physical layer (random tones on a 15.625 Hz grid of 144,
// 4 / 8 / 16 baud; NOT ROS encoding, which is not public) must be found with the right sub-mode under noise and offsets, in USB audio and in
// complex samples with a neighbour; noise, a steady carrier and other MFSK modes (Olivia, MFSK16, JT65-like) must not be called ROS; the
// signal identifier must rank ROS first on the test signal and never propose it on noise.
#include "dect2/ros/ros_detect.h"
#include "dect2/engine/sig_ident.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "../common/test_util.h"

using namespace dect2;

namespace {
constexpr double kPi = 3.14159265358979323846;

// USB audio at fs: the real part of the complex test signal (centre in audio Hz), white noise for snrDb in 2500 Hz
std::vector<float> audio(const std::vector<cf32>& s, double fs, double snrDb, std::mt19937& g) {
    std::vector<float> a(s.size());
    const double sig2 = 0.5, noise2 = sig2 * (fs / 2) / 2500.0 / std::pow(10.0, snrDb / 10);
    std::normal_distribution<double> nd(0, std::sqrt(noise2));
    for (size_t i = 0; i < s.size(); i++) a[i] = (float)(s[i].real() + nd(g));
    return a;
}
void addComplexNoise(std::vector<cf32>& s, double fs, double snrDb, std::mt19937& g) {
    const double noise2 = 1.0 * fs / 2500.0 / std::pow(10.0, snrDb / 10);
    std::normal_distribution<double> nd(0, std::sqrt(noise2 / 2));
    for (cf32& v : s) v += cf32((float)nd(g), (float)nd(g));
}

}  // namespace

int main() {
    const double fs = 12000;
    std::mt19937 g(1234);
    std::uniform_real_distribution<double> off(-80, 80);
    // ---- the three reported HF sub-modes in USB audio, offset up to +-80 Hz, at +10 / 0 / -6 dB SNR in 2.5 kHz
    struct Want { double baud, secs; const char* id; };
    for (const Want& w : {Want{16, 4, "ros16"}, Want{8, 6, "ros8"}, Want{4, 10, "ros4"}}) {
        for (double snr : {10.0, 0.0, -6.0}) {
            int ok = 0, found = 0;
            const int trials = 8;
            double spacing = 0;
            for (int t = 0; t < trials; t++) {
                const auto s = ros::synthReportedGrid(w.baud, w.secs, fs, 1550 + off(g), 100u + t * 7u + (uint32_t)w.baud);
                const auto a = audio(s, fs, snr, g);
                const ros::Detection d = ros::detectAudio(a.data(), a.size(), fs);
                if (d.ros) found++;
                if (d.ros && d.subId == w.id) { ok++; spacing += d.spacingHz; }
                if (t == 0 && snr == 0) printf("  %s %+.0f dB: %s -> %s\n", w.id, snr, d.why.c_str(), d.ros ? d.name.c_str() : "not ROS");
            }
            if (ok) spacing /= ok;
            printf("%s at %+.0f dB: %d / %d right, %d found, grid %.3f Hz\n", w.id, snr, ok, trials, found, spacing);
            CHECK(ok >= trials - 1, "%s at %.0f dB: only %d of %d", w.id, snr, ok, trials);
            if (ok) CHECK(std::fabs(spacing - 15.625) < 0.1, "%s: grid %.3f Hz, not 15.625", w.id, spacing);
        }
    }
    // ---- complex samples at 48 kHz: the signal 7 kHz off the centre, a strong carrier 3 kHz above it, 0 dB SNR
    {
        int ok = 0;
        for (int t = 0; t < 5; t++) {
            const double f48 = 48000;
            auto s = ros::synthReportedGrid(16, 4, f48, 7000 + off(g), 900u + t);
            for (size_t i = 0; i < s.size(); i++) s[i] += 3.0f * cf32((float)std::cos(2 * kPi * 10000 * i / f48), (float)std::sin(2 * kPi * 10000 * i / f48));
            addComplexNoise(s, f48, 0, g);
            ros::DetectOptions o; o.centreHz = 7000;
            const ros::Detection d = ros::detect(s.data(), s.size(), f48, o);
            if (d.ros && d.subId == "ros16") ok++;
            if (t == 0) printf("  complex 48 kHz: %s -> %s\n", d.why.c_str(), d.ros ? d.name.c_str() : "not ROS");
        }
        CHECK(ok >= 4, "complex 48 kHz with a neighbour: %d of 5", ok);
    }
    // ---- a slow drift (0.15 Hz/s over 6 s, 0.9 Hz in all) at 0 dB: still found
    {
        int ok = 0;
        for (int t = 0; t < 4; t++) {
            auto s = ros::synthReportedGrid(8, 6, fs, 1550 + off(g), 4000u + t);
            for (size_t i = 0; i < s.size(); i++) { const double tt = i / fs; s[i] *= cf32((float)std::cos(kPi * 0.15 * tt * tt), (float)std::sin(kPi * 0.15 * tt * tt)); }
            const auto a = audio(s, fs, 0, g);
            const ros::Detection d = ros::detectAudio(a.data(), a.size(), fs);
            if (d.ros && d.subId == "ros8") ok++;
        }
        CHECK(ok >= 3, "drifting 8 Bd: %d of 4", ok);
    }
    // ---- a rate that is not a reported one: ROS-like, weak, no sub-mode
    {
        const auto s = ros::synthReportedGrid(11, 5, fs, 1550, 77);
        const auto a = audio(s, fs, 10, g);
        const ros::Detection d = ros::detectAudio(a.data(), a.size(), fs);
        CHECK(d.ros && d.weak && d.subId.empty(), "11 Bd on the grid: ros %d weak %d sub '%s'", d.ros, d.weak, d.subId.c_str());
    }
    // ---- not ROS
    int fp = 0;
    for (int t = 0; t < 40; t++) {   // noise only
        std::vector<cf32> z((size_t)(fs * 4), cf32(0, 0));
        const auto a = audio(z, fs, 0, g);
        if (ros::detectAudio(a.data(), a.size(), fs).ros) fp++;
    }
    CHECK(fp == 0, "noise called ROS %d times of 40", fp);
    struct Other { const char* name; double baud; int tones; double spacing; };
    for (const Other& m : {Other{"Olivia 32/1000", 31.25, 32, 31.25}, Other{"Olivia 32/2000", 62.5, 32, 62.5}, Other{"Olivia 8/250", 31.25, 8, 31.25},
                           Other{"MFSK16", 15.625, 16, 15.625}, Other{"MFSK8", 7.8125, 32, 7.8125}, Other{"JT65-like", 2.69, 65, 2.69},
                           Other{"FT8-like", 6.25, 8, 6.25}}) {
        int hits = 0;
        for (int t = 0; t < 6; t++) {
            const auto s = ros::synthReportedGrid(m.baud, 6, fs, 1550 + off(g), 300u + t, m.tones, m.spacing);
            const auto a = audio(s, fs, 10, g);
            const ros::Detection d = ros::detectAudio(a.data(), a.size(), fs);
            if (d.ros) hits++;
            if (t == 0) printf("  %s: %s\n", m.name, d.why.c_str());
        }
        CHECK(hits == 0, "%s called ROS %d times of 6", m.name, hits);
    }
    {   // a steady carrier and a slow sweep
        std::vector<cf32> s((size_t)(fs * 4));
        double ph = 0;
        for (size_t i = 0; i < s.size(); i++) { s[i] = cf32((float)std::cos(ph), (float)std::sin(ph)); ph += 2 * kPi * (1000 + 300.0 * i / s.size()) / fs; }
        const auto a = audio(s, fs, 10, g);
        CHECK(!ros::detectAudio(a.data(), a.size(), fs).ros, "a sweeping carrier called ROS");
    }
    // ---- the signal identifier (complex samples at 48 kHz, 14.103 MHz): ROS first on the test signal, never proposed on noise
    {
        const double f48 = 48000;
        int first = 0;
        for (int t = 0; t < 4; t++) {
            auto s = ros::synthReportedGrid(16, sigid::captureSeconds(f48), f48, 5000, 500u + t);
            addComplexNoise(s, f48, 10, g);
            sigid::Options o; o.rfHz = 14.103e6 - 5000; o.loHz = 3700; o.hiHz = 6300;
            const sigid::Result r = sigid::identify(s.data(), s.size(), f48, o);
            if (!r.cands.empty() && r.cands[0].name == "ROS") first++;
            if (t == 0) printf("  identifier: %s | %s\n", r.headline.c_str(), r.cands.empty() ? "-" : (r.cands[0].name + ": " + r.cands[0].why).c_str());
        }
        CHECK(first >= 3, "identifier put ROS first %d of 4", first);
        int bad = 0;
        for (int t = 0; t < 6; t++) {
            std::vector<cf32> z((size_t)(f48 * sigid::captureSeconds(f48)), cf32(0, 0));
            addComplexNoise(z, f48, 0, g);
            sigid::Options o; o.rfHz = 14.1e6; o.loHz = -1200; o.hiHz = 1200;
            const sigid::Result r = sigid::identify(z.data(), z.size(), f48, o);
            for (const auto& c : r.cands) if (c.name == "ROS") bad++;
        }
        CHECK(bad == 0, "identifier proposed ROS on noise %d times", bad);
    }
    size_t n = 0;
    const ros::SubModeInfo* sm = ros::subModes(n);
    CHECK(n >= 3 && !strcmp(sm[0].id, "ros16") && sm[0].identified, "sub-mode table");
    return testSummary("ros_detect");
}

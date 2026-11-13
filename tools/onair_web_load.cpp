// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// onair-web-load: a load test of OnAir Web without a radio or a network. Many fake listeners (each tuned to its own channel, with audio and
// a waterfall) inside a synthetic capture (noise plus FM carriers); the hub runs on one thread so the processor time per listener can be read
// directly. Prints, for each listener count, the processor time per second of signal (100 % = one core) and the data each listener gets.
//
//   onair-web-load [--rate HZ] [--users 1,8,32] [--seconds S] [--mode nfm|am|usb|lsb|cw|wfm] [--codec adpcm|pcm16] [--arate HZ] [--no-spectrum]
#include "web/web_hub.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <sstream>
#include <string>
#include <vector>
using namespace onair_web;

static ClientMsg msg(const std::string& j) {
    ClientMsg m; std::string c, w;
    if (!parseClientMsg(j, m, c, w)) fprintf(stderr, "bad message %s: %s\n", j.c_str(), w.c_str());
    return m;
}

int main(int argc, char** argv) {
    double rate = 2.4e6, seconds = 5;
    std::string users = "0,1,4,16,32", mode = "nfm", codec = "adpcm";
    int arate = 12000;
    bool spectrum = true;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--rate") rate = atof(next().c_str());
        else if (a == "--users") users = next();
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--mode") mode = next();
        else if (a == "--codec") codec = next();
        else if (a == "--arate") arate = atoi(next().c_str());
        else if (a == "--no-spectrum") spectrum = false;
        else { fprintf(stderr, "usage: onair-web-load [--rate HZ] [--users 1,8,32] [--seconds S] [--mode nfm] [--codec adpcm|pcm16] [--arate HZ] [--no-spectrum]\n"); return 2; }
    }
    // the synthetic capture: noise and 12 NFM carriers with tones
    const double centre = 145e6;
    const size_t chunk = 65536, total = (size_t)(rate * seconds);
    std::vector<cf32> iq(total);
    {
        std::mt19937 rng(1);
        std::normal_distribution<float> nd(0.f, 0.003f);
        const int nc = 12;
        for (size_t i = 0; i < total; i++) {
            const double t = (double)i / rate;
            cf32 v(nd(rng), nd(rng));
            for (int c = 0; c < nc; c++) {
                const double f = (c - nc / 2) * rate / (nc + 2);
                const double ph = 2 * M_PI * f * t + (2500.0 / (400 + 50 * c)) * std::sin(2 * M_PI * (400 + 50 * c) * t);
                v += cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.02f;
            }
            iq[i] = v;
        }
    }
    printf("OnAir Web load: capture %.3f Msps, %.0f s of signal, mode %s, audio %s %d Hz, spectrum %s\n", rate / 1e6, seconds, mode.c_str(), codec.c_str(), arate, spectrum ? "1024 bins 10 fps" : "off");
    printf("%8s %14s %16s %14s\n", "users", "CPU (1 core)", "per user", "kbit/s each");
    double base = -1;
    std::stringstream ss(users);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const int n = atoi(item.c_str());
        WebHub h;
        HubLimits L;
        L.maxUsers = n + 1; L.maxPerIp = n + 1; L.maxKbpsUser = 100000; L.maxKbpsTotal = 10000000; L.queueBytes = 1 << 20; L.maxFps = 30;
        h.setLimits(L);
        h.pushIq(iq.data(), 1024, centre, rate);   // the capture is known
        h.processPending();
        std::vector<int> ids;
        for (int u = 0; u < n; u++) {
            AdmitRequest a; a.ip = "10.0.0." + std::to_string(u);
            std::string c, w;
            const int id = h.admit(a, c, w);
            if (id <= 0) { fprintf(stderr, "admit: %s\n", w.c_str()); return 1; }
            ids.push_back(id);
            const double f = centre + (u % 12 - 6) * rate / 14 + (u / 12) * 2500;
            h.handle(id, msg("{\"type\":\"tune\",\"freq_hz\":" + std::to_string((long long)f) + ",\"mode\":\"" + mode + "\"}"));
            h.handle(id, msg("{\"type\":\"audio\",\"on\":true,\"codec\":\"" + codec + "\",\"rate\":" + std::to_string(arate) + "}"));
            if (spectrum) h.handle(id, msg("{\"type\":\"spectrum\",\"on\":true,\"bins\":1024,\"fps\":10}"));
        }
        if (n == 0) {   // the cost of the shared part alone: one listener with a waterfall and no audio
            AdmitRequest a; a.ip = "10.0.0.250";
            std::string c, w;
            ids.push_back(h.admit(a, c, w));
            if (spectrum) h.handle(ids[0], msg("{\"type\":\"spectrum\",\"on\":true,\"bins\":1024,\"fps\":10}"));
        }
        size_t bytes = 0;
        std::vector<OutMsg> out;
        const std::clock_t c0 = std::clock();
        for (size_t i = 0; i < total; i += chunk) {
            const size_t k = std::min(chunk, total - i);
            h.pushIq(iq.data() + i, k, centre, rate);
            h.processPending();
            for (int id : ids) { out.clear(); std::string kick; h.drain(id, out, kick); for (auto& m : out) bytes += m.data.size(); }
        }
        const double cpu = (double)(std::clock() - c0) / CLOCKS_PER_SEC;
        const double pct = cpu / seconds * 100.0;
        if (n == 0) { base = pct; printf("%8s %13.1f%% %16s %14s\n", "shared", pct, "-", "-"); continue; }
        const double per = base >= 0 ? (pct - base) / n : pct / n;
        printf("%8d %13.1f%% %15.2f%% %14.1f\n", n, pct, per, (double)bytes * 8 / 1000 / seconds / n);
    }
    return 0;
}

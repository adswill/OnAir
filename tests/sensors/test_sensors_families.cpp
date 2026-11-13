// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One real recording per protocol family of rtl_433's test set (tests/sensors/families.txt, reduced to its pulse trains by
// tools/sensors_make_families.py) rebuilt as an IQ signal (OOK: carrier on during the pulses; FSK: tone at the mark / space frequency) and run through
// the whole receiver: pulse detector, slicers, the decoders. Each family must be decoded as the model rtl_433 reports for the recording.
#include "dect2/sensors/sensors_rx.h"
#include "dect2/sensors/sensors_tel.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
using namespace dect2;
#include "../common/test_util.h"

struct Entry { std::string fam, model, id, type; double f1 = 0, f2 = 0; std::vector<int> p; };

static std::vector<Entry> load() {
    std::vector<Entry> v;
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/tests/sensors/families.txt");
    std::string l;
    while (std::getline(in, l)) {
        std::vector<std::string> f;
        std::stringstream ss(l);
        std::string t;
        while (std::getline(ss, t, '|')) f.push_back(t);
        if (f.size() < 7) continue;
        Entry e;
        e.fam = f[0]; e.model = f[1]; e.id = f[2]; e.type = f[3]; e.f1 = atof(f[4].c_str()); e.f2 = atof(f[5].c_str());
        std::stringstream ps(f[6]);
        while (std::getline(ps, t, ',')) e.p.push_back(atoi(t.c_str()));
        v.push_back(std::move(e));
    }
    return v;
}

static std::vector<cf32> synth(const Entry& e, double rate) {
    std::vector<cf32> x(size_t(0.05 * rate), cf32(0, 0));
    const double sps = rate / 1e6;
    double ph = 0;
    for (size_t i = 0; i + 1 < e.p.size(); i += 2) {
        for (int k = 0; k < 2; k++) {
            const size_t n = (size_t)std::lround(e.p[i + k] * sps);
            const bool high = k == 0;
            const double hz = e.type == "fsk" ? (high ? e.f1 : e.f2) : 0;
            for (size_t j = 0; j < n; j++) {
                ph += 2 * math::kPi * hz / rate;
                const float a = e.type == "fsk" ? 0.5f : (high ? 0.5f : 0.f);
                x.push_back(cf32(a * (float)std::cos(ph), a * (float)std::sin(ph)));
            }
        }
    }
    x.insert(x.end(), size_t(0.08 * rate), cf32(0, 0));
    return x;
}

int main() {
    const auto es = load();
    CHECK(es.size() >= 20, "families table has %zu packages", es.size());
    std::map<std::string, int> famOk, famTotal;
    for (const auto& e : es) {
        int minW = 1 << 30;
        for (int w : e.p) if (w > 0) minW = std::min(minW, w);
        const double rate = minW < 80 ? 1e6 : 250e3;   // fast codes (smart meters, 30 us bits) need a faster input
        SensorsReceiver rx;
        rx.setAutoLevel(true);
        rx.configure(rate);
        // the packages of a recording are separated: every entry is run alone, so a family counts when one package decodes
        std::vector<cf32> x = synth(e, rate);
        const size_t blk = 8192;
        for (size_t o = 0; o < x.size(); o += blk) rx.feed(x.data() + o, std::min(blk, x.size() - o));
        rx.flush();
        SensorsTelemetry t;
        rx.telemetry(t, 0);
        bool hit = false;
        for (const auto& d : t.devices) if (d.model == e.model) hit = true;
        famTotal[e.fam]++;
        if (hit) famOk[e.fam]++;
        printf("  %-34s %-26s %s %s\n", e.fam.c_str(), e.model.c_str(), e.type.c_str(), hit ? "ok" : "no");
    }
    for (const auto& kv : famTotal) CHECK(famOk[kv.first] >= 1, "family %s: no recording decoded", kv.first.c_str());
    return testSummary("sensors families");
}

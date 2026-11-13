// SPDX-License-Identifier: CC0-1.0
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Burst counter: counts bursts of signal above a threshold over the noise floor, with timestamps and durations.
#include "onair_plugin.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
struct Counter {
    const OnAirHostApi* host;
    double rate = 0, noise = -1;   // noise: smoothed linear power floor (-1 = unknown)
    bool inBurst = false;
    double startTime = 0, peak = 0, burstSec = 0;
    uint64_t count = 0;
    uint32_t acc = 0; double sum = 0;   // power of 256-sample slices
};

void* create(const OnAirHostApi* host, const char*) {
    auto* c = new Counter{host};
    const OnAirColumn cols[] = {{"#", ONAIR_COL_INT}, {"Start", ONAIR_COL_TIME}, {"Length ms", ONAIR_COL_FLOAT}, {"Peak dB over floor", ONAIR_COL_FLOAT}};
    host->table_define(host->ctx, "bursts", "Bursts", 4, cols);
    host->status(host->ctx, "count", "0 bursts", ONAIR_LAMP_OFF);
    return c;
}
void destroy(void* p) { delete (Counter*)p; }
void configure(void* p, double, double rate) { ((Counter*)p)->rate = rate; }

void process(void* p, const float* iq, uint32_t n, const OnAirIqMeta* meta) {
    auto* c = (Counter*)p;
    const double thr = c->host->get_setting_number(c->host->ctx, "threshold_db", 10.0);
    const double minMs = c->host->get_setting_number(c->host->ctx, "min_ms", 1.0);
    constexpr uint32_t S = 256;
    for (uint32_t i = 0; i < n; i++) {
        c->sum += (double)iq[2 * i] * iq[2 * i] + (double)iq[2 * i + 1] * iq[2 * i + 1];
        if (++c->acc < S) continue;
        const double pw = c->sum / S + 1e-20;
        c->acc = 0; c->sum = 0;
        const double t = meta->timestamp + (double)i / (meta->sample_rate > 0 ? meta->sample_rate : 1);
        if (c->noise < 0) c->noise = pw;
        const double over = 10 * log10(pw / c->noise);
        const double slice = S / (c->rate > 0 ? c->rate : 1);
        if (!c->inBurst) {
            if (over > thr) { c->inBurst = true; c->startTime = t; c->peak = over; c->burstSec = slice; }
            else c->noise += 0.02 * (pw - c->noise);   // the floor follows only quiet slices
        } else if (over > thr - 3) {   // 3 dB of hysteresis
            c->peak = std::max(c->peak, over); c->burstSec += slice;
        } else {
            c->inBurst = false;
            if (c->burstSec * 1000 >= minMs) {
                c->count++;
                const OnAirCell cells[] = {{ONAIR_COL_INT, (int64_t)c->count, 0, nullptr}, {ONAIR_COL_TIME, 0, c->startTime, nullptr}, {ONAIR_COL_FLOAT, 0, c->burstSec * 1000, nullptr}, {ONAIR_COL_FLOAT, 0, c->peak, nullptr}};
                c->host->table_row(c->host->ctx, "bursts", 4, cells);
                char text[48]; snprintf(text, sizeof text, "%llu bursts", (unsigned long long)c->count);
                c->host->status(c->host->ctx, "count", text, ONAIR_LAMP_OK);
                c->host->emit_event(c->host->ctx, "burst", 0, nullptr);
            }
        }
    }
}

const OnAirPluginVTable kVt = {sizeof(OnAirPluginVTable), ONAIR_PLUGIN_ABI_VERSION, ONAIR_CAP_IQ_SINK, "burst-counter", create, destroy, configure, process, nullptr, nullptr, nullptr};
}  // namespace

extern "C" ONAIR_EXPORT const OnAirPluginVTable* onair_plugin_entry(uint32_t host_abi) { return host_abi >= ONAIR_PLUGIN_ABI_VERSION ? &kVt : nullptr; }

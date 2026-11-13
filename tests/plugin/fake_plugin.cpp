// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A fake plugin for the plugin tests. Its behaviour follows the mode id it is created with. Compiled twice: normally, and with FAKE_BADABI
// (claims plugin ABI 99, which the host must refuse).
#include "onair_plugin.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

namespace {
struct F { const OnAirHostApi* h; std::string mode; uint64_t samples = 0; uint64_t calls = 0; };

void* create(const OnAirHostApi* h, const char* mode) {
    auto* f = new F{h, mode};
    if (f->mode == "refuse") { delete f; return nullptr; }
    if (f->mode == "hang-create") for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (f->mode == "perm") {
        float pcm[8] = {};
        char dir[512] = "";
        const int a = h->audio_out(h->ctx, pcm, 8, 48000);
        const int d = h->data_dir(h->ctx, dir, sizeof dir);
        char t[700];
        snprintf(t, sizeof t, "audio=%d dir=%d path=%s", a, d, dir);
        h->status(h->ctx, "perm", t, ONAIR_LAMP_OK);
    }
    if (f->mode == "flood") {
        const OnAirColumn cols[3] = {{"a", ONAIR_COL_TEXT}, {"b", ONAIR_COL_INT}, {"c", ONAIR_COL_FLOAT}};
        int tablesOk = 0;
        for (int t = 0; t < 20; t++) { char id[16]; snprintf(id, sizeof id, "t%d", t); if (h->table_define(h->ctx, id, "T", 3, cols) == 0) tablesOk++; }
        std::string big(20000, 'x');
        for (int r = 0; r < 5000; r++) { OnAirCell c[3] = {{ONAIR_COL_TEXT, 0, 0, big.c_str()}, {ONAIR_COL_INT, r, 0, nullptr}, {ONAIR_COL_FLOAT, 0, 1.5, nullptr}}; h->table_row(h->ctx, "t0", 3, c); }
        std::vector<float> huge(300000, 1.0f);
        const int pr = h->plot(h->ctx, "p", "P", ONAIR_PLOT_SERIES, huge.data(), (uint32_t)huge.size(), 0, 1);
        std::vector<uint8_t> img(8 << 20, 0);
        const int ir = h->image(h->ctx, "i", ONAIR_IMG_RGBA, 2048, 1024, img.data(), (uint32_t)img.size());
        const int br = h->image(h->ctx, "j", ONAIR_IMG_RGBA, 10, 10, img.data(), 12);   // size does not match the dimensions
        const int mr = h->map_point(h->ctx, "m", "x", 200.0, 0.0);
        char t[200]; snprintf(t, sizeof t, "tables=%d plot=%d img=%d badimg=%d map=%d", tablesOk, pr, ir, br, mr);
        h->status(h->ctx, "flood", t, ONAIR_LAMP_OK);
    }
    return f;
}
void destroy(void* p) { delete (F*)p; }
void process(void* p, const float*, uint32_t n, const OnAirIqMeta* m) {
    auto* f = (F*)p;
    f->samples += n; f->calls++;
    if (f->mode == "loop") for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (f->mode == "crash") *(volatile int*)0 = 1;
    if (f->mode == "gain") {
        char t[64]; snprintf(t, sizeof t, "gain=%.2f", f->h->get_setting_number(f->h->ctx, "gain", -1));
        f->h->status(f->h->ctx, "gain", t, ONAIR_LAMP_OK);
    }
    if (f->mode == "event") {
        OnAirField fl[2] = {{"n", 0, (double)f->samples, nullptr}, {"who", 1, 0, "fake"}};
        f->h->emit_event(f->h->ctx, "Hello World", 2, fl);
    }
    char t[96]; snprintf(t, sizeof t, "%llu samples at %.0f Hz / %.0f sps", (unsigned long long)f->samples, m->centre_hz, m->sample_rate);
    f->h->status(f->h->ctx, "n", t, ONAIR_LAMP_OK);
}
void configure(void* p, double c, double r) { auto* f = (F*)p; char t[64]; snprintf(t, sizeof t, "%.0f %.0f", c, r); f->h->result_log(f->h->ctx, t); }
void onEvent(void* p, const OnAirEvent* e) { auto* f = (F*)p; f->h->result_log(f->h->ctx, e->type); }

#ifdef FAKE_BADABI
const OnAirPluginVTable kVt = {sizeof(OnAirPluginVTable), 99, ONAIR_CAP_IQ_SINK, "fake", create, destroy, configure, process, nullptr, nullptr, nullptr};
#else
const OnAirPluginVTable kVt = {sizeof(OnAirPluginVTable), ONAIR_PLUGIN_ABI_VERSION, ONAIR_CAP_IQ_SINK | ONAIR_CAP_EVENTS, "fake", create, destroy, configure, process, onEvent, nullptr, nullptr};
#endif
}

extern "C" ONAIR_EXPORT const OnAirPluginVTable* onair_plugin_entry(uint32_t) { return &kVt; }

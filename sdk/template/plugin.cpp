// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A do-nothing IQ plugin: counts the samples it gets and shows the number as a status line.
#include "onair_plugin.h"
#include <cstdio>

namespace {
struct State { const OnAirHostApi* host; unsigned long long samples = 0; };

void* create(const OnAirHostApi* host, const char* mode_id) { (void)mode_id; return new State{host}; }
void destroy(void* p) { delete (State*)p; }
void process(void* p, const float* iq, uint32_t n, const OnAirIqMeta* meta) {
    (void)iq; (void)meta;
    auto* s = (State*)p;
    s->samples += n;
    char text[64];
    snprintf(text, sizeof text, "%llu samples", s->samples);
    s->host->status(s->host->ctx, "samples", text, ONAIR_LAMP_OK);
}
const OnAirPluginVTable kVt = {sizeof(OnAirPluginVTable), ONAIR_PLUGIN_ABI_VERSION, ONAIR_CAP_IQ_SINK, "my-plugin", create, destroy, nullptr, process, nullptr, nullptr, nullptr};
}  // namespace

extern "C" ONAIR_EXPORT const OnAirPluginVTable* onair_plugin_entry(uint32_t host_abi) { return host_abi >= ONAIR_PLUGIN_ABI_VERSION ? &kVt : nullptr; }

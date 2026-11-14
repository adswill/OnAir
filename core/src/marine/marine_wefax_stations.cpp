// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF radiofax directory and schedule (see marine_wefax_stations.h). Edit the two tables below to update.
// Frequencies are the assigned carriers in kHz (tune the USB dial 1.9 kHz lower). Broadcast times are UTC; most are marked
// approximate: they follow the published schedules only roughly, and stations change them with the seasons.
#include "dect2/marine/marine_wefax_stations.h"
#include <algorithm>

namespace dect2 {

const std::vector<WefaxStation>& wefaxStations() {
    static const std::vector<WefaxStation> t = {
        // Europe
        {"DDK", "DWD Pinneberg (Hamburg)", "DDH3 DDK3 DDK6", "Germany", "Europe", {3855, 7880, 13882.5}},
        {"GYA", "Northwood (UK Met Office / Royal Navy)", "GYA", "United Kingdom", "Europe", {2618.5, 4610, 8040, 11086.5}},
        {"SVJ4", "Athens", "SVJ4", "Greece", "Europe", {4481, 8105}},
        {"TAH", "Istanbul (Turkish Met Service)", "TAH", "Turkey", "Europe", {3377, 6825}},
        {"RBW", "Murmansk", "RBW41", "Russia", "Europe", {6445.5, 7907}},
        // North America
        {"NMF", "Boston (NOAA / USCG)", "NMF", "United States", "North America", {4235, 6340.5, 9110, 12750}},
        {"NMG", "New Orleans (NOAA / USCG)", "NMG", "United States", "North America", {4317.9, 8503.9, 12789.9, 17146.4}},
        {"NMC", "Point Reyes (NOAA / USCG)", "NMC", "United States", "North America", {4346, 8682, 12786, 17151.2, 22527}},
        {"NOJ", "Kodiak (NOAA / USCG)", "NOJ", "United States", "North America", {2054, 4298, 8459, 12412.5}},
        {"KVM70", "Honolulu (NOAA)", "KVM70", "United States", "Oceania", {9982.5, 11090, 16135, 23331.5}},
        {"CFH", "Halifax (Canadian Forces)", "CFH", "Canada", "North America", {4271, 6496.4, 10536, 13510}},
        {"VCO", "Sydney, Nova Scotia (CCG)", "VCO", "Canada", "North America", {4416, 6915.1}},
        {"VFF", "Iqaluit (CCG, summer)", "VFF", "Canada", "North America", {3253, 7710}},
        {"VFA", "Inuvik (CCG, summer)", "VFA", "Canada", "North America", {4292, 8457.8}},
        // Asia
        {"JMH", "Tokyo (Japan Meteorological Agency)", "JMH", "Japan", "Asia", {3622.5, 7795, 13988.5}},
        {"JJC", "Kyodo News, Tokyo", "JJC", "Japan", "Asia", {4316, 8467.5, 12745.5, 16971, 17069.6, 22542}},
        {"JFX", "Kagoshima (Japan Coast Guard)", "JFX", "Japan", "Asia", {4274, 8658, 13074, 16907.5, 22559.6}},
        {"HLL2", "Seoul (KMA)", "HLL2", "South Korea", "Asia", {3585, 5857.5, 7433.5, 9165, 13570}},
        {"BAF", "Beijing", "BAF", "China", "Asia", {5526.9, 8121.9, 10116.9, 14366.9, 16025.9, 18236.9}},
        {"XSG", "Shanghai", "XSG", "China", "Asia", {4170, 8302, 12382, 16559}},
        {"XSQ", "Guangzhou", "XSQ", "China", "Asia", {4199.8, 8412.5, 12629.3, 16826.3}},
        {"BMF", "Taipei", "BMF", "Taiwan", "Asia", {4616, 8140, 13900, 18560}},
        {"HSW64", "Bangkok (Thai Navy)", "HSW64", "Thailand", "Asia", {7395}},
        // Oceania
        {"VMC", "Charleville (Bureau of Meteorology)", "VMC", "Australia", "Oceania", {2628, 5100, 11030, 13920, 20469}},
        {"VMW", "Wiluna (Bureau of Meteorology)", "VMW", "Australia", "Oceania", {5755, 7535, 10555, 15615, 18060}},
        {"ZKLF", "Wellington (MetService)", "ZKLF", "New Zealand", "Oceania", {3247.4, 5807, 9459, 13550.5, 16340.1}},
        // South America
        {"CBV", "Valparaiso (Chilean Navy)", "CBV", "Chile", "South America", {4228, 8677, 17146.4}},
        {"CBM", "Punta Arenas, Magallanes (Chilean Navy)", "CBM", "Chile", "South America", {4322, 8696}},
        // Africa
        {"ZSJ", "Cape Town (South African Navy)", "ZSJ", "South Africa", "Africa", {4014, 7508, 13538, 18238}},
    };
    return t;
}

// UTC start (hhmm), station, product. Charts every 6 hours are the usual pattern; the exact minutes vary.
const std::vector<WefaxBroadcast>& wefaxBroadcasts() {
    auto m = [](int hhmm) { return hhmm / 100 * 60 + hhmm % 100; };
    static const std::vector<WefaxBroadcast> t = {
        {"DDK", m(430), 120, 576, "Surface analysis, North Atlantic and Europe", true},
        {"DDK", m(1030), 120, 576, "Surface analysis", true},
        {"DDK", m(1630), 120, 576, "Surface analysis", true},
        {"DDK", m(2230), 120, 576, "Surface analysis", true},
        {"DDK", m(900), 120, 576, "Sea state / wave forecast", true},
        {"GYA", m(300), 120, 576, "Surface analysis", true},
        {"GYA", m(900), 120, 576, "Surface analysis", true},
        {"GYA", m(1500), 120, 576, "Surface analysis", true},
        {"GYA", m(2100), 120, 576, "Surface analysis", true},
        {"GYA", m(1000), 120, 576, "Sea and swell forecast", true},
        {"SVJ4", m(900), 120, 576, "Surface analysis, Mediterranean", true},
        {"SVJ4", m(1600), 120, 576, "Surface forecast", true},
        {"NMF", m(230), 120, 576, "Surface analysis, Atlantic", true},
        {"NMF", m(815), 120, 576, "Surface analysis", true},
        {"NMF", m(1400), 120, 576, "Surface analysis", true},
        {"NMF", m(1945), 120, 576, "Surface analysis", true},
        {"NMG", m(0), 120, 576, "Tropical surface analysis", true},
        {"NMG", m(600), 120, 576, "Tropical surface analysis", true},
        {"NMG", m(1200), 120, 576, "Tropical surface analysis", true},
        {"NMG", m(1800), 120, 576, "Tropical surface analysis", true},
        {"NMC", m(140), 120, 576, "Surface analysis, Pacific", true},
        {"NMC", m(740), 120, 576, "Surface analysis", true},
        {"NMC", m(1340), 120, 576, "Surface analysis", true},
        {"NMC", m(1940), 120, 576, "Surface analysis", true},
        {"NOJ", m(400), 120, 576, "Surface analysis, Alaska", true},
        {"NOJ", m(1600), 120, 576, "Surface analysis", true},
        {"KVM70", m(130), 120, 576, "Surface analysis, Pacific", true},
        {"KVM70", m(1330), 120, 576, "Surface analysis", true},
        {"CFH", m(300), 120, 576, "Surface analysis", true},
        {"CFH", m(1500), 120, 576, "Surface analysis", true},
        {"JMH", m(300), 120, 576, "Asia surface analysis", true},
        {"JMH", m(900), 120, 576, "Asia surface analysis", true},
        {"JMH", m(1500), 120, 576, "Asia surface analysis", true},
        {"JMH", m(2100), 120, 576, "Asia surface analysis", true},
        {"JJC", m(0), 60, 576, "Weather charts and news", true},
        {"HLL2", m(330), 120, 576, "Surface analysis", true},
        {"HLL2", m(1530), 120, 576, "Surface analysis", true},
        {"BMF", m(200), 120, 576, "Surface analysis", true},
        {"VMC", m(130), 120, 576, "MSL analysis, Australia", true},
        {"VMC", m(730), 120, 576, "MSL analysis", true},
        {"VMC", m(1330), 120, 576, "MSL analysis", true},
        {"VMC", m(1930), 120, 576, "MSL analysis", true},
        {"VMW", m(200), 120, 576, "MSL analysis, Indian Ocean", true},
        {"VMW", m(1400), 120, 576, "MSL analysis", true},
        {"ZKLF", m(100), 120, 576, "MSL analysis, Southwest Pacific", true},
        {"ZKLF", m(1300), 120, 576, "MSL analysis", true},
        {"CBV", m(1100), 120, 576, "Surface analysis, Southeast Pacific", true},
        {"CBV", m(2300), 120, 576, "Surface analysis", true},
        {"ZSJ", m(600), 120, 576, "Surface analysis, South Atlantic", true},
        {"ZSJ", m(1800), 120, 576, "Surface analysis", true},
    };
    return t;
}

const WefaxStation* wefaxStation(const std::string& id) {
    for (const auto& s : wefaxStations()) if (id == s.id) return &s;
    return nullptr;
}

std::vector<WefaxNext> wefaxNext(long long now, int count, const std::string& region) {
    std::vector<WefaxNext> out;
    const long long day0 = now - ((now % 86400) + 86400) % 86400;
    for (const auto& b : wefaxBroadcasts()) {
        const WefaxStation* s = wefaxStation(b.station);
        if (!s || (!region.empty() && region != s->region)) continue;
        long long t = day0 + b.utcMin * 60LL;
        if (t + 600 < now) t += 86400;          // a broadcast that began up to 10 minutes ago is still on
        out.push_back({&b, s, t});
    }
    std::sort(out.begin(), out.end(), [](const WefaxNext& x, const WefaxNext& y) { return x.startUtc < y.startUtc; });
    if ((int)out.size() > count) out.resize((size_t)count);
    return out;
}

} // namespace dect2

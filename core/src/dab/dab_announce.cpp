// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dab/dab_announce.h"
#include <algorithm>

namespace dect2 {

const char* dabAnnouncementTypeName(int t) {
    static const char* const n[16] = {"Alarm", "Road traffic flash", "Transport flash", "Warning/Service", "News flash", "Area weather flash", "Event announcement",
        "Special event", "Programme information", "Sport report", "Financial report", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved"};
    return t >= 0 && t < 16 ? n[t] : "?";
}

bool dabActiveAnnouncement(const DabEnsemble& e, int listenSub, unsigned enabled, DabAnnouncement& out) {
    if (listenSub < 0 || !enabled) return false;
    // the services that have the listened sub-channel as a component
    std::vector<uint32_t> sids;
    for (const auto& kv : e.services) for (const DabComponent& c : kv.second.comps) if (c.subId == listenSub) { sids.push_back(kv.first); break; }
    bool found = false;
    for (const DabAnnSwitch& sw : e.annActive) {
        if (sw.subId < 0 || sw.asw == 0) continue;
        const bool alarmCluster = sw.cluster == 0xFF || sw.cluster == 0xFE;
        unsigned ok = 0;
        if (alarmCluster) ok = sw.asw & 1u;   // an alarm goes to everybody
        else {
            for (uint32_t sid : sids) {
                const auto it = e.annSupport.find(sid);
                if (it == e.annSupport.end()) continue;
                const auto& cl = it->second.clusters;
                if (std::find(cl.begin(), cl.end(), sw.cluster) != cl.end()) ok |= sw.asw & it->second.asu;
            }
        }
        ok &= enabled;
        if (!ok || !e.subs.count(sw.subId) || e.subs.at(sw.subId).size <= 0) continue;
        bool audio = false;   // only a sub-channel that carries an audio service can be played
        for (const auto& kv : e.services) for (const DabComponent& c : kv.second.comps) if (c.tmid == 0 && c.subId == sw.subId) audio = true;
        if (!audio) continue;
        int type = 0;
        while (!(ok & (1u << type))) type++;
        if (!found || type < out.type) { out.cluster = sw.cluster; out.type = type; out.flags = sw.asw; out.subId = sw.subId; found = true; }
    }
    return found && out.subId != listenSub;
}

} // namespace dect2

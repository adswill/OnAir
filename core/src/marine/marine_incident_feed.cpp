// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The receivers' side of the incident board: alerts become automation events and log lines.
#include "dect2/automation/engine.h"
#include "dect2/marine/marine_incident.h"
#include <atomic>

namespace dect2 {
namespace marine {

void incidentFlush(const std::function<void(const std::string&)>& log) {
    IncidentBoard& b = IncidentBoard::global();
    const double now = automation::wallNow();
    static std::atomic<double> lastTick{0};
    double lt = lastTick.load();
    if (now - lt >= 10 && lastTick.compare_exchange_strong(lt, now)) b.tick(now);
    for (const IncidentAlert& a : b.takeAlerts()) {
        if (log) log("Incident " + std::to_string(a.id) + ": " + a.text);
        if (!automation::wants(automation::FDsc)) continue;
        automation::Event ev("dsc.incident");
        ev.at(now, "Marine", 156.525e6);
        ev.num("incident", a.id).str("what", a.what).str("text", a.text).str("kind", a.kind).str("mmsi", a.mmsi);
        if (a.hasPos) ev.num("lat", a.lat).num("lon", a.lon);
        if (a.distNm >= 0) ev.num("distance_nm", a.distNm);
        automation::emit(std::move(ev));
    }
}

} // namespace marine
} // namespace dect2

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Position prediction on the aircraft maps (ADS-B, VDL2, HFDL, ACARS): one TrackHub per screen, the "Predict positions" and "uncertainty"
// switches, the ghost marker with its dashed ring and the 95 % ellipse, and the tooltip text. The estimator is core/src/track; see
// docs/position-estimation.md. Header only; every function is inline so a screen needs no other file.
#pragma once
#include "dect2/adsb/adsb_tel.h"
#include "dect2/track/track_hub.h"
#include "i18n/i18n.h"
#include "imgui.h"
#include "platform/platform.h"
#include "../adsb/adsb_map.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

extern float gUi;

namespace pospred {

struct Layer {
    dect2::track::TrackHub hub;
    bool predict = true, ellipses = false, loaded = false;
    std::string keyPredict, keyEllipse;
    ~Layer() { dect2::track::unregisterHub(&hub); }
    std::unordered_map<uint32_t, std::pair<double, double>> lastFed_;
    double lastPurge_ = 0;
    // defaults: the satellite and VDL2 maps predict (their aircraft report every few minutes), ADS-B interpolates but draws no ellipses
    void load(const char* prefix, bool predictDefault, bool ellipseDefault) {
        if (loaded) return;
        loaded = true;
        dect2::track::registerHub(&hub);   // Search everything asks the map screens for live positions
        keyPredict = std::string(prefix) + "Predict"; keyEllipse = std::string(prefix) + "PredictEllipse";
        predict = plat::prefs().getB(keyPredict.c_str(), predictDefault);
        ellipses = plat::prefs().getB(keyEllipse.c_str(), ellipseDefault);
    }
    // ADS-B: hand every new position (with the speed, altitude and so on that came with it) to the tracker; call once per frame with ImGui::GetTime()
    void feedAdsb(const std::vector<dect2::AdsbAircraft>& v, double now) {
        for (const auto& ac : v) {
            if (!ac.hasPos || ac.posAgeSec > 30) continue;
            auto& lf = lastFed_[ac.icao];
            if (lf.first == ac.lat && lf.second == ac.lon) continue;
            lf = {ac.lat, ac.lon};
            dect2::track::Fix f;
            if (dect2::track::fixFromAdsb(ac, now, f)) hub.feed(dect2::track::keyFromIcao(ac.icao), f);
        }
        if (now - lastPurge_ > 5) { lastPurge_ = now; hub.purge(now, 600); for (auto it = lastFed_.begin(); it != lastFed_.end();) it = hub.find(dect2::track::keyFromIcao(it->first)) ? std::next(it) : lastFed_.erase(it); }
    }
    // the estimate to draw instead of the raw position (false: switched off, or no track yet). Cheap mean-only form while the fix is fresh and no ellipse is drawn.
    bool get(dect2::track::Key k, double now, dect2::track::PosEstimate& pe, double posAgeSec) {
        if (!predict) return false;
        return hub.estimate(k, now, pe, !ellipses && posAgeSec < 10);
    }
    // the two switches, on the screen's own button line
    void controls() {
        if (ImGui::Checkbox(tr("Predict positions"), &predict)) { plat::prefs().setB(keyPredict.c_str(), predict); plat::prefs().flush(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Fills the gaps between position reports: the aircraft moves smoothly and, when messages stop, is dead-reckoned along its track (and along the route when a message named the destination). Predicted positions are drawn as a ghost with a dashed ring."));
        if (predict) {
            ImGui::SameLine(0, 6 * gUi);
            if (ImGui::Checkbox(tr("uncertainty"), &ellipses)) { plat::prefs().setB(keyEllipse.c_str(), ellipses); plat::prefs().flush(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Draws the uncertainty ellipse (95 percent) of every estimated position."));
        }
    }
};

inline std::string agoText(double sec) {
    char b[32];
    if (sec < 90) snprintf(b, sizeof b, "%.0f s", sec); else if (sec < 5400) snprintf(b, sizeof b, "%.0f min", sec / 60); else snprintf(b, sizeof b, "%.1f h", sec / 3600);
    return b;
}

// "predicted from the last position 3 min ago, +-4 NM"
inline std::string tooltipText(const dect2::track::PosEstimate& pe) {
    char b[160];
    const double nm = pe.ellipse.majorM / 1852.0;
    if (pe.coarse) snprintf(b, sizeof b, tr("estimated from the ground station only, within about %.0f NM"), nm);
    else snprintf(b, sizeof b, tr("predicted from the last position %s ago, +-%.1f NM"), agoText(pe.ageSec).c_str(), nm);
    return b;
}

// the 95 % ellipse (or the circle of a coarse estimate) around the estimate
inline void drawEllipse(ImDrawList* dl, const dect2::track::PosEstimate& pe, ImU32 col, bool dashed) {
    using namespace dect2::track;
    const int n = 48;
    ImVec2 pts[n];
    const double th = pe.ellipse.orientDeg * dect2::track::kDeg;
    for (int i = 0; i < n; i++) {
        const double a = 2 * dect2::track::kPi * i / n, x = pe.ellipse.majorM * std::cos(a), y = pe.ellipse.minorM * std::sin(a);   // along the major axis, across it
        const double dE = x * std::sin(th) + y * std::cos(th), dN = x * std::cos(th) - y * std::sin(th);
        double la, lo;
        gcDestination(pe.lat, pe.lon, wrap360(std::atan2(dE, dN) / kDeg), std::hypot(dE, dN), la, lo);
        pts[i] = adsbmap::project(la, lo);
    }
    if (!dashed) { dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, 1.2f); return; }
    for (int i = 0; i < n; i += 2) dl->AddLine(pts[i], pts[(i + 1) % n], col, 1.2f);
}

// A predicted aircraft is drawn as a ghost: the caller draws the icon with the returned colour; this adds the dashed ring around it
inline ImU32 ghostColour(ImU32 c) { return (c & 0x00FFFFFF) | 0x90000000; }

inline void drawPredictionMarks(ImDrawList* dl, const dect2::track::PosEstimate& pe, ImVec2 q, ImU32 col, bool ellipses) {
    if (pe.predicted) {
        const float r = 9.f * gUi;
        for (int i = 0; i < 12; i += 2) {
            const float a0 = (float)(i * 2 * M_PI / 12), a1 = (float)((i + 1) * 2 * M_PI / 12);
            dl->AddLine(ImVec2(q.x + r * std::cos(a0), q.y + r * std::sin(a0)), ImVec2(q.x + r * std::cos(a1), q.y + r * std::sin(a1)), ghostColour(col), 1.3f);
        }
    }
    if (ellipses && pe.valid) drawEllipse(dl, pe, ghostColour(col), pe.predicted);
}

// an aircraft with no position at all that talks to a ground station of known position: the station's reach as a dashed ring (a wide region, not a point)
inline void drawCoarse(ImDrawList* dl, const dect2::track::PosEstimate& pe, ImU32 col) {
    drawEllipse(dl, pe, ghostColour(col), true);
    const ImVec2 c = adsbmap::project(pe.lat, pe.lon);
    const ImVec2 m = ImGui::GetIO().MousePos;
    dl->AddCircleFilled(c, 2.5f * gUi, ghostColour(col));
    (void)m;
}

inline void hoverTooltip(const dect2::track::PosEstimate& pe, ImVec2 q) {
    if (!pe.predicted) return;
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (std::hypot(m.x - q.x, m.y - q.y) < 12 * gUi) ImGui::SetTooltip("%s", tooltipText(pe).c_str());
}

}  // namespace pospred

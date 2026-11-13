// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "map/my_location.h"
#include "adsb/adsb_map.h"
#include "app.h"
#include "dect2/geo/mylocation.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "ui/scale.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mylocmap {

namespace {

bool metricUnits() { return std::strcmp(adsbDistUnit(), "km") == 0; }

std::vector<double> ringKm() {
    const std::vector<double> v = dect2::myloc::parseRingList(plat::prefs().getS("mapMyLocRingList", "50,100,200"));
    std::vector<double> km;
    const bool metric = metricUnits();
    for (double x : v) km.push_back(dect2::myloc::toKm(x, metric));
    return km;
}

void drawRing(ImDrawList* dl, double lat, double lon, double km, ImVec2 origin, ImVec2 size, ImU32 col, const char* label, ImU32 text) {
    const ImVec2 c = adsbmap::project(lat, lon);
    double la, lo;
    dect2::myloc::destination(lat, lon, 0.0, km, la, lo);
    const float rPx = std::fabs(adsbmap::project(la, lo).y - c.y);
    const auto pts = dect2::myloc::ringPoints(lat, lon, km, dect2::myloc::ringSegments(rPx));
    if (pts.size() < 4) return;
    const float jump = std::max(size.x, size.y) * 0.8f;
    std::vector<ImVec2> sp;
    sp.reserve(pts.size());
    for (const auto& p : pts) sp.push_back(adsbmap::project(p.first, p.second));
    for (size_t i = 1; i < sp.size(); i++) {
        const float dx = sp[i].x - sp[i - 1].x, dy = sp[i].y - sp[i - 1].y;
        if (!std::isfinite(dx) || !std::isfinite(dy) || std::fabs(dx) > jump || std::fabs(dy) > jump) continue;
        dl->AddLine(sp[i - 1], sp[i], col, 1.2f);
    }
    if (label && std::isfinite(sp[0].x) && std::isfinite(sp[0].y) && sp[0].x > origin.x && sp[0].x < origin.x + size.x && sp[0].y > origin.y && sp[0].y < origin.y + size.y)
        dl->AddText(ImVec2(sp[0].x + 3, sp[0].y - 1), text, label);
}

}

bool markerOn() { return plat::prefs().getB("mapMyLoc", true); }
bool ringsOn() { return plat::prefs().getB("mapMyLocRings", false); }

bool describeFromMe(double lat, double lon, std::string& out) {
    if (!receiverLocationSet()) return false;
    double ml, mo;
    receiverLocation(ml, mo);
    out = dect2::myloc::describe(ml, mo, lat, lon, metricUnits());
    return !out.empty();
}

std::string distanceBearingText(double lat, double lon) {
    std::string s;
    return describeFromMe(lat, lon, s) ? s : std::string();
}

void infoLine(double lat, double lon) {
    std::string s;
    if (describeFromMe(lat, lon, s)) ImGui::TextDisabled("%s %s", tr("From me:"), s.c_str());
}

void overlay(ImDrawList* dl, ImVec2 origin, ImVec2 size, bool hovered) {
    if (!markerOn()) return;
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    const ImU32 text = IM_COL32((adsbmap::overlayText() >> 16) & 255, (adsbmap::overlayText() >> 8) & 255, adsbmap::overlayText() & 255, 230);
    if (!receiverLocationSet()) {
        const char* msg = tr("My location not set - click to set it");
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        const ImVec2 a(origin.x + 8 * gUi, origin.y + 6 * gUi), b(a.x + ts.x + 8 * gUi, a.y + ts.y + 4 * gUi);
        const unsigned pc = adsbmap::panelColour();
        dl->AddRectFilled(a, b, IM_COL32((pc >> 16) & 255, (pc >> 8) & 255, pc & 255, 200), 3.f);
        dl->AddText(ImVec2(a.x + 4 * gUi, a.y + 2 * gUi), text, msg);
        if (hovered && ImGui::IsMouseHoveringRect(a, b)) {
            ImGui::SetTooltip("%s", tr("Settings > Location: the position shown on every map, with the distance to what you receive."));
            if (ImGui::IsMouseClicked(0)) settingsOpenPage("Location");
        }
        return;
    }
    double lat, lon;
    receiverLocation(lat, lon);
    dl->PushClipRect(origin, end, true);
    const ImVec2 p = adsbmap::project(lat, lon);
    const ImU32 ring = IM_COL32(255, 196, 40, 200), edge = IM_COL32(20, 20, 20, 230);
    if (ringsOn()) {
        const bool metric = metricUnits();
        const std::vector<double> km = ringKm();
        for (double k : km) {
            char l[40];
            std::snprintf(l, sizeof l, "%s", dect2::myloc::formatDistance(k, metric).c_str());
            drawRing(dl, lat, lon, k, origin, size, IM_COL32(255, 196, 40, 150), l, text);
        }
    }
    const float r = 6.f * gUi;
    dl->AddCircleFilled(p, r + 1.5f, edge);
    dl->AddCircle(p, r, ring, 0, 2.f);
    dl->AddCircleFilled(p, 2.f * gUi, ring);
    dl->AddLine(ImVec2(p.x - r - 4 * gUi, p.y), ImVec2(p.x - r * 0.4f, p.y), ring, 2.f);
    dl->AddLine(ImVec2(p.x + r * 0.4f, p.y), ImVec2(p.x + r + 4 * gUi, p.y), ring, 2.f);
    dl->AddLine(ImVec2(p.x, p.y - r - 4 * gUi), ImVec2(p.x, p.y - r * 0.4f), ring, 2.f);
    dl->AddLine(ImVec2(p.x, p.y + r * 0.4f), ImVec2(p.x, p.y + r + 4 * gUi), ring, 2.f);
    dl->PopClipRect();
    if (hovered) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        if (m.x >= origin.x && m.x <= end.x && m.y >= origin.y && m.y <= end.y) {
            double ml, mo;
            adsbmap::unproject(m, ml, mo);
            const std::string s = dect2::myloc::describe(lat, lon, ml, mo, metricUnits());
            if (!s.empty()) {
                const std::string t = std::string(tr("Pointer:")) + " " + s;
                const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
                const ImVec2 a(end.x - ts.x - 14 * gUi, origin.y + 6 * gUi), b(end.x - 6 * gUi, a.y + ts.y + 4 * gUi);
                const unsigned pc = adsbmap::panelColour();
                dl->AddRectFilled(a, b, IM_COL32((pc >> 16) & 255, (pc >> 8) & 255, pc & 255, 200), 3.f);
                dl->AddText(ImVec2(a.x + 4 * gUi, a.y + 2 * gUi), text, t.c_str());
            }
        }
    }
}

}

// DMR Map and Table sub-tabs: the LRRP positions of the radios (see core/src/dmr/dmr_lrrp.cpp)
// drawn on the shared slippy map, with Src / TG / age filters, per-radio movement tracks, a table
// of the received positions and a CSV export.
#include "dmr_map.h"
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/common/u8file.h"
#include "dect2/geo/maplayers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr double kStaleSec = 300.0;   // a radio not heard for this long is drawn grey

struct State {
    bool loaded = false;
    adsbmap::View map;
    char srcBuf[24] = "";   // filter: source radio id (decimal; empty = all)
    char tgBuf[24] = "";    // filter: talkgroup
    int ageMin = 0;         // 0 = all, else only the last N minutes
    bool metric = true;     // km/h, or mph for the imperial units
    uint32_t selSrc = 0;    // the radio selected on the map (0 = none)
    bool selValid = false;
    bool follow = false;    // keep the selected radio centred on its new fixes
    uint32_t followSrc = 0; // the radio the follow last centred on
    double followFix = -1;  // unixSec of the fix last centred on
    double followAt = 0;    // last recentre time, rate-limited to 1 Hz
    bool sideCollapsed = false;   // the right radios panel collapsed to a strip
    int sortMode = 0;       // the radios list: 0 by Src (unknown last), 1 the freshest first
    std::string msg;        // CSV export feedback
};
State S;

const char* rumb(double deg) {
    static const char* k[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    if (deg < 0) return "-";
    return k[(int)std::lround(deg / 45.0) & 7];
}
double speedShown(double kmh) { return S.metric ? kmh : kmh * 0.621371; }
const char* speedUnit() { return S.metric ? "km/h" : "mph"; }

bool live(const App& a) { return a.rx.standard == 10; }

void load() {
    if (S.loaded) return;
    S.loaded = true;
    receiverLocation(S.map.lat, S.map.lon);
    S.map.zoom = 8;
    S.sideCollapsed = plat::prefs().getB("dmrMapPanel", false);
}

uint32_t parseId(const char* s) {
    char* e = nullptr;
    const unsigned long v = std::strtoul(s, &e, 10);
    return (e && *e == '\0') ? (uint32_t)v : 0;
}

bool pass(const DmrLocation& loc, double now) {
    if (S.srcBuf[0]) { const uint32_t f = parseId(S.srcBuf); if (f && loc.src != f) return false; }
    if (S.tgBuf[0]) { const uint32_t f = parseId(S.tgBuf); if (f && loc.dst != f) return false; }
    if (S.ageMin > 0 && now - loc.unixSec > S.ageMin * 60.0) return false;
    return true;
}

// latest fix of each radio that passes the filters, with its movement track
std::vector<dect2::MapMarker> dmrMarkers(const DmrTelemetry& t, double now) {
    std::vector<dect2::MapMarker> v;
    for (size_t i = t.locations.size(); i-- > 0;) {
        const DmrLocation& loc = t.locations[i];
        if (!loc.hasFix || !pass(loc, now)) continue;
        const std::string id = std::to_string(loc.src);
        bool have = false;
        for (const auto& m : v) if (m.id == id) { have = true; break; }
        if (have) continue;
        dect2::MapMarker m; m.layer = dect2::LayerDmr; m.id = id; m.label = id;
        m.lat = loc.lat; m.lon = loc.lon;
        if (loc.hasSpeed) { m.hasSpeed = true; m.speedKt = loc.speedMs * 1.943844; }
        m.headingDeg = loc.hasHeading ? loc.headingDeg : -1;
        m.ageSec = std::max(0.0, now - loc.unixSec);
        char b[64]; snprintf(b, sizeof b, "TG %u, slot %d", loc.dst, loc.slot);
        m.detail = b;
        v.push_back(std::move(m));
    }
    for (auto& m : v) {
        const uint32_t src = (uint32_t)std::stoul(m.id);
        for (const DmrLocation& loc : t.locations)
            if (loc.src == src && loc.hasFix && pass(loc, now)) m.track.push_back({(float)loc.lat, (float)loc.lon});
    }
    return v;
}

void filters() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Src");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputTextWithHint("##dmrsrc", "all", S.srcBuf, sizeof S.srcBuf);
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("TG");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputTextWithHint("##dmrtg", "all", S.tgBuf, sizeof S.tgBuf);
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("last");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(120 * gUi);
    static const char* ages[] = {"all", "1 min", "5 min", "15 min", "30 min", "1 h"};
    static const int mins[] = {0, 1, 5, 15, 30, 60};
    int cur = 0;
    for (int i = 0; i < 6; i++) if (S.ageMin == mins[i]) cur = i;
    if (ImGui::BeginCombo("##dmrage", ages[cur])) {
        for (int i = 0; i < 6; i++) if (ImGui::Selectable(ages[i], cur == i)) S.ageMin = mins[i];
        ImGui::EndCombo();
    }
}

// one marker colour per radio, stable for the whole session
constexpr ImU32 kDmrColours[] = {
    IM_COL32(0x22, 0xD3, 0xEE, 255), IM_COL32(0xF5, 0x9E, 0x0B, 255), IM_COL32(0x34, 0xD3, 0x99, 255),
    IM_COL32(0xA7, 0x8B, 0xFA, 255), IM_COL32(0xFB, 0x71, 0x85, 255), IM_COL32(0x60, 0xA5, 0xFA, 255),
    IM_COL32(0xFA, 0xCC, 0x15, 255), IM_COL32(0xE8, 0x79, 0xF9, 255), IM_COL32(0xF9, 0x73, 0x16, 255),
    IM_COL32(0x2D, 0xD4, 0xBF, 255),
};
ImU32 dmrColour(uint32_t src) { return kDmrColours[src % (sizeof(kDmrColours) / sizeof(kDmrColours[0]))]; }

// IM_COL32 packs R in the low byte: read and write the channels the same way or the
// hue flips (an orange stopped radio came out blue)
ImU32 greyMix(ImU32 c, float k) {   // k = 1 -> full grey, 0 -> unchanged
    const int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = (int)((c >> 24) & 0xFF);
    const int L = (r * 299 + g * 587 + b * 114) / 1000;
    return IM_COL32((int)(r + (L - r) * k), (int)(g + (L - g) * k), (int)(b + (L - b) * k), a ? a : 255);
}

// badge size in screen pixels: 20 px up close, shrinking quickly as the map zooms
// out, so a radio becomes a small dot over a city-scale view instead of a big circle
float badgeRadius(float zoom) {
    return std::clamp(20.0f - (15.0f - zoom) * 5.0f, 4.0f, 20.0f);
}

// the radio badge: a dark circle with a coloured ring and the id inside while there
// is room for it, a short course dash, fading with age; grey and dashed when stale.
// At far zoom it degenerates into a plain ring or a dot, never a huge circle.
void drawBadge(ImDrawList* dl, const ImVec2& q, uint32_t src, ImU32 col, double headingDeg,
               double ageSec, bool stale, bool stopped, bool sel, float zoom) {
    char id[16];
    if (src) snprintf(id, sizeof id, "%u", src); else snprintf(id, sizeof id, "?");
    const float rBase = badgeRadius(zoom);
    const float r = rBase * gUi;
    const bool showId = rBase >= 9.0f;
    const bool soft = rBase >= 12.0f;

    const float k = (float)std::clamp((ageSec - 120.0) / 480.0, 0.0, 1.0);   // fade over 2 -> 10 min
    const int a = stale ? 150 : (int)(255.0f + (110.0f - 255.0f) * k);
    ImU32 ring = stale ? greyMix(col, 0.8f) : stopped ? greyMix(col, 0.15f) : col;
    ring = (ring & 0x00FFFFFFu) | ((ImU32)a << 24);
    const ImU32 body = IM_COL32(27, 34, 42, std::min(235, a));
    const ImU32 white = (IM_COL32(255, 255, 255, 235) & 0x00FFFFFFu) | ((ImU32)a << 24);

    if (soft) {   // shadow and halo only where they read
        dl->AddCircleFilled(ImVec2(q.x, q.y + 2.5f * gUi), r, IM_COL32(0, 0, 0, 80));
        dl->AddCircleFilled(q, r + 3.0f * gUi, (col & 0x00FFFFFFu) | 0x40000000u);
    }
    dl->AddCircleFilled(q, r, body);
    dl->AddCircle(q, r, ring, 0, (showId ? 2.0f : 1.6f) * gUi);
    if (sel) dl->AddCircle(q, r + 3.0f * gUi, white, 0, 1.6f * gUi);

    if (!stale && headingDeg >= 0) {   // just the head, clear of the badge and of the selection ring
        const float h = (float)(headingDeg * M_PI / 180.0), sx = std::sin(h), sy = -std::cos(h);
        const float gap = (sel ? 5.0f : 3.5f) * gUi;
        const float w = (showId ? 4.6f : 3.4f) * gUi;      // half-width of the head
        const float len = (showId ? 11.0f : 8.5f) * gUi;
        const ImVec2 base(q.x + sx * (r + gap), q.y + sy * (r + gap));
        const ImVec2 tip(q.x + sx * (r + gap + len), q.y + sy * (r + gap + len));
        const ImVec2 a1(base.x - sy * w, base.y + sx * w);
        const ImVec2 a2(base.x + sy * w, base.y - sx * w);
        dl->AddTriangleFilled(tip, a1, a2, ring);
    }
    if (showId) {
        ImFont* font = ImGui::GetFont();
        const float base = ImGui::GetFontSize();
        const float fs = std::min(base, r * 1.05f);
        const ImVec2 t0 = ImGui::CalcTextSize(id);
        const ImVec2 ts(t0.x * fs / base, t0.y * fs / base);
        dl->AddText(font, fs, ImVec2(q.x - ts.x * 0.5f, q.y - ts.y * 0.5f), IM_COL32(230, 237, 243, a), id);
    } else if (sel) {   // no room inside: the id goes into a chip next to the dot
        const ImVec2 t0 = ImGui::CalcTextSize(id);
        const ImVec2 c0(q.x + r + 6.0f * gUi, q.y - t0.y * 0.5f - 3.0f * gUi);
        dl->AddRectFilled(ImVec2(c0.x - 4.0f * gUi, c0.y - 2.0f * gUi),
                          ImVec2(c0.x + t0.x + 4.0f * gUi, c0.y + t0.y + 2.0f * gUi), IM_COL32(34, 42, 51, 235), 4.0f * gUi);
        dl->AddRect(ImVec2(c0.x - 4.0f * gUi, c0.y - 2.0f * gUi),
                    ImVec2(c0.x + t0.x + 4.0f * gUi, c0.y + t0.y + 2.0f * gUi), IM_COL32(58, 69, 82, 235), 4.0f * gUi);
        dl->AddText(ImVec2(c0.x, c0.y), IM_COL32(230, 237, 243, 255), id);
    }
}

const DmrLocation* latestFix(const DmrTelemetry& t, uint32_t src) {
    for (size_t i = t.locations.size(); i-- > 0;)
        if (t.locations[i].src == src && t.locations[i].hasFix) return &t.locations[i];
    return nullptr;
}

const char* wallClock(double unixSec, char* b, size_t n) {
    const std::time_t tt = (std::time_t)unixSec;
    if (const std::tm* tm = std::localtime(&tt)) std::strftime(b, n, "%H:%M:%S", tm);
    else snprintf(b, n, "-");
    return b;
}

// a small tinted tag with coloured text; the cursor ends after it
void statusPill(const char* s, ImU32 rgb) {
    const ImVec2 ts = ImGui::CalcTextSize(s);
    const float px = 5.0f * gUi, py = 1.5f * gUi;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 mx(p.x + ts.x + px * 2.0f, p.y + ts.y + py * 2.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, mx, (rgb & 0x00FFFFFFu) | 0x26000000u, 4.0f * gUi);
    dl->AddText(ImVec2(p.x + px, p.y + py), rgb, s);
    ImGui::SetCursorScreenPos(ImVec2(mx.x + 6.0f * gUi, p.y));
}

void cardRow(const char* k, const char* v) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", k);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(v);
}

} // namespace

void dmrMap(App& a) {
    load();
    const DmrTelemetry& rx = a.rx.dmr;
    const bool on = live(a);
    const double now = (double)std::time(nullptr);
    // a transient empty or collapsed report must not blink the map: keep the last good snapshot
    static DmrTelemetry keep;
    static uint64_t keepSeq = 0;
    static double keepAt = 0;
    const size_t have = rx.locations.size(), kept = keep.locations.size();
    if (have > 0 && rx.seq != keepSeq) {
        if (kept == 0 || have + 2 >= kept || now - keepAt > 120.0) { keep = rx; keepSeq = rx.seq; keepAt = now; }
    }
    const bool cleared = have == 0 && rx.callLog.empty();   // the Clear button empties both
    const bool collapsed = !cleared && kept >= 3 && have + 2 < kept;
    const bool held = !cleared && !keep.locations.empty() && now - keepAt < 30.0 && (have == 0 || collapsed);
    const DmrTelemetry& t = held ? keep : rx;
    filters();
    // Esc drops the selection (and the follow); in full screen Esc leaves the full screen instead
    if (S.selValid && !ImGui::GetIO().WantTextInput && !adsbmap::fullActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        S.selValid = false; S.selSrc = 0; S.follow = false;
    }
    if (!on && t.locations.empty()) {
        ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see radios on the map");
        return;
    }
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float dw = std::min(250.f * gUi, W * 0.28f);
    const float panelW = S.sideCollapsed ? 32.f * gUi : dw;
    const float gap = 8.f * gUi;
    ImGui::BeginChild("##dmrmap", ImVec2(W - panelW - gap, H), false);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(120 * gUi, ImGui::GetContentRegionAvail().y));
    const double beforeLat = S.map.lat, beforeLon = S.map.lon;
    const int beforeZoom = S.map.zoom;
    adsbmap::draw(S.map, size);
    // a manual pan pauses the follow; a wheel zoom only changes the scale and must not
    if (S.follow && S.map.zoom == beforeZoom && (S.map.lat != beforeLat || S.map.lon != beforeLon)) S.follow = false;
    // follow: keep the selected radio centred, at most once a second
    if (S.follow && S.selValid)
        if (const DmrLocation* fl = latestFix(t, S.selSrc); fl && fl->hasFix)
            if ((S.followSrc != S.selSrc || fl->unixSec != S.followFix) && now - S.followAt >= 1.0) {
                S.map.lat = fl->lat; S.map.lon = fl->lon;
                S.followSrc = S.selSrc; S.followFix = fl->unixSec; S.followAt = now;
            }
    const std::vector<dect2::MapMarker> mk = dmrMarkers(t, now);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    struct Hit { ImVec2 p; const dect2::MapMarker* m; };
    std::vector<Hit> hits;
    hits.reserve(mk.size());
    for (const dect2::MapMarker& m : mk) {
        const uint32_t src = (uint32_t)std::stoul(m.id);
        const ImU32 col = dmrColour(src);
        const double kmh = m.hasSpeed ? m.speedKt * 1.852 : 0.0;
        const bool stale = m.ageSec > kStaleSec;
        const bool stopped = !stale && kmh < 1.0;
        const bool sel = S.selValid && src == S.selSrc;
        if (m.track.size() > 1) {
            const ImU32 trackCol = (col & 0x00FFFFFFu) | 0x99000000u;   // the movement trail
            ImVec2 prev = adsbmap::project(m.track[0].first, m.track[0].second);
            for (size_t k = 1; k < m.track.size(); k++) {
                const ImVec2 q = adsbmap::project(m.track[k].first, m.track[k].second);
                if (std::fabs(m.track[k].second - m.track[k - 1].second) < 180) dl->AddLine(prev, q, trackCol, 2.0f * gUi);
                prev = q;
            }
        }
        const ImVec2 q = adsbmap::project(m.lat, m.lon);
        drawBadge(dl, q, src, col, m.headingDeg, m.ageSec, stale, stopped, sel, S.map.zoom);
        hits.push_back({q, &m});
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("me")) { double rl, ro; receiverLocation(rl, ro); S.map.lat = rl; S.map.lon = ro; }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button(S.metric ? "km/h" : "mph", ImVec2(46 * gUi, 0))) S.metric = !S.metric;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the radio positions");
    ImVec2 cp;
    if (adsbmap::clickedAt(cp) && cp.y > p0.y + 34.0f * gUi) {   // the top strip holds the buttons
        const dect2::MapMarker* best = nullptr;
        float bd = (badgeRadius(S.map.zoom) + 6.0f) * gUi;
        for (const Hit& h : hits) {
            const float d = std::hypotf(cp.x - h.p.x, cp.y - h.p.y);
            if (d < bd) { bd = d; best = h.m; }
        }
        if (best) { S.selSrc = (uint32_t)std::stoul(best->id); S.selValid = true; }
        else S.selValid = false;
    }
    ImGui::EndChild();
    // double-click on the thin strip between the map and the panel collapses/expands it
    {
        const ImVec2 mo = ImGui::GetIO().MousePos;
        if (mo.x >= p0.x + size.x && mo.x <= p0.x + size.x + gap && mo.y >= p0.y && mo.y <= p0.y + size.y && ImGui::IsMouseDoubleClicked(0))
            S.sideCollapsed = !S.sideCollapsed;
    }
    if (S.selValid) {
        const DmrLocation* loc = latestFix(t, S.selSrc);
        if (loc && loc->hasFix) {
            const ImVec2 q = adsbmap::project(loc->lat, loc->lon);
            // the card stays inside the map; when there is no room on the chosen side it flips over
            static ImVec2 cardSize(190 * gUi, 170 * gUi);
            const float margin = 4 * gUi;
            float px = q.x + 20 * gUi, py = q.y + 12 * gUi;
            if (px + cardSize.x > p0.x + size.x - margin) px = q.x - 20 * gUi - cardSize.x;
            if (py + cardSize.y > p0.y + size.y - margin) py = q.y - 12 * gUi - cardSize.y;
            px = std::clamp(px, p0.x + margin, std::max(p0.x + margin, p0.x + size.x - margin - cardSize.x));
            py = std::clamp(py, p0.y + margin, std::max(p0.y + margin, p0.y + size.y - margin - cardSize.y));
            ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always);
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::max(1.f, size.x - 2 * margin), std::max(1.f, size.y - 2 * margin)));
            ImGui::SetNextWindowBgAlpha(0.94f);
            if (ImGui::Begin("##dmrcard", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
                const ImU32 col = dmrColour(loc->src);
                const ImVec2 hp = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(hp.x + 5.0f * gUi, hp.y + 8.0f * gUi), 4.5f * gUi, col);
                ImGui::SetCursorScreenPos(ImVec2(hp.x + 13.0f * gUi, hp.y + 2.0f * gUi));
                if (loc->src) ImGui::TextColored(ImVec4(1, 1, 1, 1), "Src %u", loc->src);
                else ImGui::TextColored(ImVec4(1, 1, 1, 1), "? unknown");
                ImGui::SameLine(0, 8 * gUi);
                const double kmh = loc->hasSpeed ? loc->speedMs * 3.6 : 0.0;
                const bool stale = now - loc->unixSec > kStaleSec;
                const char* status = stale ? "STALE" : kmh < 1.0 ? "STOPPED" : "MOVING";
                statusPill(status, stale ? IM_COL32(240, 190, 60, 255) : kmh < 1.0 ? IM_COL32(150, 160, 175, 255) : IM_COL32(80, 200, 120, 255));
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                char b[64];
                if (ImGui::BeginTable("##dmrcardg", 2, ImGuiTableFlags_SizingFixedFit)) {
                    snprintf(b, sizeof b, "%.1f %s", speedShown(kmh), speedUnit());
                    cardRow("Speed", b);
                    if (loc->hasHeading) snprintf(b, sizeof b, "%.0f deg  %s", loc->headingDeg, rumb(loc->headingDeg));
                    cardRow("Course", loc->hasHeading ? b : "-");
                    snprintf(b, sizeof b, "%.5f", loc->lat);
                    cardRow("Latitude", b);
                    snprintf(b, sizeof b, "%.5f", loc->lon);
                    cardRow("Longitude", b);
                    cardRow("Updated", wallClock(loc->unixSec, b, sizeof b));
                    cardRow("Age", dect2::ageText(std::max(0.0, now - loc->unixSec)).c_str());
                    ImGui::EndTable();
                }
                ImGui::Spacing();
                if (ImGui::SmallButton(S.follow ? "Following" : "Follow")) {
                    S.follow = !S.follow;
                    if (S.follow) { S.followSrc = 0; S.followFix = -1; S.followAt = 0; }   // centre on the very next frame
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the map centred on this radio's new fixes (a manual pan pauses it).");
                ImGui::SameLine(0, 8 * gUi);
                if (ImGui::SmallButton("copy coords")) { snprintf(b, sizeof b, "%.6f, %.6f", loc->lat, loc->lon); ImGui::SetClipboardText(b); }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextDisabled("TG %u, slot %d", loc->dst, loc->slot);
                cardSize = ImGui::GetWindowSize();
                ImGui::End();
            }
        }
    }
    ImGui::SameLine(0, gap);
    ImGui::BeginChild("##dmrside", ImVec2(panelW, H), false);
    if (S.sideCollapsed) {
        // a narrow strip: the "<<", the count as a badge, and a click anywhere to expand (no room for a label)
        const float w = panelW - 2 * ImGui::GetStyle().WindowPadding.x;
        if (ImGui::Button("<<", ImVec2(w, 0))) S.sideCollapsed = false;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Expand the radios panel");
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##dmrsidex", ImVec2(w, std::max(1.f, ImGui::GetContentRegionAvail().y)))) S.sideCollapsed = false;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("radios %zu: click to expand", mk.size());
        char cnt[16]; snprintf(cnt, sizeof cnt, "%zu", mk.size());
        ImDrawList* d = ImGui::GetWindowDrawList();
        const ImVec2 ts = ImGui::CalcTextSize(cnt);
        const float bh = ImGui::GetTextLineHeight() + 4 * gUi, bw = std::max(ts.x + 8 * gUi, w);
        const ImVec2 b0(at.x + (w - bw) * 0.5f, at.y + 4 * gUi), b1(b0.x + bw, b0.y + bh);
        d->AddRectFilled(b0, b1, IM_COL32(66, 92, 120, 225), 4 * gUi);
        d->AddText(ImVec2(b0.x + (bw - ts.x) * 0.5f, b0.y + 2 * gUi), IM_COL32(240, 244, 250, 255), cnt);
    } else {
        if (ImGui::SmallButton(">>")) S.sideCollapsed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Collapse to a strip");
        ImGui::SameLine();
        ImGui::TextDisabled("radios %zu", mk.size());
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Sort");
        ImGui::SameLine(0, 4 * gUi);
        ImGui::SetNextItemWidth(std::max(70.f * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##dmrsort", S.sortMode == 0 ? "Src" : "Newest")) {
            if (ImGui::Selectable("Src", S.sortMode == 0)) S.sortMode = 0;
            if (ImGui::Selectable("Newest", S.sortMode == 1)) S.sortMode = 1;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Src: by radio number (an unknown source last). Newest: the freshest radio first.");
        ImGui::Separator();
        // the list order: by Src (an unknown source last) by default, or the freshest first
        std::vector<const dect2::MapMarker*> rows;
        rows.reserve(mk.size());
        for (const dect2::MapMarker& m : mk) rows.push_back(&m);
        if (S.sortMode == 0)
            std::stable_sort(rows.begin(), rows.end(), [](const dect2::MapMarker* x, const dect2::MapMarker* y) {
                const uint32_t a = (uint32_t)std::stoul(x->id), b = (uint32_t)std::stoul(y->id);
                if ((a == 0) != (b == 0)) return b == 0;   // an unknown source goes last
                return a < b;
            });
        else
            std::stable_sort(rows.begin(), rows.end(), [](const dect2::MapMarker* x, const dect2::MapMarker* y) { return x->ageSec < y->ageSec; });
        for (const dect2::MapMarker* mp : rows) {
            const dect2::MapMarker& m = *mp;
            const uint32_t src = (uint32_t)std::stoul(m.id);
            const bool sel = S.selValid && src == S.selSrc;
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(rp.x + 6 * gUi, rp.y + ImGui::GetTextLineHeight() * 0.5f), 4 * gUi,
                                                       src ? dmrColour(src) : IM_COL32(150, 160, 175, 255));
            ImGui::SetCursorScreenPos(ImVec2(rp.x + 14 * gUi, rp.y));
            char b[128], cb[24] = "", head[16];
            if (src) snprintf(head, sizeof head, "Src %u", src); else snprintf(head, sizeof head, "? unknown");
            if (m.headingDeg >= 0) snprintf(cb, sizeof cb, "  %s %.0f", rumb(m.headingDeg), m.headingDeg);
            snprintf(b, sizeof b, "%s  %.0f %s%s", head, speedShown(m.hasSpeed ? m.speedKt * 1.852 : 0.0), speedUnit(), cb);
            if (ImGui::Selectable(b, sel)) { S.selSrc = src; S.selValid = true; S.map.lat = m.lat; S.map.lon = m.lon; S.map.zoom = std::max(S.map.zoom, 13); }
            ImGui::TextDisabled("%.5f, %.5f", m.lat, m.lon);
            if (!m.detail.empty()) ImGui::TextDisabled("%s", m.detail.c_str());
            ImGui::TextDisabled("last heard %s ago", dect2::ageText(m.ageSec).c_str());
            ImGui::Separator();
        }
    }
    ImGui::EndChild();
    static bool sideSaved = false, sideInit = false;
    if (!sideInit) { sideSaved = S.sideCollapsed; sideInit = true; }
    if (sideSaved != S.sideCollapsed) { plat::prefs().setB("dmrMapPanel", S.sideCollapsed); plat::prefs().flush(); sideSaved = S.sideCollapsed; }
}

void dmrTable(App& a) {
    const DmrTelemetry& t = a.rx.dmr;
    const double now = (double)std::time(nullptr);
    filters();
    ImGui::SameLine(0, 10 * gUi);
    if (ImGui::SmallButton("Export CSV")) {
        char nm[64];
        const std::time_t tnow = std::time(nullptr);
        std::strftime(nm, sizeof nm, "dmr_lrrp_%Y%m%d_%H%M%S.csv", std::localtime(&tnow));
        const std::string p = saveFileDialog(nm);
        if (!p.empty()) {
            std::string csv = "sec,src,tg,slot,lat,lon,speed_kph,heading_deg,age_s,type\n";
            for (const DmrLocation& loc : t.locations) {
                if (!pass(loc, now)) continue;
                char row[256];
                snprintf(row, sizeof row, "%.3f,%u,%u,%d,%.6f,%.6f,%.2f,%.0f,%.0f,%s\n",
                         loc.sec, loc.src, loc.dst, loc.slot, loc.lat, loc.lon,
                         loc.hasSpeed ? loc.speedMs * 3.6 : 0.0,
                         loc.hasHeading ? loc.headingDeg : -1.0,
                         std::max(0.0, now - loc.unixSec),
                         loc.hasFix ? "fix" : (loc.resultCode == 16 ? "nofix" : "status"));
                csv += row;
            }
            std::ofstream f(dect2::u8file(p), std::ios::binary);
            if (f) { f << csv; f.close(); S.msg = "saved " + p; } else S.msg = "could not write " + p;
        }
    }
    if (!S.msg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.msg.c_str()); }
    if (!ImGui::BeginTable("##dmrloc", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 48 * gUi);
    ImGui::TableSetupColumn("Src", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
    ImGui::TableSetupColumn("TG", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 34 * gUi);
    ImGui::TableSetupColumn("Lat", ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
    ImGui::TableSetupColumn("Lon", ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
    ImGui::TableSetupColumn(speedUnit(), ImGuiTableColumnFlags_WidthFixed, 54 * gUi);
    ImGui::TableSetupColumn("Heading", ImGuiTableColumnFlags_WidthFixed, 78 * gUi);
    ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
    ImGui::TableHeadersRow();
    for (size_t i = t.locations.size(); i-- > 0;) {
        const DmrLocation& loc = t.locations[i];
        if (!pass(loc, now)) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d:%02d", (int)loc.sec / 60, (int)loc.sec % 60);
        ImGui::TableNextColumn(); ImGui::Text("%u", loc.src);
        ImGui::TableNextColumn(); ImGui::Text("%u", loc.dst);
        ImGui::TableNextColumn(); if (loc.slot) ImGui::Text("%d", loc.slot); else ImGui::TextUnformatted("-");
        if (loc.hasFix) {
            ImGui::TableNextColumn(); ImGui::Text("%.5f", loc.lat);
            ImGui::TableNextColumn(); ImGui::Text("%.5f", loc.lon);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", speedShown(loc.hasSpeed ? loc.speedMs * 3.6 : 0.0));
            ImGui::TableNextColumn(); if (loc.hasHeading) ImGui::Text("%.0f", loc.headingDeg); else ImGui::TextUnformatted("-");
        } else {
            for (int c = 0; c < 4; c++) { ImGui::TableNextColumn(); ImGui::TextUnformatted("-"); }
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::ageText(std::max(0.0, now - loc.unixSec)).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(loc.hasFix ? "fix" : (loc.resultCode == 16 ? "no fix" : "status"));
    }
    ImGui::EndTable();
}

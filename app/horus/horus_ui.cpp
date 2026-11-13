// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Project Horus balloon screens: the map with every payload's track, the table of payloads heard, the detail of the selected one
// (last packet, custom fields, the sentence), the packet log, the user's payload ID and custom field tables, and the bottom row with
// altitude against time and the tone estimator's view of the dial channel.
#include "app.h"
#include "dect2/horus/horus_rx.h"
#include "dect2/geo/maidenhead.h"
#include "../adsb/adsb_map.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

using dect2::HorusPayload;
using dect2::HorusTelemetry;

struct State {
    bool loaded = false;
    int viewMode = 0;                  // 0 map, 1 table, 2 packets, 3 tables (IDs, custom fields)
    adsbmap::View map;
    bool mapFollow = true;
    std::string sel;                   // callsign of the selected payload ("" = the most recently heard)
    uint64_t lastSeq = 0;
    bool pushed = false, wasRunning = false;
    int maxCh = 6;
    bool wide = true;
    char ids[16384] = {}, fields[16384] = {};
    std::string tableErr;
};
State S;

bool live(const App& a) { return dect2::horus::matchesMode(a.family, a.rx.standard); }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.map.zoom = (int)d.getI("horusZoom", 8);
    S.maxCh = (int)d.getI("horusChannels", 6);
    S.wide = d.getB("horusWide", true);
    const std::string ids = d.getS("horusIds", dect2::horus::defaultIdList()), f = d.getS("horusFields", dect2::horus::defaultCustomFields());
    snprintf(S.ids, sizeof S.ids, "%s", ids.c_str());
    snprintf(S.fields, sizeof S.fields, "%s", f.c_str());
}

void pushSettings(App& a) {
    dect2::HorusReceiver& r = a.engine.horus();
    S.tableErr = r.setTables(S.ids, S.fields);
    r.setMaxChannels(S.maxCh);
    r.setWideSearch(S.wide);
}

ImU32 colour(size_t i, float alpha = 1.f) {
    static const float c[6][3] = {{0.45f, 0.80f, 0.95f}, {0.95f, 0.70f, 0.30f}, {0.55f, 0.88f, 0.50f}, {0.92f, 0.45f, 0.42f}, {0.80f, 0.58f, 0.92f}, {0.90f, 0.86f, 0.45f}};
    const auto& k = c[i % 6];
    return IM_COL32((int)(255 * k[0]), (int)(255 * k[1]), (int)(255 * k[2]), (int)(255 * alpha));
}

const HorusPayload* selected(const HorusTelemetry& t) {
    for (const auto& p : t.payloads) if (p.callsign == S.sel) return &p;
    return t.payloads.empty() ? nullptr : &t.payloads.front();
}

const char* kindName(int k) { return k == 2 ? "Binary v2" : k == 1 ? "Binary v1" : "RTTY"; }

std::string heardText(double s) {
    char b[24];
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

void tick(App& a) {
    loadState();
    const bool run = a.engine.running();
    if (run && (!S.pushed || !S.wasRunning)) { pushSettings(a); S.pushed = true; }
    S.wasRunning = run;
    if (live(a) && a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        const HorusPayload* p = selected(a.rx.horus);
        if (p && !p->track.empty() && S.mapFollow) { S.map.lat = p->track.back().lat; S.map.lon = p->track.back().lon; }
    }
    if (!run) S.lastSeq = 0;
}

void mapView(App& a, ImVec2 size) {
    const HorusTelemetry& t = a.rx.horus;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const double lat0 = S.map.lat, lon0 = S.map.lon; const int zoom0 = S.map.zoom;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0 || S.map.zoom != zoom0) S.mapFollow = false;   // panned or zoomed (the wheel at the centre moves nothing)
    ImVec2 click;
    const bool clicked = adsbmap::clickedAt(click);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    const HorusPayload* sel = on ? selected(t) : nullptr;
    const HorusPayload* hit = nullptr;
    float hitD = 18.f * gUi;
    if (on) for (size_t i = 0; i < t.payloads.size(); i++) {
        const HorusPayload& p = t.payloads[i];
        if (p.track.empty()) continue;
        const bool isSel = &p == sel;
        const ImU32 col = colour(i, isSel ? 1.f : 0.7f);
        ImVec2 prev = adsbmap::project(p.track[0].lat, p.track[0].lon);
        for (size_t k = 1; k < p.track.size(); k++) {
            const ImVec2 q = adsbmap::project(p.track[k].lat, p.track[k].lon);
            dl->AddLine(prev, q, col, isSel ? 2.4f * gUi : 1.4f * gUi);
            prev = q;
        }
        const auto& last = p.track.back();
        const ImVec2 q = adsbmap::project(last.lat, last.lon);
        if (p.hasAscent && p.ascentMs < -1.0) {   // falling after the burst: a diamond
            const float r = 6 * gUi;
            dl->AddQuadFilled(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y), col);
        } else {
            dl->AddCircleFilled(ImVec2(q.x, q.y - 4 * gUi), 5 * gUi, col, 20);
            dl->AddLine(ImVec2(q.x, q.y + gUi), ImVec2(q.x, q.y + 6 * gUi), col, 1.2f * gUi);
        }
        if (isSel) dl->AddCircle(q, 10 * gUi, IM_COL32(255, 255, 255, 230), 28, 1.6f);
        char b[64]; snprintf(b, sizeof b, "%s  %.1f km", p.callsign.c_str(), last.altM / 1000.0);
        if (adsbmap::labelsAt(S.map.zoom, isSel)) dl->AddText(ImVec2(q.x + 9 * gUi, q.y - 8 * gUi), IM_COL32(235, 238, 240, p.active ? 255 : 150), b);
        const float d = std::hypot(q.x - click.x, q.y - click.y);
        if (clicked && d < hitD) { hitD = d; hit = &p; }
    }
    dl->PopClipRect();
    if (hit) { S.sel = hit->callsign; S.mapFollow = true; }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+##hz", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-##hz", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button(tr("follow"))) { S.mapFollow = true; if (sel && !sel->track.empty()) { S.map.lat = sel->track.back().lat; S.map.lon = sel->track.back().lon; } }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("horusZoom", S.map.zoom);
    if (!on || !sel || sel->track.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
        adsbmap::legend(size.x, "%s", !on ? tr("start the receiver") : t.payloads.empty() ? tr("no payload heard yet") : tr("no position yet"));
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));
}

void payloadTable(App& a, bool compact) {
    const HorusTelemetry& t = a.rx.horus;
    const bool on = live(a);
    const int cols = compact ? 4 : 13;
    if (!ImGui::BeginTable(compact ? "##hr_s" : "##hr_f", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Payload"));
    if (!compact) ImGui::TableSetupColumn(tr("Type"));
    ImGui::TableSetupColumn(tr("Alt m"));
    if (!compact) { ImGui::TableSetupColumn(tr("Latitude")); ImGui::TableSetupColumn(tr("Longitude")); ImGui::TableSetupColumn(tr("Climb m/s")); ImGui::TableSetupColumn(tr("Temp C"));
                    ImGui::TableSetupColumn(tr("Batt V")); ImGui::TableSetupColumn(tr("Sats")); ImGui::TableSetupColumn(tr("SNR dB")); ImGui::TableSetupColumn(tr("Packets")); }
    if (!compact) ImGui::TableSetupColumn(tr("kHz"));
    else ImGui::TableSetupColumn(tr("SNR dB"));
    ImGui::TableSetupColumn(tr("Heard"));
    ImGui::TableHeadersRow();
    if (on) {
        const HorusPayload* sel = selected(t);
        for (size_t i = 0; i < t.payloads.size(); i++) {
            const HorusPayload& p = t.payloads[i];
            const auto& k = p.last;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(colour(i)));
            if (ImGui::Selectable((p.callsign + "##hp" + std::to_string(i)).c_str(), &p == sel, ImGuiSelectableFlags_SpanAllColumns)) { S.sel = p.callsign; S.mapFollow = true; }
            ImGui::PopStyleColor();
            if (!compact) { ImGui::TableNextColumn(); ImGui::TextUnformatted(kindName(k.kind)); }
            ImGui::TableNextColumn(); ImGui::Text("%.0f", k.altM);
            if (!compact) {
                ImGui::TableNextColumn(); ImGui::Text("%.5f", k.lat);
                ImGui::TableNextColumn(); ImGui::Text("%.5f", k.lon);
                ImGui::TableNextColumn(); if (p.hasAscent) ImGui::Text("%+.1f", p.ascentMs); else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); if (k.kind != 0) ImGui::Text("%d", k.tempC); else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); if (k.kind != 0) ImGui::Text("%.2f", k.battV); else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); if (k.kind != 0) ImGui::Text("%d", k.sats); else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); ImGui::Text("%.1f", k.snrDb);
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)p.packets);
                ImGui::TableNextColumn(); ImGui::Text("%+.2f", k.offsetHz / 1e3);
            } else { ImGui::TableNextColumn(); ImGui::Text("%.1f", k.snrDb); }
            ImGui::TableNextColumn();
            if (!p.active) ImGui::PushStyleColor(ImGuiCol_Text, pal::grey());
            ImGui::TextUnformatted(heardText(p.lastHeardS).c_str());
            if (!p.active) ImGui::PopStyleColor();
        }
    }
    ImGui::EndTable();
}

void detailCard(const App& a) {
    const HorusTelemetry& t = a.rx.horus;
    const bool on = live(a);
    const HorusPayload* sp = on ? selected(t) : nullptr;
    auto kv = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", k); kvColumn(96 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    ImGui::PushStyleColor(ImGuiCol_Text, pal::heading());
    if (sp) ImGui::Text("%s  (%s)", sp->callsign.c_str(), kindName(sp->last.kind));
    else { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(on ? (t.state == 0 ? tr("waiting for samples") : tr("no payload decoded yet")) : tr("stopped")); ImGui::PopTextWrapPos(); }
    ImGui::PopStyleColor();
    if (!sp) return;
    const auto& k = sp->last;
    char b[160];
    if (t.centerHz > 0) snprintf(b, sizeof b, "%.4f MHz (%+.2f kHz)", (t.centerHz + k.offsetHz) / 1e6, k.offsetHz / 1e3); else snprintf(b, sizeof b, "%+.2f kHz from the dial", k.offsetHz / 1e3);
    kv(tr("frequency"), b);
    if (k.kind != 0) snprintf(b, sizeof b, "%d baud 4FSK, %.0f Hz spacing, %d bits corrected", k.baud, k.toneSpacingHz, k.corrected);
    else snprintf(b, sizeof b, "%d baud, %d data bits, %.0f Hz shift", k.baud, k.rttyBits, k.toneSpacingHz);
    kv(tr("signal"), b);
    snprintf(b, sizeof b, "%.1f dB in 3 kHz", k.snrDb); kv(tr("SNR"), b);
    if (k.payloadId >= 0) kv(tr("payload ID"), std::to_string(k.payloadId));
    snprintf(b, sizeof b, "%u at %02d:%02d:%02d UTC", k.seq, k.hour, k.minute, k.second); kv(tr("packet"), b);
    snprintf(b, sizeof b, "%.5f, %.5f", k.lat, k.lon); kv(tr("position"), b);
    snprintf(b, sizeof b, "%.0f m (highest %.0f m)", k.altM, sp->maxAltM); kv(tr("altitude"), b);
    if (sp->hasAscent) { snprintf(b, sizeof b, "%+.1f m/s", sp->ascentMs); kv(tr("climb"), b); }
    if (k.kind != 0) {
        snprintf(b, sizeof b, "%d km/h", k.speedKmh); kv(tr("speed"), b);
        kv(tr("satellites"), std::to_string(k.sats));
        snprintf(b, sizeof b, "%d C", k.tempC); kv(tr("temperature"), b);
        snprintf(b, sizeof b, "%.2f V", k.battV); kv(tr("battery"), b);
    }
    for (const auto& c : k.custom) kv(c.first.c_str(), c.second);
    if (receiverLocationSet() && !(k.lat == 0 && k.lon == 0)) {
        double mla, mlo, km, br;
        receiverLocation(mla, mlo);
        dect2::greatCircle(mla, mlo, k.lat, k.lon, km, br);
        const double el = std::atan2(k.altM / 1000.0 - km * km / (2 * 6371.0), std::max(0.01, km)) * 180 / 3.14159265358979;
        snprintf(b, sizeof b, "%.1f km at %.0f deg, elevation %.1f deg", km, br, el);
        kv(tr("from you"), b);
    }
    snprintf(b, sizeof b, "%llu, heard %s ago", (unsigned long long)sp->packets, heardText(sp->lastHeardS).c_str()); kv(tr("packets"), b);
    ImGui::Spacing();
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(k.sentence.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    if (ImGui::SmallButton(tr("copy sentence"))) ImGui::SetClipboardText(k.sentence.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("copy position"))) { snprintf(b, sizeof b, "%.6f, %.6f", k.lat, k.lon); ImGui::SetClipboardText(b); }
}

void packetLog(App& a) {
    const HorusTelemetry& t = a.rx.horus;
    if (!ImGui::BeginTable("##hr_log", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn(tr("SNR dB")); ImGui::TableSetupColumn(tr("kHz")); ImGui::TableSetupColumn(tr("Ch")); ImGui::TableSetupColumn(tr("Sentence"));
    ImGui::TableHeadersRow();
    if (live(a)) for (const auto& k : t.packets) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(kindName(k.kind));
        ImGui::TableNextColumn(); ImGui::Text("%.1f", k.snrDb);
        ImGui::TableNextColumn(); ImGui::Text("%+.2f", k.offsetHz / 1e3);
        ImGui::TableNextColumn(); ImGui::Text("%d", k.channel);
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(k.sentence.c_str()); ImGui::PopFont();
    }
    ImGui::EndTable();
}

void tablesEditor(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Payload IDs: one \"id, callsign\" per line (the format of horusdemodlib's payload_id_list.txt). Custom fields: the JSON of "
                                  "custom_field_list.json (struct characters < > B b H h f x; none, battery_5v_byte, divide_by_10, divide_by_100). "
                                  "Nothing is downloaded: paste the entries of the payloads you follow."));
    ImGui::PopTextWrapPos();
    const float h = std::max(80.f, (ImGui::GetContentRegionAvail().y - 3 * ImGui::GetFrameHeightWithSpacing()) / 2);
    ImGui::TextDisabled("%s", tr("Payload IDs"));
    ImGui::InputTextMultiline("##hids", S.ids, sizeof S.ids, ImVec2(-1, h * 0.7f));
    ImGui::TextDisabled("%s", tr("Custom fields (v2 bytes 21 to 29)"));
    ImGui::InputTextMultiline("##hfields", S.fields, sizeof S.fields, ImVec2(-1, h * 1.3f));
    if (ImGui::Button(tr("Apply"))) {
        plat::prefs().setS("horusIds", S.ids);
        plat::prefs().setS("horusFields", S.fields);
        if (a.engine.running()) pushSettings(a); else S.pushed = false;
        if (!a.engine.running()) { dect2::HorusReceiver probe; S.tableErr = probe.setTables(S.ids, S.fields); }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Defaults"))) {
        snprintf(S.ids, sizeof S.ids, "%s", dect2::horus::defaultIdList().c_str());
        snprintf(S.fields, sizeof S.fields, "%s", dect2::horus::defaultCustomFields().c_str());
    }
    if (!S.tableErr.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1), "%s", S.tableErr.c_str()); }
}

void tab(App& a) {
    loadState();
    subNav("horusv", S.viewMode, {"Map", "Table", "Packets", "IDs and fields"});
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    if (S.viewMode == 1) { payloadTable(a, false); return; }
    if (S.viewMode == 2) { packetLog(a); return; }
    if (S.viewMode == 3) { tablesEditor(a); return; }
    const float cw = std::min(320.f * gUi, W * 0.34f);
    ImGui::BeginChild("##horus_l", ImVec2(W - cw - 8 * gUi, H));
    mapView(a, ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##horus_r", ImVec2(0, H));
    detailCard(a);
    ImGui::EndChild();
}

void list(App& a) {
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see balloons")); ImGui::PopTextWrapPos(); return; }
    const HorusTelemetry& t = a.rx.horus;
    int act = 0;
    for (const auto& p : t.payloads) act += p.active ? 1 : 0;
    ImGui::TextDisabled(tr("%zu heard, %d active"), t.payloads.size(), act);
    payloadTable(a, true);
}

void receiver(App& a) {
    const HorusTelemetry& t = a.rx.horus;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("input"), "%.0f kS/s, dial %.4f MHz", t.inputRate / 1e3, t.centerHz / 1e6);
    kv(tr("packets"), "%llu decoded, %llu syncs, %llu failed the CRC", (unsigned long long)t.blocksOk, (unsigned long long)t.syncs, (unsigned long long)t.blocksBad);
    kv(tr("span search"), "%s, %d channels at most", t.wide ? tr("on") : tr("off"), t.maxChannels);
    ImGui::Spacing();
    if (ImGui::BeginTable("##hr_ch", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Channel")); ImGui::TableSetupColumn(tr("kHz")); ImGui::TableSetupColumn(tr("Decoding")); ImGui::TableSetupColumn(tr("Tones Hz"));
        ImGui::TableSetupColumn(tr("SNR dB")); ImGui::TableSetupColumn(tr("Packets / syncs"));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < t.channels.size(); i++) {
            const auto& c = t.channels[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%zu%s", i, c.dynamic ? "" : tr(" (dial)"));
            ImGui::TableNextColumn(); ImGui::Text("%+.2f", c.offsetHz / 1e3);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.mode == 1 ? tr("Horus Binary") : c.mode == 2 ? tr("RTTY") : tr("listening"));
            ImGui::TableNextColumn();
            if (c.mode == 2) ImGui::Text("%.0f / %.0f", c.rtty[0], c.rtty[1]);
            else if (c.ntones == 4) ImGui::Text("%.0f %.0f %.0f %.0f", c.tones[0], c.tones[1], c.tones[2], c.tones[3]);
            else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (c.packets) ImGui::Text("%.1f", c.snrDb); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", (unsigned long long)c.packets, (unsigned long long)c.syncs);
        }
        ImGui::EndTable();
    }
}

// the bottom row: altitude against time of the selected payload and the dial channel's spectrum with the tones it follows
void panels(App& a) {
    const HorusTelemetry& t = a.rx.horus;
    const bool on = live(a);
    const HorusPayload* s = on ? selected(t) : nullptr;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    const float w = std::max(120.f, (W - 3 * gap) / 2.f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    captionFit(w, "%s", tr("Altitude against time"));
    if (plt::BeginPlot("##hr_alt", ImVec2(w, plotH), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("minutes", "km", plt::AxisFlags_AutoFit, plt::AxisFlags_AutoFit);
        if (s && s->track.size() > 1) {
            std::vector<float> x, y;
            const double t0 = s->track.front().unixT;
            for (const auto& p : s->track) { x.push_back((float)((p.unixT - t0) / 60.0)); y.push_back(p.altM / 1000.f); }
            plt::PlotLine("alt", x.data(), y.data(), (int)x.size());
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(w, "%s", tr("Dial channel: spectrum and the tones followed"));
    if (plt::BeginPlot("##hr_spec", ImVec2(w, plotH), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("Hz", "dB", plt::AxisFlags_AutoFit, plt::AxisFlags_AutoFit);
        if (on && !t.spectrumDb.empty()) {
            std::vector<float> x(t.spectrumDb.size());
            for (size_t i = 0; i < x.size(); i++) x[i] = (float)(t.specLoHz + i * t.specStepHz);
            plt::PlotLine("spec", x.data(), t.spectrumDb.data(), (int)x.size());
            if (!t.channels.empty()) {
                const auto& c = t.channels[0];
                std::vector<double> tones;
                if (c.mode == 2) tones = {c.rtty[0], c.rtty[1]};
                else for (int k = 0; k < c.ntones; k++) tones.push_back(c.tones[k]);
                plt::Spec sp; sp.LineColor = ImVec4(0.95f, 0.70f, 0.30f, 0.8f);
                if (!tones.empty()) plt::PlotInfLines("tones", tones.data(), (int)tones.size(), sp);
            }
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const HorusTelemetry& t = a.rx.horus;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    int act = 0;
    for (const auto& p : t.payloads) act += p.active ? 1 : 0;
    bool tones = false;
    for (const auto& c : t.channels) tones = tones || c.ntones == 4 || c.rtty[1] > 0;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Tones"), !on ? 0 : tones ? 1 : 0); flowNext(12 * gUi);
    lamp(tr("Decoding"), !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro(tr("State"), run ? tr("starting") : tr("stopped")); return; }
    snprintf(b, sizeof b, "%d active of %zu", act, t.payloads.size()); ro(tr("Payloads"), b);
    snprintf(b, sizeof b, "%llu", (unsigned long long)t.blocksOk); ro(tr("Packets"), b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro(tr("SNR"), b);
    snprintf(b, sizeof b, "%zu", t.channels.size()); ro(tr("Channels"), b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Horus balloons";
    if (live(a)) l2 = dect2::horusSummary(a.rx.horus);
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("%s", tr("Frequency"));
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));
    static const struct { double mhz; const char* label; } kF[] = {{434.660, "434.660 MHz (Project Horus)"}, {434.650, "434.650 MHz"}, {434.550, "434.55 MHz (whole 434.2 - 434.9 span)"},
                                                                  {434.200, "434.200 MHz (Australia)"}, {437.600, "437.600 MHz (Poland)"}, {431.050, "431.050 MHz (California)"}};
    const char* cur = "custom frequency";
    for (const auto& f : kF) if (std::fabs(a.freqMhz - f.mhz) < 0.0005) cur = f.label;
    if (ImGui::BeginCombo("##hrband", cur)) {
        for (const auto& f : kF) if (ImGui::Selectable(f.label, std::fabs(a.freqMhz - f.mhz) < 0.0005)) { a.freqMhz = f.mhz; retune = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Each flight announces its frequency. The dial channel listens +-4 kHz around the dial;\nwith the span search on, every other balloon in the captured width gets a channel of its own."));
}

void decoder(App& a, bool&) {
    loadState();
    ImGui::TextDisabled("%s", tr("CHANNELS")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(80 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::SliderInt("##hrch", &S.maxCh, 1, 8)) { plat::prefs().setI("horusChannels", S.maxCh); if (a.engine.running()) a.engine.horus().setMaxChannels(S.maxCh); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How many balloons are followed at once (1: the dial channel only)."));
    sameLineIf(ImGui::CalcTextSize(tr("span search")).x + 30 * gUi, 8 * gUi);
    if (ImGui::Checkbox(tr("span search"), &S.wide)) { plat::prefs().setB("horusWide", S.wide); if (a.engine.running()) a.engine.horus().setWideSearch(S.wide); }
    sameLineIf(ImGui::CalcTextSize(tr("clear list")).x + 2 * ImGui::GetStyle().FramePadding.x, 8 * gUi);
    if (ImGui::SmallButton(tr("clear list"))) { a.engine.horus().clearResults(); S.sel.clear(); }
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* kinds[] = {"Binary v2", "Binary v1", "RTTY 100 Bd 7N2", "RTTY 300 Bd 8N2", "RTTY 50 Bd 7N2", "Three balloons"};
    int k = std::clamp(sc.modeOpt[0], 0, 5);
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::Combo("##hrkind", &k, kinds, 6)) { sc.modeOpt[0] = k; changed = true; }
    flowNext(); ImGui::TextDisabled("SNR"); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    float snr = (float)sc.snrDb;
    if (ImGui::SliderFloat("##hrsnr", &snr, -10.f, 30.f, "%.0f dB")) { sc.snrDb = snr; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("SNR of each balloon in 3 kHz. Horus Binary decodes down to about -6 dB, RTTY 100 Bd to about 0 dB."));
    flowNext(); ImGui::TextDisabled("CFO"); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    float cfo = (float)sc.cfoHz;
    if (ImGui::SliderFloat("##hrcfo", &cfo, -3000, 3000, "%.0f Hz")) { sc.cfoHz = cfo; changed = true; }
    flowNext();
    bool mirror = sc.modeOpt[2] == 1;
    if (ImGui::Checkbox(tr("mirrored (LSB)"), &mirror)) { sc.modeOpt[2] = mirror ? 1 : 0; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const HorusTelemetry& t = a.rx.horus;
    int act = 0;
    for (const auto& p : t.payloads) act += p.active ? 1 : 0;
    out.push_back({"PAYLOADS", "%.0f", (double)act, 0, 8, act > 0 ? 1 : 0});
    out.push_back({"SNR  dB (3 kHz)", "%.1f", (double)t.snrDb, -10, 30, t.snrDb >= 3 ? 1 : t.snrDb >= -4 ? 2 : 3});
    out.push_back({"PACKETS", "%.0f", (double)t.blocksOk, 0, 1000, t.blocksOk > 0 ? 1 : 0});
}

} // namespace

extern const ModeUi kHorusUi;
const ModeUi kHorusUi = {
    .sideTitle = "BALLOONS",
    .tabName = "Balloons",
    .tabIcon = Ic::Pulse,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};

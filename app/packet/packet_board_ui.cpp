// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APRS Weather and Messages views: the weather stations with their last values and a 24 h graph, the messages as conversations, the bulletins
// and weather service bulletins in their own list, and the user's callsign for "to me" (docs/APRS-BOARD-3.9.md).
#include "packet_board_ui.h"
#include "app.h"
#include "ui/kit/export.h"
#include "ui/kit/kit.h"
#include "dect2/geo/maidenhead.h"
#include "dect2/packet/packet_tel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace aprsboard {
namespace {

using dect2::AprsMessage;
using dect2::AprsMsgKind;
using dect2::AprsWx;
using dect2::PacketStation;

struct State {
    bool loaded = false;
    char call[16] = {};
    int graph = 0;                       // 0 temperature, 1 wind and gust, 2 pressure, 3 humidity, 4 rain
    int msgView = 0;                     // 0 conversations, 1 bulletins
    std::string thread;                  // the open conversation
    std::set<std::string> seen;          // "from>to#no#time" of the messages to me already opened
    bool onlyToMe = false;
};
State S;

void load() {
    if (S.loaded) return;
    S.loaded = true;
    const std::string c = plat::prefs().getS("packetMyCall", "");
    snprintf(S.call, sizeof S.call, "%s", c.c_str());
}

bool has(float v) { return !std::isnan(v); }

std::string val(float v, const char* fmt) {
    if (!has(v)) return "-";
    char b[32]; snprintf(b, sizeof b, fmt, v);
    return b;
}

std::string age(double s) {
    char b[24];
    if (s < 0) s = 0;
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

std::string sigClock(double s) {
    char b[24];
    const int t = std::max(0, (int)s);
    snprintf(b, sizeof b, "%02d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    return b;
}

std::string printable(const std::string& s) {
    std::string o = s;
    for (char& c : o) if ((unsigned char)c < 32 || (unsigned char)c == 127) c = '.';
    return o;
}

std::string msgId(const AprsMessage& m) { return m.from + ">" + m.to + "#" + m.msgNo + "#" + m.text; }

// the 24 h graph of one value (or two: wind and gust) of the selected station
void graph(const PacketStation& s, double now) {
    static const char* kG[] = {"Temperature", "Wind and gust", "Pressure", "Humidity", "Rain (1 h)"};
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::BeginCombo("##aprswxg", tr(kG[S.graph]))) {
        for (int i = 0; i < 5; i++) if (ImGui::Selectable(tr(kG[i]), S.graph == i)) S.graph = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s  %zu %s", s.call.c_str(), s.wxHist.size(), tr("reports, the last 24 h"));
    auto pick = [](const AprsWx& w, int which, int series) -> float {
        switch (which) {
        case 0: return w.tempC;
        case 1: return series == 0 ? w.windKmh : w.gustKmh;
        case 2: return w.pressureHpa;
        case 3: return w.humidityPct;
        default: return w.rain1hMm;
        }
    };
    static const char* kUnit[] = {"\xC2\xB0""C", "km/h", "hPa", "%", "mm"};
    const int nSeries = S.graph == 1 ? 2 : 1;
    double lo = 1e30, hi = -1e30; int n = 0;
    for (const AprsWx& w : s.wxHist) for (int k = 0; k < nSeries; k++) { const float v = pick(w, S.graph, k); if (has(v)) { lo = std::min(lo, (double)v); hi = std::max(hi, (double)v); n++; } }
    const ImVec2 sz(ImGui::GetContentRegionAvail().x, std::max(80.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##aprswxplot", sz);
    if (n < 1) { ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi), ImGui::GetColorU32(ImGuiCol_TextDisabled), tr("This station has not sent that value.")); return; }
    if (S.graph == 1 || S.graph == 4) lo = std::min(lo, 0.0);
    double pad = (hi - lo) * 0.1; if (pad < 0.5) pad = 0.5;
    lo -= pad; hi += pad;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 cText = ImGui::GetColorU32(ImGuiCol_TextDisabled), cGrid = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 col[2] = {IM_COL32(240, 150, 70, 255), IM_COL32(90, 180, 240, 255)};
    const float padL = 50 * gUi, padR = 8 * gUi, padT = 6 * gUi, padB = 18 * gUi;
    const ImVec2 q0(p0.x + padL, p0.y + padT), q1(p0.x + sz.x - padR, p0.y + sz.y - padB);
    dl->AddRect(q0, q1, cGrid);
    const double t1 = now, t0 = now - dect2::kAprsWxHistorySec;
    auto X = [&](double t) { return q0.x + (float)((t - t0) / (t1 - t0)) * (q1.x - q0.x); };
    auto Y = [&](double v) { return q1.y - (float)((v - lo) / (hi - lo)) * (q1.y - q0.y); };
    for (int g = 0; g <= 4; g++) {
        const double v = lo + (hi - lo) * g / 4.0;
        char b[24]; snprintf(b, sizeof b, "%.1f", v);
        dl->AddText(ImVec2(q0.x - ImGui::CalcTextSize(b).x - 4 * gUi, Y(v) - ImGui::GetFontSize() * 0.5f), cText, b);
    }
    for (int h = 24; h >= 0; h -= 6) {   // hours ago along the bottom
        char b[16]; snprintf(b, sizeof b, h ? "-%d h" : "now", h);
        const float x = X(now - h * 3600.0);
        dl->AddLine(ImVec2(x, q1.y), ImVec2(x, q1.y + 3 * gUi), cGrid);
        dl->AddText(ImVec2(std::min(x - ImGui::CalcTextSize(b).x * 0.5f, q1.x - ImGui::CalcTextSize(b).x), q1.y + 3 * gUi), cText, b);
    }
    for (int k = 0; k < nSeries; k++) {
        bool prev = false; ImVec2 pp;
        for (size_t i = 0; i < s.wxHist.size(); i++) {
            const AprsWx& w = s.wxHist[i];
            const float v = pick(w, S.graph, k);
            // a gap of more than 3 report steps: the line breaks, it does not bridge the missing hours
            if (!has(v) || w.timeSec < t0) { prev = false; continue; }
            const ImVec2 q(X(w.timeSec), Y(v));
            if (prev && i > 0 && w.timeSec - s.wxHist[i - 1].timeSec < 4 * dect2::kAprsWxStepSec) dl->AddLine(pp, q, col[k], 1.8f * gUi);
            dl->AddCircleFilled(q, 2.f * gUi, col[k]);
            pp = q; prev = true;
        }
    }
    char lg[96];
    snprintf(lg, sizeof lg, "%s (%s)%s", tr(kG[S.graph]), kUnit[S.graph], S.graph == 1 ? "   orange: wind, blue: gust" : "");
    dl->AddText(ImVec2(q0.x + 6 * gUi, q0.y + 2 * gUi), cText, lg);
}

} // namespace

const char* myCall(App& a) {
    load();
    if (S.call[0]) return S.call;
    return a.feeds.aprsCall.c_str();
}

int unreadToMe(App& a) {
    int n = 0;
    for (const AprsMessage& m : a.rx.packet.messages) if (m.toMe && !S.seen.count(msgId(m))) n++;
    return n;
}

void weatherView(App& a, std::string& sel) {
    load();
    const dect2::PacketTelemetry& t = a.rx.packet;
    std::vector<const PacketStation*> wx;
    for (const PacketStation& s : t.stations) if (s.hasWx) wx.push_back(&s);
    {
        static std::string note;
        resexp::Table x; x.cols = {"callsign", "lat", "lon", "temp_c", "wind_dir_deg", "wind_kmh", "gust_kmh", "rain_1h_mm", "rain_24h_mm", "rain_midnight_mm", "humidity_pct", "pressure_hpa"};
        auto n = [](float v, int d) { return has(v) ? resexp::num(v, d) : std::string(); };
        for (const PacketStation* s : wx)
            x.add({s->call, s->hasPos ? resexp::num(s->lat, 6) : "", s->hasPos ? resexp::num(s->lon, 6) : "", n(s->wx.tempC, 1), n(s->wx.windDirDeg, 0), n(s->wx.windKmh, 1),
                   n(s->wx.gustKmh, 1), n(s->wx.rain1hMm, 1), n(s->wx.rain24hMm, 1), n(s->wx.rainMidnightMm, 1), n(s->wx.humidityPct, 0), n(s->wx.pressureHpa, 1)});
        kit::exportBar(x, "aprs-weather", note, "pkwx");
    }
    if (wx.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("No weather station heard yet. APRS weather stations (the blue-green ones on the map) appear here with their temperature, wind, rain, humidity and pressure."));
        ImGui::PopTextWrapPos();
        return;
    }
    const PacketStation* cur = nullptr;
    for (const PacketStation* s : wx) if (s->call == sel) cur = s;
    if (!cur) cur = wx.front();
    const float graphH = 190 * gUi;
    if (ImGui::BeginTable("##pkwx_t", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, std::max(80.f * gUi, ImGui::GetContentRegionAvail().y - graphH)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Station"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Last heard"));
        ImGui::TableSetupColumn("\xC2\xB0""C");
        ImGui::TableSetupColumn(tr("Wind km/h"));
        ImGui::TableSetupColumn(tr("Gust"));
        ImGui::TableSetupColumn(tr("Rain 1 h / 24 h / today mm"));
        ImGui::TableSetupColumn(tr("Humidity %"));
        ImGui::TableSetupColumn(tr("Pressure hPa"));
        ImGui::TableSetupColumn(tr("km"));
        ImGui::TableHeadersRow();
        double rl = 0, ro = 0; receiverLocation(rl, ro);
        for (const PacketStation* s : wx) {
            const AprsWx& w = s->wx;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(s->call.c_str());
            if (ImGui::Selectable(s->call.c_str(), s == cur, ImGuiSelectableFlags_SpanAllColumns)) sel = s->call;
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(age(t.timeSec - s->lastHeardSec).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(val(w.tempC, "%.1f").c_str());
            ImGui::TableNextColumn();
            if (has(w.windKmh)) ImGui::Text("%s %.0f", dect2::aprsCompass(w.windDirDeg), w.windKmh); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(val(w.gustKmh, "%.0f").c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s / %s / %s", val(w.rain1hMm, "%.1f").c_str(), val(w.rain24hMm, "%.1f").c_str(), val(w.rainMidnightMm, "%.1f").c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(val(w.humidityPct, "%.0f").c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(val(w.pressureHpa, "%.1f").c_str());
            ImGui::TableNextColumn();
            if (s->hasPos && (rl != 0 || ro != 0)) { double km, brg; dect2::greatCircle(rl, ro, s->lat, s->lon, km, brg); ImGui::Text("%.0f", km); } else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    graph(*cur, t.timeSec);
}

void messagesView(App& a) {
    load();
    const dect2::PacketTelemetry& t = a.rx.packet;
    // the user's callsign: messages to it are marked and raise the aprs.message event with to_me (the "APRS message to me" automation)
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("My callsign")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::InputTextWithHint("##pkmycall", a.feeds.aprsCall.empty() ? "LY2XX" : a.feeds.aprsCall.c_str(), S.call, sizeof S.call, ImGuiInputTextFlags_CharsUppercase))
        plat::prefs().setS("packetMyCall", S.call);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Messages to this callsign are marked \"to you\". Without an SSID every SSID of it counts. Empty: the APRS-IS login callsign (Feeds). Switch on the \"APRS message to me\" automation for a notification."));
    const std::string want = myCall(a);   // the packet tab's tick hands it to the receiver
    {
        static std::string note;
        resexp::Table x; x.cols = {"signal_time", "from", "to", "kind", "msgno", "text", "acked", "rejected", "to_me", "copies"};
        for (const AprsMessage& m : t.messages)
            x.add({sigClock(m.timeSec), m.from, m.to, dect2::aprsMsgKindName(m.kind), m.msgNo, printable(m.text), m.acked ? "yes" : "", m.rejected ? "yes" : "", m.toMe ? "yes" : "", std::to_string(m.copies)});
        kit::exportBar(x, "aprs-messages", note, "pkmsgx");
    }
    ImGui::SameLine(0, 12 * gUi);
    if (ImGui::RadioButton(tr("Conversations"), S.msgView == 0)) S.msgView = 0;
    ImGui::SameLine();
    int nb = 0;
    for (const AprsMessage& m : t.messages) if (m.kind != AprsMsgKind::Message) nb++;
    char bl[64]; snprintf(bl, sizeof bl, "%s (%d)###pkbln", tr("Bulletins"), nb);
    if (ImGui::RadioButton(bl, S.msgView == 1)) S.msgView = 1;
    if (S.msgView == 1) {
        if (!nb) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("No bulletin heard yet. Bulletins (BLN0-9), announcements (BLNA-Z), group bulletins and weather service bulletins (NWS, SKY, CWA, BOM) are listed here.")); ImGui::PopTextWrapPos(); return; }
        if (!ImGui::BeginTable("##pkbl", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Heard")); ImGui::TableSetupColumn(tr("From")); ImGui::TableSetupColumn(tr("Id")); ImGui::TableSetupColumn(tr("Kind"));
        ImGui::TableSetupColumn(tr("Text"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (auto it = t.messages.rbegin(); it != t.messages.rend(); ++it) {
            const AprsMessage& m = *it;
            if (m.kind == AprsMsgKind::Message) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(age(t.timeSec - m.timeSec).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s %s, %u %s", tr("signal time"), sigClock(m.timeSec).c_str(), (unsigned)m.copies, tr("copies"));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.from.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.to.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(tr(dect2::aprsMsgKindName(m.kind)));
            ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(printable(m.text).c_str()); ImGui::PopTextWrapPos();
        }
        ImGui::EndTable();
        return;
    }
    // conversations: one row per pair of stations, newest first; the open one's messages on the right
    struct Thread { std::string key; double last = 0; int n = 0, toMe = 0; };
    std::map<std::string, Thread> th;
    for (const AprsMessage& m : t.messages) {
        if (m.kind != AprsMsgKind::Message) continue;
        Thread& x = th[dect2::aprsThreadKey(m)];
        x.key = dect2::aprsThreadKey(m);
        x.last = std::max(x.last, m.lastSec); x.n++;
        if (m.toMe && !S.seen.count(msgId(m))) x.toMe++;
    }
    std::vector<const Thread*> order;
    for (const auto& kv : th) if (!S.onlyToMe || kv.second.toMe || kv.first.find(want) != std::string::npos) order.push_back(&kv.second);
    std::sort(order.begin(), order.end(), [](const Thread* a, const Thread* b) { return a->last > b->last; });
    ImGui::SameLine(0, 12 * gUi);
    ImGui::Checkbox(tr("Only mine"), &S.onlyToMe);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Only the conversations with your callsign."));
    if (order.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("No APRS message heard yet. Messages between stations appear here as conversations, with their acknowledgements.")); ImGui::PopTextWrapPos(); return; }
    if (S.thread.empty() || !th.count(S.thread)) S.thread = order.front()->key;
    const float listW = std::min(260.f * gUi, ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::BeginChild("##pkth", ImVec2(listW, 0), ImGuiChildFlags_Borders);
    for (const Thread* x : order) {
        char lb[96]; snprintf(lb, sizeof lb, "%s  (%d)%s###%s", x->key.c_str(), x->n, x->toMe ? "  *" : "", x->key.c_str());
        if (x->toMe) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.82f, 0.35f, 1.f));
        if (ImGui::Selectable(lb, S.thread == x->key)) S.thread = x->key;
        if (x->toMe) { ImGui::PopStyleColor(); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d %s", x->toMe, tr("new to you")); }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##pkthm", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted(S.thread.c_str());
    ImGui::Separator();
    for (const AprsMessage& m : t.messages) {
        if (m.kind != AprsMsgKind::Message || dect2::aprsThreadKey(m) != S.thread) continue;
        if (m.toMe) S.seen.insert(msgId(m));
        std::string st;
        if (m.acked) st = tr("acknowledged");
        else if (m.rejected) st = tr("rejected");
        else if (!m.msgNo.empty()) st = tr("no ack heard");
        ImGui::TextDisabled("%s  %s > %s%s", age(t.timeSec - m.timeSec).c_str(), m.from.c_str(), m.to.c_str(), m.toMe ? "  (to you)" : "");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s %s, %u %s%s%s", tr("signal time"), sigClock(m.timeSec).c_str(), (unsigned)m.copies, tr("copies"), m.msgNo.empty() ? "" : ", {", m.msgNo.empty() ? "" : (m.msgNo + "}").c_str());
        ImGui::Indent(10 * gUi);
        ImGui::PushTextWrapPos(0);
        if (m.toMe) ImGui::TextColored(ImVec4(1.f, 0.85f, 0.45f, 1.f), "%s", printable(m.text).c_str());
        else ImGui::TextUnformatted(printable(m.text).c_str());
        ImGui::PopTextWrapPos();
        if (!st.empty()) { ImGui::SameLine(); ImGui::TextDisabled("[%s]", st.c_str()); }
        ImGui::Unindent(10 * gUi);
    }
    ImGui::EndChild();
}

} // namespace aprsboard
